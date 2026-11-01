#include "kernel/kernel.h"

#include "memory.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace s3n::kernel {
namespace {

constexpr uint64_t kDetailedPerImport = 8;  // log the first calls of each import in full
constexpr size_t kImportSlots = 1024;       // more than the title's 286 function imports

// Per-import call counts, found by the name literal's address. Lock-free, since every
// guest thread passes through here on every kernel call.
struct ImportStats {
    std::atomic<const char*> name{nullptr};
    std::atomic<uint64_t> calls{0};
};

struct RecentCall {
    std::atomic<const char*> name{nullptr};
    std::atomic<uint32_t> thread{0};
    std::atomic<uint32_t> lr{0};
    std::atomic<uint32_t> r3{0};
};

std::mutex gLogMutex;
Options gOptions;
GuestMemory* gMemory = nullptr;
std::atomic<uint64_t> gCalls{0};
std::atomic<int64_t> gLastCallMs{0};
std::chrono::steady_clock::time_point gStart;
ImportStats gImports[kImportSlots];
std::atomic<const char*> gOrder[kImportSlots];  // imports in order of first call
std::atomic<uint32_t> gOrderCount{0};
RecentCall gRecent[16];
std::atomic<bool> gFinishing{false};
constexpr size_t kMaxReportSections = 8;
std::atomic<std::string (*)()> gReportSections[kMaxReportSections];
std::atomic<size_t> gReportSectionCount{0};

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - gStart).count();
}

ImportStats& StatsFor(const char* name) {
    size_t i = (reinterpret_cast<uintptr_t>(name) >> 3) % kImportSlots;
    for (;; i = (i + 1) % kImportSlots) {
        const char* current = gImports[i].name.load(std::memory_order_acquire);
        if (current == name) return gImports[i];
        if (!current) {
            if (gImports[i].name.compare_exchange_strong(current, name)) {
                gOrder[gOrderCount.fetch_add(1)] = name;
                return gImports[i];
            }
            if (current == name) return gImports[i];
        }
    }
}

void VLog(const char* format, va_list args) {
    va_list copy;
    va_copy(copy, args);
    std::vfprintf(stdout, format, args);
    if (gOptions.log) std::vfprintf(gOptions.log, format, copy);
    va_end(copy);
}

// The guest address of the recompiled function whose host code contains `rip`, or 0 when
// it's runtime code. The host function's start comes from the x64 unwind tables.
uint32_t GuestFunctionAt(uintptr_t rip, uintptr_t& hostStart) {
    static std::unordered_map<uintptr_t, uint32_t> byHost;  // built at the first fault
    if (byHost.empty())
        for (const PPCFuncMapping* m = PPCFuncMappings; m->guest; ++m)
            byHost.emplace(reinterpret_cast<uintptr_t>(m->host), uint32_t(m->guest));
    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(rip, &imageBase, nullptr);
    if (!function) return 0;
    hostStart = uintptr_t(imageBase + function->BeginAddress);
    auto it = byHost.find(hostStart);
    return it == byHost.end() ? 0 : it->second;
}

// Where a fault happened, and the faulting guest thread's registers.
int DescribeFault(char* out, size_t size, const CONTEXT& host) {
    int n = 0;
    uintptr_t hostStart = 0;
    if (uint32_t guest = GuestFunctionAt(host.Rip, hostStart))
        n += std::snprintf(out + n, size - n, "\n  in the recompiled function at 0x%08X (host +0x%llX)", guest,
                           uint64_t(host.Rip - hostStart));
    else
        n += std::snprintf(out + n, size - n, "\n  in runtime code at %p", reinterpret_cast<void*>(host.Rip));
    if (const PPCContext* c = CurrentContext()) {
        n += std::snprintf(out + n, size - n,
                           "\n  lr=%08X ctr=%08X r1=%08X r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X\n"
                           "  r9=%08X r10=%08X r11=%08X r12=%08X r29=%08X r30=%08X r31=%08X",
                           uint32_t(c->lr), c->ctr.u32, c->r1.u32, c->r3.u32, c->r4.u32, c->r5.u32, c->r6.u32,
                           c->r7.u32, c->r8.u32, c->r9.u32, c->r10.u32, c->r11.u32, c->r12.u32, c->r29.u32, c->r30.u32,
                           c->r31.u32);
    }
    return n;
}

