#include "memory.h"

#include <windows.h>

namespace s3n {

uint32_t RangeAllocator::Alloc(uint32_t size, uint32_t align, uint32_t limit) {
    if (size == 0) return kNone;
    std::lock_guard lock(mutex_);
    for (auto it = free_.begin(); it != free_.end(); ++it) {
        uint64_t start = it->first, end = uint64_t(it->first) + it->second;
        uint64_t address = (start + align - 1) & ~uint64_t(align - 1);
        if (address + size > end || address + size > limit) continue;
        // Split the free range around the allocation.
        free_.erase(it);
        if (address > start) free_[uint32_t(start)] = uint32_t(address - start);
        if (address + size < end) free_[uint32_t(address + size)] = uint32_t(end - address - size);
        used_[uint32_t(address)] = size;
        return uint32_t(address);
    }
    return kNone;
}

uint32_t RangeAllocator::Free(uint32_t address) {
    std::lock_guard lock(mutex_);
    auto used = used_.find(address);
    if (used == used_.end()) return 0;
    uint32_t start = address, size = used->second, freed = size;
    used_.erase(used);
    // Merge with the free ranges on either side.
    auto next = free_.lower_bound(start);
    if (next != free_.end() && start + size == next->first) {
        size += next->second;
        next = free_.erase(next);
    }
    if (next != free_.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == start) {
            start = prev->first;
            size += prev->second;
            free_.erase(prev);
        }
    }
    free_[start] = size;
    return freed;
}

uint32_t RangeAllocator::Available() {
    std::lock_guard lock(mutex_);
    uint32_t total = 0;
    for (const auto& [start, size] : free_) total += size;
    return total;
}

// The 4 GB space starts as one placeholder (Windows 10 1803+). The three physical
// windows are split off and each replaced by a view of one 512 MB section, so they alias;
// the rest becomes an ordinary reservation that's committed piece by piece.
bool GuestMemory::Reserve(std::string& error) {
    using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
    using MapViewOfFile3Fn =
        PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
    HMODULE kernelBase = GetModuleHandleW(L"kernelbase.dll");
    auto virtualAlloc2 = reinterpret_cast<VirtualAlloc2Fn>(GetProcAddress(kernelBase, "VirtualAlloc2"));
    auto mapViewOfFile3 = reinterpret_cast<MapViewOfFile3Fn>(GetProcAddress(kernelBase, "MapViewOfFile3"));
    auto fail = [&](const char* what) {
        error = std::string(what) + " failed (Windows error " + std::to_string(GetLastError()) + ")";
        return false;
    };
    if (!virtualAlloc2 || !mapViewOfFile3) {
        error = "this version of Windows can't alias memory (needs Windows 10 1803 or later)";
        return false;
    }

    HANDLE process = GetCurrentProcess();
    base_ = static_cast<uint8_t*>(
        virtualAlloc2(process, nullptr, 1ull << 32, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
    if (!base_) return fail("reserving 4 GB");
    for (uint32_t window : {kPhysicalWindows[0], kPhysicalWindows[1]})
        if (!VirtualFree(base_ + window, kPhysicalSize, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
            return fail("splitting the physical windows");
    if (!virtualAlloc2(process, base_, kPhysicalWindows[0], MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_NOACCESS,
                       nullptr, 0))
        return fail("reserving virtual memory");

    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT, 0, kPhysicalSize,
                                        nullptr);
    if (!section) return fail("creating 512 MB of physical memory");
    for (uint32_t window : kPhysicalWindows)
        if (!mapViewOfFile3(section, process, base_ + window, 0, kPhysicalSize, MEM_REPLACE_PLACEHOLDER,
                            PAGE_READWRITE, nullptr, 0))
            return fail("mapping physical memory");
    return Commit(kSystemBase, kSystemSize) || fail("committing system memory");
}

bool GuestMemory::Commit(uint32_t address, uint32_t size) {
    if (size == 0 || address >= kPhysicalWindows[0]) return true;
    return VirtualAlloc(base_ + address, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

uint32_t GuestMemory::AllocSystem(uint32_t size, uint32_t align) {
    std::lock_guard lock(systemMutex_);
    uint32_t address = (systemNext_ + align - 1) & ~(align - 1);
    if (uint64_t(address) + size > uint64_t(kSystemBase) + kSystemSize) return 0;
    systemNext_ = address + size;
    return address;  // committed pages start zeroed
}

uint32_t GuestMemory::ReserveVirtual(uint32_t size, uint32_t page) {
    std::lock_guard lock(virtualMutex_);
    bool small = page < 0x10000;
    uint32_t& next = small ? smallPageNext_ : virtualNext_;
    uint32_t address = (next + page - 1) & ~(page - 1);
    if (uint64_t(address) + size > (small ? kSmallPageEnd : kVirtualEnd)) return 0;
    next = address + size;
    return address;
}

uint32_t GuestMemory::AllocPhysical(uint32_t size, uint32_t align, uint32_t limit, bool smallPageWindow) {
    if (!smallPageWindow) return physical_.Alloc(size, align, limit);
    uint32_t offset = physical_.Alloc(size + kSmallPageWindowShift, align, limit);
    if (offset != kNoPhysical) {
        std::lock_guard lock(shiftedMutex_);
        shifted_[offset + kSmallPageWindowShift] = offset + kSmallPageWindowShift + size;
    }
    return offset;
}

bool GuestMemory::FreePhysical(uint32_t offset) {
    {
        std::lock_guard lock(shiftedMutex_);
        shifted_.erase(offset + kSmallPageWindowShift);
    }
    return physical_.Free(offset) != 0;
}

uint32_t GuestMemory::ConsolePhysical(uint32_t address) const {
    uint32_t offset = address & (kPhysicalSize - 1);
    return address >= kPhysicalWindows[2] ? (offset + kSmallPageWindowShift) & (kPhysicalSize - 1) : offset;
}

uint8_t* GuestMemory::Physical(uint32_t gpuAddress) {
    uint32_t address = gpuAddress & (kPhysicalSize - 1);
    {
        std::lock_guard lock(shiftedMutex_);
        auto it = shifted_.upper_bound(address);
        if (it != shifted_.begin() && address < std::prev(it)->second) address -= kSmallPageWindowShift;
    }
    return base_ + kPhysicalWindows[0] + address;
}

uint32_t GuestMemory::AllocStack(uint32_t size) {
    uint32_t address = stacks_.Alloc(size, 0x10000);
    if (address == RangeAllocator::kNone) return 0;
    if (!Commit(address, size)) {
        stacks_.Free(address);
        return 0;
    }
    return address;
}

}  // namespace s3n
