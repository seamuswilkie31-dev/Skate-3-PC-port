// Guest threads.
//
// Every guest thread runs on its own host thread. Its console-side state lives in guest
// memory where title code expects it: a processor block (r13), a KTHREAD, a TLS block and
// a stack. The KTHREAD is a dispatcher object, signaled when the thread exits.

#include "kernel/kernel.h"
#include "memory.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace s3n::kernel {
namespace {

// The processor-block (r13) and KTHREAD fields title code reads directly.
constexpr uint32_t kPcrTls = 0x000;
constexpr uint32_t kPcrSelf = 0x030;
constexpr uint32_t kPcrStackBase = 0x070;  // high end
constexpr uint32_t kPcrStackLimit = 0x074;  // low end
constexpr uint32_t kPcrCurrentThread = 0x100;
constexpr uint32_t kPcrProcessor = 0x10C;  // u8
constexpr uint32_t kPcrSize = 0x1000;

constexpr uint32_t kThreadStackBase = 0x05C;
constexpr uint32_t kThreadStackLimit = 0x060;
constexpr uint32_t kThreadTls = 0x068;
constexpr uint32_t kThreadSuspendCount = 0x0BC;  // u8
constexpr uint32_t kThreadProcessor = 0x0BF;     // u8
constexpr uint32_t kThreadStackAllocation = 0x0D0;
constexpr uint32_t kThreadCreateTime = 0x130;
constexpr uint32_t kThreadExitTime = 0x138;
constexpr uint32_t kThreadExitStatus = 0x140;
constexpr uint32_t kThreadId = 0x14C;
constexpr uint32_t kThreadStartAddress = 0x150;
constexpr uint32_t kThreadCreationFlags = 0x16C;
constexpr uint32_t kThreadSize = 0xB00;

constexpr uint32_t kCreateSuspended = 0x1;
constexpr uint32_t kProcessors = 6;  // three cores, two hardware threads each
constexpr uint32_t kHostStackReserve = 256u << 20;  // recompiled code recurses on the host stack

struct Thread {
    uint32_t number = 0;
    std::string name;  // from the title's SetThreadName exception; under gThreadsMutex
    ThreadStart start;
    uint32_t kthread = 0;
    uint32_t block = 0;  // stack, then processor block, then TLS: one stack-range allocation
    uint32_t stackLimit = 0;
    uint32_t stackBase = 0;
    uint32_t pcr = 0;
    uint32_t tls = 0;
    HANDLE host = nullptr;
    int32_t priority = 0;
    uint32_t affinity = (1u << kProcessors) - 1;
    std::atomic<uint32_t> suspendCount{0};
    std::atomic<PPCContext*> ctx{nullptr};  // while it runs guest code
    std::atomic<bool> exited{false};
    std::atomic<uint32_t> exitCode{0};
    std::atomic<const char*> waitingIn{nullptr};
    std::atomic<uint32_t> waitObject{0};
    std::atomic<int64_t> waitSinceMs{0};
};

TlsLayout gTlsLayout;
uint32_t gDefaultStackSize = 0x40000;
std::mutex gThreadsMutex;
std::vector<std::unique_ptr<Thread>> gThreads;  // never shrinks: reports show exited threads too
std::unordered_map<uint32_t, Thread*> gByKthread;
thread_local Thread* tCurrent = nullptr;

int64_t NowMs() { return GetTickCount64(); }

Thread* FindThread(uint32_t kthread) {
    std::lock_guard lock(gThreadsMutex);
    auto it = gByKthread.find(kthread);
    return it == gByKthread.end() ? nullptr : it->second;
}

PPCFunc* GuestFunction(uint8_t* base, uint32_t address) {
    if (address < PPC_CODE_BASE || address >= PPC_CODE_BASE + PPC_CODE_SIZE || (address & 3)) return nullptr;
    return PPC_LOOKUP_FUNC(base, address);
}

// The thread's end, whether its entry returned or it called ExTerminateThread.
void ExitGuestThread(Thread& t, uint32_t exitCode) {
    uint8_t* base = Memory().base();
    StoreBE64(base, t.kthread + kThreadExitTime, SystemTime());
    StoreBE32(base, t.kthread + kThreadExitStatus, exitCode);
    t.exitCode = exitCode;
    t.ctx = nullptr;
    t.exited = true;
    {
        std::lock_guard lock(gThreadsMutex);
        gByKthread.erase(t.kthread);  // the KTHREAD's memory is reused once its handles close
    }
    SignalObject(t.kthread, 1);
    Memory().FreeStack(t.block);
    DereferenceObject(t.kthread);  // the thread's own reference
}

DWORD WINAPI RunThread(void* param) {
    Thread& t = *static_cast<Thread*>(param);
    tCurrent = &t;
    uint8_t* base = Memory().base();
    PPCContext ctx{};
    ctx.fpscr.loadFromHost();
    ctx.r1.u64 = t.stackBase - 0x100;  // leave room for the caller's linkage area
    ctx.r13.u64 = t.pcr;
    ctx.r3.u64 = t.start.r3;
    ctx.r4.u64 = t.start.r4;
    t.ctx = &ctx;
    PPCFunc* entry = GuestFunction(base, t.start.entry);
    if (!entry) {
        char reason[96];
        std::snprintf(reason, sizeof reason, "thread t%u starts at 0x%08X, which isn't recompiled code", t.number,
                      t.start.entry);
        Finish(reason, 3);
    }
    if (t.start.main) Log("starting the entry point 0x%08X\n\n", t.start.entry);
    entry(ctx, base);
    if (t.start.main) Finish("the entry point returned", 0);
    ExitGuestThread(t, ctx.r3.u32);
    return 0;
}

}  // namespace

