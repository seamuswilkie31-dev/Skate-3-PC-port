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

bool GuestMemory::Reserve() {
    base_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, 1ull << 32, MEM_RESERVE, PAGE_NOACCESS));
    return base_ && Commit(kSystemBase, kSystemSize);
}

bool GuestMemory::Commit(uint32_t address, uint32_t size) {
    return size == 0 || VirtualAlloc(base_ + address, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
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
