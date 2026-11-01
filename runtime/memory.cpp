#include "memory.h"

#include <windows.h>

namespace s3n {

bool GuestMemory::Reserve() {
    base_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, 1ull << 32, MEM_RESERVE, PAGE_NOACCESS));
    return base_ && Commit(kSystemBase, kSystemSize);
}

bool GuestMemory::Commit(uint32_t address, uint32_t size) {
    return size == 0 || VirtualAlloc(base_ + address, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

uint32_t GuestMemory::AllocSystem(uint32_t size, uint32_t align) {
    uint32_t address = (systemNext_ + align - 1) & ~(align - 1);
    if (uint64_t(address) + size > uint64_t(kSystemBase) + kSystemSize) return 0;
    systemNext_ = address + size;
    return address;  // committed pages start zeroed
}

uint32_t GuestMemory::AllocPhysical(uint32_t size, uint32_t align, uint32_t limit) {
    if (size == 0) return kNoPhysical;
    std::lock_guard lock(physicalMutex_);
    for (auto it = physicalFree_.begin(); it != physicalFree_.end(); ++it) {
        uint64_t start = it->first, end = uint64_t(it->first) + it->second;
        uint64_t address = (start + align - 1) & ~uint64_t(align - 1);
        if (address + size > end || address + size > limit) continue;
        // Split the free range around the allocation.
        physicalFree_.erase(it);
        if (address > start) physicalFree_[uint32_t(start)] = uint32_t(address - start);
        if (address + size < end) physicalFree_[uint32_t(address + size)] = uint32_t(end - address - size);
        physicalUsed_[uint32_t(address)] = size;
        return uint32_t(address);
    }
    return kNoPhysical;
}

bool GuestMemory::FreePhysical(uint32_t address) {
    std::lock_guard lock(physicalMutex_);
    auto used = physicalUsed_.find(address);
    if (used == physicalUsed_.end()) return false;
    uint32_t start = address, size = used->second;
    physicalUsed_.erase(used);
    // Merge with the free ranges on either side.
    auto next = physicalFree_.lower_bound(start);
    if (next != physicalFree_.end() && start + size == next->first) {
        size += next->second;
        next = physicalFree_.erase(next);
    }
    if (next != physicalFree_.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == start) {
            start = prev->first;
            size += prev->second;
            physicalFree_.erase(prev);
        }
    }
    physicalFree_[start] = size;
    return true;
}

uint32_t GuestMemory::PhysicalAvailable() {
    std::lock_guard lock(physicalMutex_);
    uint32_t total = 0;
    for (const auto& [start, size] : physicalFree_) total += size;
    return total;
}

uint32_t GuestMemory::ReserveVirtual(uint32_t size, uint32_t page) {
    uint32_t address = (virtualNext_ + page - 1) & ~(page - 1);
    if (uint64_t(address) + size > kVirtualEnd) return 0;
    virtualNext_ = address + size;
    return address;
}

}  // namespace s3n