void SetTlsLayout(const TlsLayout& layout) { gTlsLayout = layout; }
const TlsLayout& GetTlsLayout() { return gTlsLayout; }
void SetDefaultStackSize(uint32_t size) {
    if (size) gDefaultStackSize = size;
}

uint32_t CreateGuestThread(const ThreadStart& start, uint32_t* threadId) {
    GuestMemory& memory = Memory();
    uint8_t* base = memory.base();
    auto t = std::make_unique<Thread>();
    t->start = start;

    uint32_t stackSize = start.stackSize ? start.stackSize : gDefaultStackSize;
    stackSize = std::max<uint32_t>(0x4000, (stackSize + 0xFFF) & ~0xFFFu);
    uint32_t tlsSize = (gTlsLayout.staticSize + gTlsLayout.slotCount * 4 + 0xFFF) & ~0xFFFu;
    t->block = memory.AllocStack(stackSize + kPcrSize + tlsSize);
    if (!t->block) return 0;
    t->kthread = NewObject(kThreadSize, kThreadObject, 0);
    if (!t->kthread) {
        memory.FreeStack(t->block);
        return 0;
    }
    ReferenceObject(t->kthread);  // one for the caller, one held until the thread exits

    t->stackLimit = t->block;
    t->stackBase = t->block + stackSize;
    t->pcr = t->stackBase;
    t->tls = t->pcr + kPcrSize;
    std::memset(base + t->pcr, 0, kPcrSize + tlsSize);  // stack-range memory is reused
    if (gTlsLayout.templateSize)
        std::memcpy(base + t->tls, base + gTlsLayout.templateAddress,
                    std::min(gTlsLayout.templateSize, gTlsLayout.staticSize));

    {
        std::lock_guard lock(gThreadsMutex);
        t->number = uint32_t(gThreads.size()) + 1;
    }
    uint8_t processor = uint8_t((t->number - 1) % kProcessors);
    StoreBE32(base, t->pcr + kPcrTls, t->tls);
    StoreBE32(base, t->pcr + kPcrSelf, t->pcr);
    StoreBE32(base, t->pcr + kPcrStackBase, t->stackBase);
    StoreBE32(base, t->pcr + kPcrStackLimit, t->stackLimit);
    StoreBE32(base, t->pcr + kPcrCurrentThread, t->kthread);
    base[t->pcr + kPcrProcessor] = processor;
    StoreBE32(base, t->kthread + kThreadStackBase, t->stackBase);
    StoreBE32(base, t->kthread + kThreadStackLimit, t->stackLimit);
    StoreBE32(base, t->kthread + kThreadTls, t->tls);
    base[t->kthread + kThreadProcessor] = processor;
    StoreBE32(base, t->kthread + kThreadStackAllocation, t->block);
    StoreBE64(base, t->kthread + kThreadCreateTime, SystemTime());
    StoreBE32(base, t->kthread + kThreadId, t->number);
    StoreBE32(base, t->kthread + kThreadStartAddress, start.entry);
    StoreBE32(base, t->kthread + kThreadCreationFlags, start.flags);

    // Start the host thread suspended so it's registered before it runs.
    t->host = CreateThread(nullptr, kHostStackReserve, RunThread, t.get(),
                           CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!t->host) {
        DereferenceObject(t->kthread);
        DereferenceObject(t->kthread);
        memory.FreeStack(t->block);
        return 0;
    }
    WatchThread(t->host);
    bool suspended = (start.flags & kCreateSuspended) != 0;
    t->suspendCount = suspended ? 1 : 0;
    base[t->kthread + kThreadSuspendCount] = suspended ? 1 : 0;

    Thread* thread = t.get();
    {
        std::lock_guard lock(gThreadsMutex);
        gByKthread[thread->kthread] = thread;
        gThreads.push_back(std::move(t));
    }
    Log("  t%u created: entry %08X (r3=%08X r4=%08X) stack %08X-%08X pcr %08X kthread %08X%s\n", thread->number,
        start.entry, start.r3, start.r4, thread->stackLimit, thread->stackBase, thread->pcr, thread->kthread,
        suspended ? " suspended" : "");
    if (threadId) *threadId = thread->number;
    if (!suspended) ResumeThread(thread->host);
    return thread->kthread;
}

