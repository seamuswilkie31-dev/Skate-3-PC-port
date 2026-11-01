#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace s3n {

// Big-endian access to guest memory from host code.
inline uint32_t LoadBE32(const uint8_t* base, uint32_t address) {
    return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + address));
}
inline void StoreBE32(uint8_t* base, uint32_t address, uint32_t value) {
    *reinterpret_cast<uint32_t*>(base + address) = __builtin_bswap32(value);
}
inline uint64_t LoadBE64(const uint8_t* base, uint32_t address) {
    return __builtin_bswap64(*reinterpret_cast<const uint64_t*>(base + address));
}
inline void StoreBE64(uint8_t* base, uint32_t address, uint64_t value) {
    *reinterpret_cast<uint64_t*>(base + address) = __builtin_bswap64(value);
}

// First-fit allocator over one range of addresses, with neighbouring frees merged.
// Thread-safe. Hands out addresses only; committing memory is the caller's job.
class RangeAllocator {
public:
    static constexpr uint32_t kNone = 0xFFFFFFFF;
    RangeAllocator(uint32_t start, uint32_t size) : free_{{start, size}} {}
    // Returns the address, or kNone. Allocations end at or before `limit` (exclusive).
    uint32_t Alloc(uint32_t size, uint32_t align, uint32_t limit = 0xFFFFFFFF);
    // Returns the allocation's size, or 0 for an address that wasn't allocated.
    uint32_t Free(uint32_t address);
    uint32_t Available();

private:
    std::mutex mutex_;
    std::map<uint32_t, uint32_t> free_;  // start -> size
    std::map<uint32_t, uint32_t> used_;  // start -> size
};

// The guest's 4 GB address space, mapped 1:1 at base(). Only ranges we hand out are
// committed, so a stray guest access faults (and gets reported) instead of reading zeros.
//
// Like the console, the bottom 64 KB stay unmapped and the kernel's own allocations sit
// just above them. Title code relies on that: Skate 3 reads through a null pointer at
// offset 0x12160 during start-up, which only works if that memory is mapped.
class GuestMemory {
public:
    // Runtime-owned objects (kernel objects and variables).
    static constexpr uint32_t kSystemBase = 0x00010000;
    static constexpr uint32_t kSystemSize = 0x01000000;
    // Title virtual memory (NtAllocateVirtualMemory): 4 KB pages low, 64 KB pages high.
    static constexpr uint32_t kSmallPageBase = kSystemBase + kSystemSize;
    static constexpr uint32_t kSmallPageEnd = 0x40000000;
    static constexpr uint32_t kVirtualBase = 0x40000000;
    static constexpr uint32_t kVirtualEnd = 0x70000000;
    // Thread stacks with each thread's processor block and TLS, reused as threads exit.
    static constexpr uint32_t kStackBase = 0x70000000;
    static constexpr uint32_t kStackEnd = 0x7F000000;

    // Reserves the 4 GB space and maps physical memory into its windows. On failure,
    // `error` says why.
    bool Reserve(std::string& error);
    uint8_t* base() const { return base_; }
    // Commits title or runtime memory; physical windows are always committed.
    bool Commit(uint32_t address, uint32_t size);

    bool Contains(uintptr_t host) const {
        return host >= reinterpret_cast<uintptr_t>(base_) && host - reinterpret_cast<uintptr_t>(base_) < (1ull << 32);
    }
    uint32_t ToGuest(uintptr_t host) const { return uint32_t(host - reinterpret_cast<uintptr_t>(base_)); }

    // Zeroed system memory that lives as long as the process. Returns 0 when full.
    uint32_t AllocSystem(uint32_t size, uint32_t align = 16);

    // Reserves size bytes of title virtual memory in the range for `page` (4 KB or 64 KB),
    // aligned to it, not committed. Returns 0 when the range is exhausted. Never reused yet.
    uint32_t ReserveVirtual(uint32_t size, uint32_t page);

    // Committed (not zeroed) memory in the stack range. Returns 0 when it's full.
    uint32_t AllocStack(uint32_t size);
    void FreeStack(uint32_t address) { stacks_.Free(address); }

    // The console's 512 MB of physical memory, handed out first-fit from 1 MB up (so no
    // allocation looks like a null pointer). It's one block of host memory seen through
    // all three windows (0xA0000000 64 KB pages, 0xC0000000 16 MB, 0xE0000000 4 KB), so a
    // write through one shows in the others; it's always committed.
    //
    // On the console the 4 KB window is shifted: 0xE0000000 + x is physical x + 0x1000,
    // and titles compute GPU addresses that way. Windows maps views on 64 KB boundaries
    // only, so here every window starts at offset 0 instead, and the shift is applied
    // to addresses: a 4 KB-window allocation keeps a spare page after it (so its shifted
    // range overlaps nothing else) and GPU addresses inside it are shifted back.
    static constexpr uint32_t kPhysicalSize = 0x20000000;
    static constexpr uint32_t kPhysicalStart = 0x00100000;
    static constexpr uint32_t kNoPhysical = RangeAllocator::kNone;
    static constexpr uint32_t kPhysicalWindows[3] = {0xA0000000, 0xC0000000, 0xE0000000};
    static constexpr uint32_t kSmallPageWindowShift = 0x1000;
    // Returns the offset into physical memory (window + offset is the CPU address), or
    // kNoPhysical. `smallPageWindow`: for the 4 KB window.
    uint32_t AllocPhysical(uint32_t size, uint32_t align, uint32_t limit, bool smallPageWindow);
    bool FreePhysical(uint32_t offset);
    uint32_t PhysicalAvailable() { return physical_.Available(); }
    // The physical address the console gives a CPU address in a physical window
    // (MmGetPhysicalAddress), and the host memory behind a GPU (physical) address.
    uint32_t ConsolePhysical(uint32_t address) const;
    uint8_t* Physical(uint32_t gpuAddress);

private:
    uint8_t* base_ = nullptr;
    std::mutex systemMutex_;
    uint32_t systemNext_ = kSystemBase;
    std::mutex virtualMutex_;
    uint32_t smallPageNext_ = kSmallPageBase;
    uint32_t virtualNext_ = kVirtualBase;
    RangeAllocator stacks_{kStackBase, kStackEnd - kStackBase};
    RangeAllocator physical_{kPhysicalStart, kPhysicalSize - kPhysicalStart};
    std::mutex shiftedMutex_;
    std::map<uint32_t, uint32_t> shifted_;  // 4 KB-window allocations: GPU start -> GPU end
};

}  // namespace s3n
