#pragma once

#include <cstdint>
#include <map>
#include <mutex>

namespace s3n {

// Big-endian access to guest memory from host code.
inline uint32_t LoadBE32(const uint8_t* base, uint32_t address) {
    return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + address));
}
inline void StoreBE32(uint8_t* base, uint32_t address, uint32_t value) {
    *reinterpret_cast<uint32_t*>(base + address) = __builtin_bswap32(value);
}

// The guest's 4 GB address space, mapped 1:1 at base(). Only ranges we hand out are
// committed, so a stray guest access faults (and gets reported) instead of reading zeros.
class GuestMemory {
public:
    // Runtime-owned objects (processor blocks, threads, TLS, stacks, kernel variables).
    static constexpr uint32_t kSystemBase = 0x7F000000;
    static constexpr uint32_t kSystemSize = 0x00F00000;
    // Title virtual memory (NtAllocateVirtualMemory), below the runtime's area.
    static constexpr uint32_t kVirtualBase = 0x40000000;
    static constexpr uint32_t kVirtualEnd = kSystemBase;

    bool Reserve();
    uint8_t* base() const { return base_; }
    bool Commit(uint32_t address, uint32_t size);

    bool Contains(uintptr_t host) const {
        return host >= reinterpret_cast<uintptr_t>(base_) && host - reinterpret_cast<uintptr_t>(base_) < (1ull << 32);
    }
    uint32_t ToGuest(uintptr_t host) const { return uint32_t(host - reinterpret_cast<uintptr_t>(base_)); }

    // Zeroed system memory that lives as long as the process. Returns 0 when full.
    uint32_t AllocSystem(uint32_t size, uint32_t align = 16);

    // Reserves size bytes of title virtual memory aligned to `page` (not committed).
    // Returns 0 when the range is exhausted. Never reused yet.
    uint32_t ReserveVirtual(uint32_t size, uint32_t page);

    // The console's 512 MB of physical memory, handed out first-fit from 1 MB up (so no
    // allocation looks like a null pointer). Returns the physical address, or kNoPhysical.
    static constexpr uint32_t kPhysicalSize = 0x20000000;
    static constexpr uint32_t kPhysicalStart = 0x00100000;
    static constexpr uint32_t kNoPhysical = 0xFFFFFFFF;
    uint32_t AllocPhysical(uint32_t size, uint32_t align, uint32_t limit);
    bool FreePhysical(uint32_t address);
    uint32_t PhysicalAvailable();

private:
    uint8_t* base_ = nullptr;
    uint32_t systemNext_ = kSystemBase;
    uint32_t virtualNext_ = kVirtualBase;
    std::mutex physicalMutex_;
    std::map<uint32_t, uint32_t> physicalFree_{{kPhysicalStart, kPhysicalSize - kPhysicalStart}};  // start -> size
    std::map<uint32_t, uint32_t> physicalUsed_;                                                    // start -> size
};

}  // namespace s3n