uint32_t CurrentThread() { return tCurrent ? tCurrent->kthread : 0; }
uint32_t CurrentThreadNumber() { return tCurrent ? tCurrent->number : 0; }
PPCContext* CurrentContext() { return tCurrent ? tCurrent->ctx.load() : nullptr; }

void SetThreadName(uint32_t threadId, const std::string& name) {
    std::lock_guard lock(gThreadsMutex);
    uint32_t number = threadId == 0xFFFFFFFF ? CurrentThreadNumber() : threadId;
    if (number == 0 || number > gThreads.size()) return;
    gThreads[number - 1]->name = name;
}

void CallGuest(PPCContext& ctx, uint8_t* base, uint32_t function, uint32_t r3, uint32_t r4, uint32_t r5) {
    PPCFunc* f = GuestFunction(base, function);
    if (!f) {
        char reason[96];
        std::snprintf(reason, sizeof reason, "kernel callback to 0x%08X, which isn't recompiled code", function);
        Finish(reason, 3);
    }
    PPCContext saved = ctx;
    ctx.r1.u64 = (ctx.r1.u32 - 0x100) & ~0xFu;
    ctx.r3.u64 = r3;
    ctx.r4.u64 = r4;
    ctx.r5.u64 = r5;
    f(ctx, base);
    ctx = saved;
}

void SetWaitState(const char* caller, uint32_t object) {
    if (!tCurrent) return;
    tCurrent->waitObject = object;
    tCurrent->waitSinceMs = NowMs();
    tCurrent->waitingIn = caller;
}

