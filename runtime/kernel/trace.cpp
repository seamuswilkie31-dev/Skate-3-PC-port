#include "kernel/kernel.h"

#include "memory.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace s3n::kernel {
namespace {

constexpr uint64_t kDetailedPerImport = 8;  // log the first calls of each import in full
constexpr uint64_t kDetailedTotal = 4000;   // and stop logging single calls after this many

struct RecentCall {
    const char* name;
    uint32_t lr;
    uint32_t r3;
};

std::mutex gMutex;
Options gOptions;
GuestMemory* gMemory = nullptr;
std::atomic<PPCContext*> gMainContext{nullptr};
std::atomic<uint64_t> gCalls{0};
std::atomic<int64_t> gLastCallMs{0};
std::chrono::steady_clock::time_point gStart;
std::unordered_map<const char*, uint64_t> gCounts;
std::vector<const char*> gOrder;  // imports in order of first call
RecentCall gRecent[16];
std::atomic<bool> gFinishing{false};

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - gStart).count();
}

void VLog(const char* format, va_list args) {
    va_list copy;
    va_copy(copy, args);
    std::vfprintf(stdout, format, args);
    if (gOptions.log) std::vfprintf(gOptions.log, format, copy);
    va_end(copy);
}

LONG CALLBACK OnException(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD& e = *info->ExceptionRecord;
    char reason[160];
    switch (e.ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION: {
        uintptr_t target = e.ExceptionInformation[1];
        const char* kind = e.ExceptionInformation[0] == 1 ? "write to" : e.ExceptionInformation[0] == 8 ? "execute at" : "read from";
        if (gMemory && gMemory->Contains(target))
            std::snprintf(reason, sizeof reason, "guest %s unmapped address 0x%08X", kind, gMemory->ToGuest(target));
        else
            std::snprintf(reason, sizeof reason, "host access violation (%s %p)", kind, reinterpret_cast<void*>(target));
        Finish(reason, 3);
    }
    case EXCEPTION_BREAKPOINT:
        Finish("guest reached a trap (__builtin_debugtrap)", 4);
    case EXCEPTION_STACK_OVERFLOW:
        Finish("host stack overflow (runaway guest recursion?)", 5);
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_PRIV_INSTRUCTION:
        std::snprintf(reason, sizeof reason, "host exception 0x%08lX in recompiled code", e.ExceptionCode);
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
    std::thread(Watchdog).detach();
}

void SetMainContext(PPCContext* ctx) { gMainContext = ctx; }

GuestMemory& Memory() { return *gMemory; }

namespace {
TlsLayout gTlsLayout;
std::atomic<uint32_t> gNextHandle{0xF8000004};
}  // namespace

void SetTlsLayout(const TlsLayout& layout) { gTlsLayout = layout; }
const TlsLayout& GetTlsLayout() { return gTlsLayout; }
uint32_t NewHandle() { return gNextHandle.fetch_add(4); }

void Log(const char* format, ...) {
    std::lock_guard lock(gMutex);
    va_list args;
    va_start(args, format);
    VLog(format, args);
    va_end(args);
}

void Trace(PPCContext& ctx, const char* name, bool implemented) {
    std::lock_guard lock(gMutex);
    uint64_t call = ++gCalls;
    gLastCallMs = NowMs();
    uint64_t& count = gCounts[name];
    if (count++ == 0) gOrder.push_back(name);
    gRecent[call % 16] = {name, uint32_t(ctx.lr), ctx.r3.u32};
    if (count <= kDetailedPerImport && call <= kDetailedTotal) {
        std::fprintf(stdout, "#%-5llu %-36s r3=%08X r4=%08X r5=%08X r6=%08X lr=%08X%s\n", call, name, ctx.r3.u32,
                     ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, uint32_t(ctx.lr), implemented ? "" : "  [stub]");
        if (gOptions.log)
            std::fprintf(gOptions.log, "#%-5llu %-36s r3=%08X r4=%08X r5=%08X r6=%08X lr=%08X%s\n", call, name,
                         ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, uint32_t(ctx.lr), implemented ? "" : "  [stub]");
    }
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
    bool locked = gMutex.try_lock();
    auto log = [](const char* format, ...) {
        va_list args;
        va_start(args, format);
        VLog(format, args);
        va_end(args);
    };
    log("\n==== run ended after %.1f s: %s\n", NowMs() / 1000.0, reason);
    if (PPCContext* c = gMainContext.load()) {
        log("main thread: lr=%08X ctr=%08X r1=%08X r3=%08X r4=%08X r5=%08X r13=%08X r31=%08X\n", uint32_t(c->lr),
            c->ctr.u32, c->r1.u32, c->r3.u32, c->r4.u32, c->r5.u32, c->r13.u32, c->r31.u32);
    }
    uint64_t calls = gCalls.load();
    log("last kernel calls (oldest first):\n");
    for (uint64_t i = calls > 15 ? calls - 15 : 1; i <= calls; ++i) {
        const RecentCall& r = gRecent[i % 16];
        if (r.name) log("  #%-5llu %-36s r3=%08X lr=%08X\n", i, r.name, r.r3, r.lr);
    }
    log("%llu kernel calls to %zu different imports, in order of first use:\n", calls, gOrder.size());
    for (const char* name : gOrder) log("  %8llu  %s\n", gCounts[name], name);
    std::fflush(stdout);
    if (gOptions.log) std::fflush(gOptions.log);
    if (locked) gMutex.unlock();
    ExitProcess(UINT(exitCode));
}

}  // namespace s3n::kernel
