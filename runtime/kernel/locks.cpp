// Critical sections and spinlocks.
//
// An RTL_CRITICAL_SECTION (28 bytes) is a synchronization event followed by a lock count
// (-1 when free, otherwise the number of threads holding or waiting minus one), a
// recursion count and the owning KTHREAD. Taking a free lock is one atomic operation on
// guest memory; only contention goes through the dispatcher, waiting on the event.

#include "kernel/kernel.h"
#include "memory.h"

#include <windows.h>

#include <atomic>
#include <immintrin.h>

namespace s3n::kernel {
namespace {

constexpr uint32_t kCsSpinCount = 0x01;  // header byte: spin count / 256
constexpr uint32_t kCsLockCount = 0x10;
constexpr uint32_t kCsRecursion = 0x14;
constexpr uint32_t kCsOwner = 0x18;

std::atomic_ref<uint32_t> Word(uint8_t* base, uint32_t address) {
    return std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(base + address));
}

// Atomically adds to a big-endian word; returns the new value.
int32_t AtomicAdd(uint8_t* base, uint32_t address, int32_t delta) {
    auto word = Word(base, address);
    uint32_t old = word.load();
    for (;;) {
        uint32_t value = __builtin_bswap32(uint32_t(int32_t(__builtin_bswap32(old)) + delta));
        if (word.compare_exchange_weak(old, value)) return int32_t(__builtin_bswap32(value));
    }
}

bool CompareExchange(uint8_t* base, uint32_t address, uint32_t expected, uint32_t desired) {
    uint32_t old = __builtin_bswap32(expected);
    return Word(base, address).compare_exchange_strong(old, __builtin_bswap32(desired));
}

void InitializeCriticalSection(uint8_t* base, uint32_t cs, uint32_t spinCount) {
    for (uint32_t offset = 0; offset < 0x1C; offset += 4) StoreBE32(base, cs + offset, 0);
    base[cs] = kSynchronizationEvent;
    base[cs + kCsSpinCount] = uint8_t(std::min<uint32_t>((spinCount + 255) / 256, 255));
    base[cs + 2] = 4;                     // header size in words
    StoreBE32(base, cs + 0x08, cs + 0x08);  // empty wait list
    StoreBE32(base, cs + 0x0C, cs + 0x08);
    StoreBE32(base, cs + kCsLockCount, 0xFFFFFFFF);
}

void TakeOwnership(uint8_t* base, uint32_t cs, uint32_t thread) {
    StoreBE32(base, cs + kCsOwner, thread);
    StoreBE32(base, cs + kCsRecursion, 1);
}

// Spins, yielding more and more, until the word goes from 0 to `value`.
void AcquireSpinLock(uint8_t* base, uint32_t lock, uint32_t value) {
    for (uint32_t spins = 0; !CompareExchange(base, lock, 0, value); ++spins) {
        if (spins < 64) _mm_pause();
        else if (spins < 256) SwitchToThread();
        else Sleep(0);
    }
}

thread_local uint32_t tIrql = 0;
constexpr uint32_t kDispatchLevel = 2;

}  // namespace
}  // namespace s3n::kernel

using namespace s3n;
using namespace s3n::kernel;

// void RtlInitializeCriticalSection(RTL_CRITICAL_SECTION* cs)
PPC_FUNC(__imp__RtlInitializeCriticalSection) {
    Trace(ctx, "RtlInitializeCriticalSection", true);
    InitializeCriticalSection(base, ctx.r3.u32, 0);
}

// NTSTATUS RtlInitializeCriticalSectionAndSpinCount(RTL_CRITICAL_SECTION* cs, u32 spinCount)
PPC_FUNC(__imp__RtlInitializeCriticalSectionAndSpinCount) {
    Trace(ctx, "RtlInitializeCriticalSectionAndSpinCount", true);
    InitializeCriticalSection(base, ctx.r3.u32, ctx.r4.u32);
    ctx.r3.u64 = kStatusSuccess;
}