std::string DescribeThreads() {
    std::unique_lock lock(gThreadsMutex, std::try_to_lock);
    if (!lock.owns_lock()) return "  (thread list busy)\n";
    const uint8_t* base = Memory().base();
    std::string out;
    char line[256];
    for (const auto& t : gThreads) {
        int n = std::snprintf(line, sizeof line, "  t%-3u %-22.22s entry %08X  ", t->number,
                              t->start.main ? "(main)" : t->name.c_str(), t->start.entry);
        if (t->exited) {
            std::snprintf(line + n, sizeof line - n, "exited, code %08X\n", t->exitCode.load());
        } else if (t->suspendCount > 0) {
            std::snprintf(line + n, sizeof line - n, "suspended\n");
        } else {
            const char* waiting = t->waitingIn;
            if (waiting) {
                uint32_t object = t->waitObject;
                n += std::snprintf(line + n, sizeof line - n, "waiting %.1f s in %s", (NowMs() - t->waitSinceMs) / 1000.0,
                                   waiting);
                if (object)
                    n += std::snprintf(line + n, sizeof line - n, " on %08X (type %u, state %d)", object, base[object],
                                       int32_t(LoadBE32(base, object + 4)));
            } else {
                n += std::snprintf(line + n, sizeof line - n, "running");
            }
            if (PPCContext* c = t->ctx)
                std::snprintf(line + n, sizeof line - n, "  lr=%08X r1=%08X r3=%08X\n", uint32_t(c->lr), c->r1.u32,
                              c->r3.u32);
            else
                std::snprintf(line + n, sizeof line - n, "\n");
        }
        out += line;
    }
    return out;
}

}  // namespace s3n::kernel

using namespace s3n;
using namespace s3n::kernel;

// NTSTATUS ExCreateThread(HANDLE* handle, u32 stackSize, u32* threadId, void* xapiStartup,
//                         void* startAddress, void* startContext, u32 creationFlags)
// With an XAPI startup routine the thread runs xapiStartup(startAddress, startContext),
// otherwise startAddress(startContext). Flag 0x80 returns the KTHREAD instead of a handle.
PPC_FUNC(__imp__ExCreateThread) {
    Trace(ctx, "ExCreateThread", true);
    uint32_t handlePtr = ctx.r3.u32, idPtr = ctx.r5.u32, xapiStartup = ctx.r6.u32, flags = ctx.r9.u32;
    ThreadStart start;
    if (xapiStartup) {
        start.entry = xapiStartup;
        start.r3 = ctx.r7.u32;
        start.r4 = ctx.r8.u32;
    } else {
        start.entry = ctx.r7.u32;
        start.r3 = ctx.r8.u32;
    }
    start.stackSize = ctx.r4.u32;
    start.flags = flags;
    uint32_t id = 0;
    uint32_t kthread = CreateGuestThread(start, &id);
    if (!kthread) {
        ctx.r3.u64 = kStatusNoMemory;
        return;
    }
    if (handlePtr) {
        if (flags & 0x80) {
            StoreBE32(base, handlePtr, kthread);  // keeps the caller's reference
        } else {
            StoreBE32(base, handlePtr, CreateHandle(kthread));
            DereferenceObject(kthread);
        }
    } else {
        DereferenceObject(kthread);
    }
    if (idPtr) StoreBE32(base, idPtr, id);
    ctx.r3.u64 = kStatusSuccess;
}

// void ExTerminateThread(u32 exitCode)
PPC_FUNC(__imp__ExTerminateThread) {
    Trace(ctx, "ExTerminateThread", true);
    if (!tCurrent) return;
    if (tCurrent->start.main) Finish("the main thread called ExTerminateThread", 0);
    ExitGuestThread(*tCurrent, ctx.r3.u32);
    ExitThread(0);
}

