// Kernel memory functions.

#include "kernel/kernel.h"
#include "memory.h"

#include <algorithm>

namespace s3n::kernel {
namespace {

constexpr uint32_t kMemCommit = 0x00001000;
constexpr uint32_t kMemReserve = 0x00002000;
constexpr uint32_t kMemLargePages = 0x20000000;  // 64 KB pages
constexpr uint32_t kMem16MbPages = 0x80000000;
constexpr uint32_t kStatusBufferTooSmall = 0xC0000023;

// Physical memory is visible at three virtual windows, by page size.
constexpr uint32_t kWindow64K = 0xA0000000;
constexpr uint32_t kWindow16M = 0xC0000000;
constexpr uint32_t kWindow4K = 0xE0000000;

uint32_t RoundUp(uint32_t value, uint32_t page) { return (value + page - 1) & ~(page - 1); }

}  // namespace
}  // namespace s3n::kernel

using namespace s3n;
using namespace s3n::kernel;

// NTSTATUS NtAllocateVirtualMemory(void** base, size_t* size, u32 type, u32 protect, u32 debug)
// Reserves and/or commits title memory. *base == 0 picks an address; both *base and *size
// come back rounded to the page size.
PPC_FUNC(__imp__NtAllocateVirtualMemory) {
    Trace(ctx, "NtAllocateVirtualMemory", true);
    GuestMemory& memory = Memory();
    uint32_t basePtr = ctx.r3.u32, sizePtr = ctx.r4.u32, type = ctx.r5.u32;
    uint32_t page = (type & kMemLargePages) ? 0x10000 : 0x1000;
    uint32_t address = LoadBE32(base, basePtr);
    uint32_t size = LoadBE32(base, sizePtr);
    Log("  NtAllocateVirtualMemory: %08X bytes at %08X, type %08X\n", size, address, type);
    if (size == 0 || !(type & (kMemCommit | kMemReserve))) {
        ctx.r3.u64 = kStatusInvalidParameter;
        return;
    }

    if (address == 0) {
        size = RoundUp(size, page);
        address = memory.ReserveVirtual(size, page);
        if (!address) {
            ctx.r3.u64 = kStatusNoMemory;
            return;
        }
    } else {
        // Committing (or reserving) a range the title chose: round it out to whole pages.
        uint32_t end = RoundUp(address + size, page);
        address &= ~(page - 1);
        size = end - address;
    }
    if ((type & kMemCommit) && !memory.Commit(address, size)) {
        ctx.r3.u64 = kStatusNoMemory;
        return;
    }
    StoreBE32(base, basePtr, address);
    StoreBE32(base, sizePtr, size);
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS NtFreeVirtualMemory(void** base, size_t* size, u32 type, u32 debug)
// Address space is never reused yet, so freeing only has to succeed.
PPC_FUNC(__imp__NtFreeVirtualMemory) {
    Trace(ctx, "NtFreeVirtualMemory", true);
    ctx.r3.u64 = kStatusSuccess;
}

// void* MmAllocatePhysicalMemoryEx(u32 flags, u32 size, u32 protect, u32 minAddress,
//                                  u32 maxAddress, u32 alignment)
// Returns the allocation in the virtual window matching its page size, or NULL.
PPC_FUNC(__imp__MmAllocatePhysicalMemoryEx) {
    Trace(ctx, "MmAllocatePhysicalMemoryEx", true);
    GuestMemory& memory = Memory();
    uint32_t size = ctx.r4.u32, protect = ctx.r5.u32, maxAddress = ctx.r7.u32, alignment = ctx.r8.u32;
    uint32_t page = (protect & kMem16MbPages) ? 0x1000000 : (protect & kMemLargePages) ? 0x10000 : 0x1000;
    uint32_t window = page == 0x1000000 ? kWindow16M : page == 0x10000 ? kWindow64K : kWindow4K;
    uint32_t limit = maxAddress ? uint32_t(std::min<uint64_t>(uint64_t(maxAddress) + 1, GuestMemory::kPhysicalSize))
                                : GuestMemory::kPhysicalSize;
    size = RoundUp(size, page);
    uint32_t physical = memory.AllocPhysical(size, alignment > page ? alignment : page, limit);
    if (size == 0 || physical == GuestMemory::kNoPhysical || !memory.Commit(window + physical, size)) {
        ctx.r3.u64 = 0;
        return;
    }
    ctx.r3.u64 = window + physical;
}

// void MmFreePhysicalMemory(u32 type, void* address)
// The pages stay committed; the physical range goes back to the pool.
PPC_FUNC(__imp__MmFreePhysicalMemory) {
    Trace(ctx, "MmFreePhysicalMemory", true);
    Memory().FreePhysical(ctx.r4.u32 & (GuestMemory::kPhysicalSize - 1));
}

// u32 MmGetPhysicalAddress(void* address)
PPC_FUNC(__imp__MmGetPhysicalAddress) {
    Trace(ctx, "MmGetPhysicalAddress", true);
    ctx.r3.u64 = ctx.r3.u32 & (GuestMemory::kPhysicalSize - 1);
}

// NTSTATUS MmQueryStatistics(MM_STATISTICS* stats)
// Fields: size, total pages, kernel pages, then title and system sections of 11 words each
// (available pages first), then the highest physical page. 104 bytes, sizes in 4 KB pages.
PPC_FUNC(__imp__MmQueryStatistics) {
    Trace(ctx, "MmQueryStatistics", true);
    constexpr uint32_t kSize = 104;
    uint32_t stats = ctx.r3.u32;
    if (!stats) {
        ctx.r3.u64 = kStatusInvalidParameter;
        return;
    }
    if (LoadBE32(base, stats) != kSize) {
        ctx.r3.u64 = kStatusBufferTooSmall;
        return;
    }
    for (uint32_t offset = 0; offset < kSize; offset += 4) StoreBE32(base, stats + offset, 0);
    uint32_t totalPages = GuestMemory::kPhysicalSize / 0x1000;
    StoreBE32(base, stats + 0x00, kSize);
    StoreBE32(base, stats + 0x04, totalPages);
    StoreBE32(base, stats + 0x08, 0x100);                                      // kernel pages
    StoreBE32(base, stats + 0x0C, Memory().PhysicalAvailable() / 0x1000);      // title: available pages
    StoreBE32(base, stats + 0x64, totalPages - 1);                             // highest physical page
    ctx.r3.u64 = kStatusSuccess;
}