LONG CALLBACK OnException(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD& e = *info->ExceptionRecord;
    char reason[768];
    uint32_t thread = CurrentThreadNumber();
    switch (e.ExceptionCode) {
    case EXCEPTION_SINGLE_STEP: {
        if (!gOptions.watchAddress || !(info->ContextRecord->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;
        info->ContextRecord->Dr6 = 0;
        int n = std::snprintf(reason, sizeof reason, "watch: t%u wrote %08X to 0x%08X", thread,
                              LoadBE32(gMemory->base(), gOptions.watchAddress), gOptions.watchAddress);
        DescribeFault(reason + n, sizeof reason - n, *info->ContextRecord);
        Log("%s\n", reason);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    case EXCEPTION_ACCESS_VIOLATION: {
        uintptr_t target = e.ExceptionInformation[1];
        const char* kind = e.ExceptionInformation[0] == 1 ? "write to" : e.ExceptionInformation[0] == 8 ? "execute at" : "read from";
        int n;
        if (gMemory && gMemory->Contains(target))
            n = std::snprintf(reason, sizeof reason, "t%u: guest %s unmapped address 0x%08X", thread, kind,
                              gMemory->ToGuest(target));
        else
            n = std::snprintf(reason, sizeof reason, "t%u: host access violation (%s %p)", thread, kind,
                              reinterpret_cast<void*>(target));
        DescribeFault(reason + n, sizeof reason - n, *info->ContextRecord);
        Finish(reason, 3);
    }
    case EXCEPTION_BREAKPOINT:
        std::snprintf(reason, sizeof reason, "t%u: guest reached a trap (__builtin_debugtrap)", thread);
        Finish(reason, 4);
    case EXCEPTION_STACK_OVERFLOW:
        std::snprintf(reason, sizeof reason, "t%u: host stack overflow (runaway guest recursion?)", thread);
        Finish(reason, 5);
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_PRIV_INSTRUCTION:
        std::snprintf(reason, sizeof reason, "t%u: host exception 0x%08lX in recompiled code", thread, e.ExceptionCode);
        Finish(reason, 6);
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

void Watchdog() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        int64_t now = NowMs();
        if (now - gLastCallMs.load() > int64_t(gOptions.stallSeconds) * 1000) {
            char reason[96];
            std::snprintf(reason, sizeof reason, "no kernel calls for %u s: the guest is busy or waiting", gOptions.stallSeconds);
            Finish(reason, 2);
        }
        if (now > int64_t(gOptions.timeLimitSeconds) * 1000) Finish("time limit reached", 2);
    }
}

}  // namespace

void Init(GuestMemory& memory, const Options& options) {
    gMemory = &memory;
    gOptions = options;
    gStart = std::chrono::steady_clock::now();
    AddVectoredExceptionHandler(1, OnException);
    StartClock();
    std::thread(Watchdog).detach();
}

GuestMemory& Memory() { return *gMemory; }

void WatchThread(void* hostThread) {
    if (!gOptions.watchAddress) return;
    CONTEXT c{};
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(hostThread, &c)) return;
    c.Dr0 = reinterpret_cast<uintptr_t>(gMemory->base() + (gOptions.watchAddress & ~3u));
    c.Dr7 = 1 | (1 << 16) | (3 << 18);  // local breakpoint 0: on writes, 4 bytes
    SetThreadContext(hostThread, &c);
}

void Log(const char* format, ...) {
    std::lock_guard lock(gLogMutex);
    va_list args;
    va_start(args, format);
    VLog(format, args);
    va_end(args);
}

void Trace(PPCContext& ctx, const char* name, bool implemented) {
    uint64_t call = ++gCalls;
    gLastCallMs.store(NowMs(), std::memory_order_relaxed);
    uint64_t count = ++StatsFor(name).calls;
    uint32_t thread = CurrentThreadNumber();
    RecentCall& recent = gRecent[call % 16];
    recent.name.store(name, std::memory_order_relaxed);
    recent.thread.store(thread, std::memory_order_relaxed);
    recent.lr.store(uint32_t(ctx.lr), std::memory_order_relaxed);
    recent.r3.store(ctx.r3.u32, std::memory_order_relaxed);
    if (count <= kDetailedPerImport) {
        Log("#%-6llu t%-2u %-36s r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X lr=%08X%s\n", call, thread, name, ctx.r3.u32,
            ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, uint32_t(ctx.lr), implemented ? "" : "  [stub]");
    }
}

void AddReportSection(std::string (*describe)()) {
    size_t slot = gReportSectionCount.fetch_add(1);
    if (slot < kMaxReportSections) gReportSections[slot] = describe;
}

void UnimplementedImport(PPCContext& ctx, uint8_t*, const char* name) {
    Trace(ctx, name, false);
    ctx.r3.u64 = 0;  // STATUS_SUCCESS / ERROR_SUCCESS / NULL
}

void Finish(const char* reason, int exitCode) {
    if (gFinishing.exchange(true)) {
        // Another thread is already reporting; wait for it to exit the process.
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    // Don't wait on a lock a faulting guest thread might hold.
    bool locked = gLogMutex.try_lock();
    auto log = [](const char* format, ...) {
        va_list args;
        va_start(args, format);
        VLog(format, args);
        va_end(args);
    };
    log("\n==== run ended after %.1f s: %s\n", NowMs() / 1000.0, reason);
    log("guest threads:\n%s", DescribeThreads().c_str());
    for (size_t i = 0; i < std::min(gReportSectionCount.load(), kMaxReportSections); ++i)
        if (auto describe = gReportSections[i].load()) log("%s", describe().c_str());
    uint64_t calls = gCalls.load();
    log("last kernel calls (oldest first):\n");
    for (uint64_t i = calls > 15 ? calls - 15 : 1; i <= calls; ++i) {
        const RecentCall& r = gRecent[i % 16];
        if (const char* name = r.name.load())
            log("  #%-9llu t%-2u %-36s r3=%08X lr=%08X\n", i, r.thread.load(), name, r.r3.load(), r.lr.load());
    }
    uint32_t imports = std::min<uint32_t>(gOrderCount.load(), kImportSlots);
    log("%llu kernel calls to %u different imports, in order of first use:\n", calls, imports);
    for (uint32_t i = 0; i < imports; ++i)
        if (const char* name = gOrder[i].load()) log("  %10llu  %s\n", StatsFor(name).calls.load(), name);
    std::fflush(stdout);
    if (gOptions.log) std::fflush(gOptions.log);
    if (locked) gLogMutex.unlock();
    ExitProcess(UINT(exitCode));
}

}  // namespace s3n::kernel