// NTSTATUS NtResumeThread(HANDLE thread, u32* previousSuspendCount)
PPC_FUNC(__imp__NtResumeThread) {
    Trace(ctx, "NtResumeThread", true);
    Thread* t = FindThread(LookupHandle(ctx.r3.u32));
    if (!t) {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    uint32_t previous = t->suspendCount.load();
    while (previous > 0 && !t->suspendCount.compare_exchange_weak(previous, previous - 1)) {
    }
    if (previous > 0) {
        base[t->kthread + kThreadSuspendCount] = uint8_t(previous - 1);
        if (previous == 1) ResumeThread(t->host);
    }
    if (ctx.r4.u32) StoreBE32(base, ctx.r4.u32, previous);
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS KeSetAffinityThread(KTHREAD* thread, u32 affinity, u32* previousAffinity)
// Recorded only: the host schedules guest threads on whatever cores it has.
PPC_FUNC(__imp__KeSetAffinityThread) {
    Trace(ctx, "KeSetAffinityThread", true);
    Thread* t = FindThread(ctx.r3.u32);
    uint32_t previous = t ? t->affinity : (1u << kProcessors) - 1;
    if (t && ctx.r4.u32) t->affinity = ctx.r4.u32;
    if (ctx.r5.u32) StoreBE32(base, ctx.r5.u32, previous);
    ctx.r3.u64 = kStatusSuccess;
}

// s32 KeSetBasePriorityThread(KTHREAD* thread, s32 increment): returns the old increment.
// Mapped onto host priorities within +-2 so no guest thread can starve the others.
PPC_FUNC(__imp__KeSetBasePriorityThread) {
    Trace(ctx, "KeSetBasePriorityThread", true);
    Thread* t = FindThread(ctx.r3.u32);
    int32_t increment = ctx.r4.s32;
    int32_t previous = t ? t->priority : 0;
    if (t) {
        t->priority = increment;
        SetThreadPriority(t->host, std::clamp(increment, -2, 2));
    }
    ctx.r3.u64 = uint64_t(int64_t(previous));
}

// s32 KeQueryBasePriorityThread(KTHREAD* thread)
PPC_FUNC(__imp__KeQueryBasePriorityThread) {
    Trace(ctx, "KeQueryBasePriorityThread", true);
    Thread* t = FindThread(ctx.r3.u32);
    ctx.r3.u64 = uint64_t(int64_t(t ? t->priority : 0));
}

// u32 KeGetCurrentProcessType(): 1, the title (user) process.
PPC_FUNC(__imp__KeGetCurrentProcessType) {
    Trace(ctx, "KeGetCurrentProcessType", true);
    ctx.r3.u64 = 1;
}

// NTSTATUS KeDelayExecutionThread(u32 processorMode, BOOL alertable, s64* interval)
PPC_FUNC(__imp__KeDelayExecutionThread) {
    Trace(ctx, "KeDelayExecutionThread", true);
    uint32_t intervalPtr = ctx.r5.u32;
    if (intervalPtr && LoadBE64(base, intervalPtr) == 0) {
        SwitchToThread();
        ctx.r3.u64 = kStatusSuccess;
        return;
    }
    uint32_t status = WaitForObjects(ctx, base, "KeDelayExecutionThread", nullptr, 0, false, ctx.r4.u32 != 0,
                                     GuestTimeout(base, intervalPtr));
    ctx.r3.u64 = status == kStatusTimeout ? kStatusSuccess : status;
}

// NTSTATUS NtYieldExecution()
PPC_FUNC(__imp__NtYieldExecution) {
    Trace(ctx, "NtYieldExecution", true);
    SwitchToThread();
    ctx.r3.u64 = kStatusSuccess;
}

// void KeEnterCriticalRegion() / KeLeaveCriticalRegion(): they hold off kernel APCs, and
// the runtime delivers none.
PPC_FUNC(__imp__KeEnterCriticalRegion) { Trace(ctx, "KeEnterCriticalRegion", true); }
PPC_FUNC(__imp__KeLeaveCriticalRegion) { Trace(ctx, "KeLeaveCriticalRegion", true); }