// void RtlEnterCriticalSection(RTL_CRITICAL_SECTION* cs)
PPC_FUNC(__imp__RtlEnterCriticalSection) {
    Trace(ctx, "RtlEnterCriticalSection", true);
    uint32_t cs = ctx.r3.u32, thread = CurrentThread();
    // Spin a while on a lock someone else holds before joining the waiters.
    uint32_t spins = base[cs + kCsSpinCount] * 256u;
    for (uint32_t i = 0; i < spins; ++i) {
        if (int32_t(LoadBE32(base, cs + kCsLockCount)) == -1 && CompareExchange(base, cs + kCsLockCount, 0xFFFFFFFF, 0)) {
            TakeOwnership(base, cs, thread);
            return;
        }
        _mm_pause();
    }
    if (AtomicAdd(base, cs + kCsLockCount, 1) == 0) {
        TakeOwnership(base, cs, thread);
        return;
    }
    if (LoadBE32(base, cs + kCsOwner) == thread) {
        StoreBE32(base, cs + kCsRecursion, LoadBE32(base, cs + kCsRecursion) + 1);
        return;
    }
    WaitForObjects(ctx, base, "RtlEnterCriticalSection", &cs, 1, true, false, std::nullopt);
    TakeOwnership(base, cs, thread);
}

// BOOL RtlTryEnterCriticalSection(RTL_CRITICAL_SECTION* cs)
PPC_FUNC(__imp__RtlTryEnterCriticalSection) {
    Trace(ctx, "RtlTryEnterCriticalSection", true);
    uint32_t cs = ctx.r3.u32, thread = CurrentThread();
    if (CompareExchange(base, cs + kCsLockCount, 0xFFFFFFFF, 0)) {
        TakeOwnership(base, cs, thread);
        ctx.r3.u64 = 1;
    } else if (LoadBE32(base, cs + kCsOwner) == thread) {
        AtomicAdd(base, cs + kCsLockCount, 1);
        StoreBE32(base, cs + kCsRecursion, LoadBE32(base, cs + kCsRecursion) + 1);
        ctx.r3.u64 = 1;
    } else {
        ctx.r3.u64 = 0;
    }
}

// void RtlLeaveCriticalSection(RTL_CRITICAL_SECTION* cs)
PPC_FUNC(__imp__RtlLeaveCriticalSection) {
    Trace(ctx, "RtlLeaveCriticalSection", true);
    uint32_t cs = ctx.r3.u32;
    uint32_t recursion = LoadBE32(base, cs + kCsRecursion) - 1;
    StoreBE32(base, cs + kCsRecursion, recursion);
    if (recursion != 0) {
        AtomicAdd(base, cs + kCsLockCount, -1);
        return;
    }
    StoreBE32(base, cs + kCsOwner, 0);
    if (AtomicAdd(base, cs + kCsLockCount, -1) >= 0) SignalObject(cs, 1);  // hand over to a waiter
}

// u32 KfAcquireSpinLock(u32* lock): returns the previous IRQL.
PPC_FUNC(__imp__KfAcquireSpinLock) {
    Trace(ctx, "KfAcquireSpinLock", true);
    AcquireSpinLock(base, ctx.r3.u32, CurrentThread());
    ctx.r3.u64 = tIrql;
    tIrql = kDispatchLevel;
}

// void KfReleaseSpinLock(u32* lock, u32 oldIrql)
PPC_FUNC(__imp__KfReleaseSpinLock) {
    Trace(ctx, "KfReleaseSpinLock", true);
    Word(base, ctx.r3.u32).store(0);
    tIrql = ctx.r4.u32;
}

// void KeAcquireSpinLockAtRaisedIrql(u32* lock)
PPC_FUNC(__imp__KeAcquireSpinLockAtRaisedIrql) {
    Trace(ctx, "KeAcquireSpinLockAtRaisedIrql", true);
    AcquireSpinLock(base, ctx.r3.u32, CurrentThread());
}

// void KeReleaseSpinLockFromRaisedIrql(u32* lock)
PPC_FUNC(__imp__KeReleaseSpinLockFromRaisedIrql) {
    Trace(ctx, "KeReleaseSpinLockFromRaisedIrql", true);
    Word(base, ctx.r3.u32).store(0);
}
