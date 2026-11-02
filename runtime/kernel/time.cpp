// Time: system time, the performance counter, KeTimeStampBundle and kernel timers.
//
// One host clock thread wakes every millisecond to refresh the time stamp bundle and to
// fire armed timers (signal the KTIMER, queue its APC, re-arm periodic ones).

#include "kernel/kernel.h"
#include "memory.h"

#include <windows.h>
#include <timeapi.h>

#include <condition_variable>
#include <intrin.h>
#include <mutex>
#include <thread>
#include <unordered_map>

#pragma comment(lib, "winmm.lib")

namespace s3n::kernel {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kTimerSize = 0x28;

struct Timer {
    Clock::time_point due;
    uint32_t periodMs = 0;
    uint32_t routine = 0;  // APC routine(arg, low time, high time), queued to `thread`
    uint32_t arg = 0;
    uint32_t thread = 0;
};

std::mutex gTimersMutex;
std::condition_variable gTimersWake;
std::unordered_map<uint32_t, Timer> gTimers;  // armed timers by KTIMER address
uint32_t gTimeStampBundle = 0;
Clock::time_point gBoot;

// The guest's timebase (mftb, KeQueryPerformanceFrequency) runs at the console's 50 MHz.
// Titles assume that rate in places, so the host's TSC is scaled to it: 32.32 fixed point.
constexpr uint64_t kTimebaseFrequency = 50000000;
uint64_t gTscStart = 0;
uint64_t gTscToTimebase = 0;

uint64_t MeasureTscFrequency() {
    LARGE_INTEGER frequency, start, now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    uint64_t tscStart = __rdtsc();
    do {
        QueryPerformanceCounter(&now);
    } while (now.QuadPart - start.QuadPart < frequency.QuadPart / 20);  // 50 ms
    uint64_t tscEnd = __rdtsc();
    return (tscEnd - tscStart) * uint64_t(frequency.QuadPart) / uint64_t(now.QuadPart - start.QuadPart);
}

// KeTimeStampBundle: interrupt time (100 ns since boot), system time, tick count (ms).
void UpdateTimeStampBundle(uint8_t* base, Clock::time_point now) {
    if (!gTimeStampBundle) return;
    uint64_t sinceBoot = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now - gBoot).count());
    StoreBE64(base, gTimeStampBundle + 0x00, sinceBoot / 100);
    StoreBE64(base, gTimeStampBundle + 0x08, SystemTime());
    StoreBE32(base, gTimeStampBundle + 0x10, uint32_t(sinceBoot / 1000000));
}

void ClockThread() {
    uint8_t* base = Memory().base();
    std::unique_lock lock(gTimersMutex);
    for (;;) {
        Clock::time_point now = Clock::now();
        UpdateTimeStampBundle(base, now);
        for (auto it = gTimers.begin(); it != gTimers.end();) {
            Timer& t = it->second;
            if (t.due > now) {
                ++it;
                continue;
            }
            SignalObject(it->first, 1);
            if (t.routine) {
                uint64_t time = SystemTime();
                QueueUserApc(t.thread, t.routine, t.arg, uint32_t(time), uint32_t(time >> 32));
            }
            if (t.periodMs) {
                t.due += std::chrono::milliseconds(t.periodMs);
                ++it;
            } else {
                it = gTimers.erase(it);
            }
        }
        Clock::time_point next = now + std::chrono::milliseconds(1);
        for (const auto& [timer, t] : gTimers) next = std::min(next, t.due);
        gTimersWake.wait_until(lock, next);
    }
}

}  // namespace

uint64_t SystemTime() {
    FILETIME time;
    GetSystemTimePreciseAsFileTime(&time);
    return uint64_t(time.dwHighDateTime) << 32 | time.dwLowDateTime;
}

Clock::time_point DeadlineFor(int64_t timeout) {
    Clock::time_point now = Clock::now();
    if (timeout >= 0) {
        int64_t remaining = timeout - int64_t(SystemTime());  // absolute (0: poll)
        return remaining > 0 ? now + std::chrono::nanoseconds(remaining * 100) : now;
    }
    return now + std::chrono::nanoseconds(-timeout * 100);
}

Deadline GuestTimeout(const uint8_t* base, uint32_t timeoutPtr) {
    if (!timeoutPtr) return std::nullopt;
    return DeadlineFor(int64_t(LoadBE64(base, timeoutPtr)));
}

Deadline MillisecondTimeout(uint32_t milliseconds) {
    if (milliseconds == 0xFFFFFFFF) return std::nullopt;
    return Clock::now() + std::chrono::milliseconds(milliseconds);
}

void StartClock() {
    timeBeginPeriod(1);  // millisecond sleeps and waits
    gBoot = Clock::now();
    uint64_t tscFrequency = MeasureTscFrequency();
    gTscToTimebase = (kTimebaseFrequency << 32) / tscFrequency;
    gTscStart = __rdtsc();
    Log("  timebase: host TSC %.3f GHz scaled to the console's 50 MHz\n", tscFrequency / 1e9);
    std::thread(ClockThread).detach();
}

void SetTimeStampBundle(uint32_t address) {
    std::lock_guard lock(gTimersMutex);
    gTimeStampBundle = address;
    UpdateTimeStampBundle(Memory().base(), Clock::now());
}

}  // namespace s3n::kernel

using namespace s3n;
using namespace s3n::kernel;

// mftb, as recompiled by our XenonRecomp patch 0003.
extern "C" uint64_t PPCQueryTimebase() {
    uint64_t high, low = _umul128(__rdtsc() - gTscStart, gTscToTimebase, &high);
    return high << 32 | low >> 32;
}

// u64 KeQueryPerformanceFrequency()
PPC_FUNC(__imp__KeQueryPerformanceFrequency) {
    Trace(ctx, "KeQueryPerformanceFrequency", true);
    ctx.r3.u64 = kTimebaseFrequency;
}

// void KeQuerySystemTime(u64* time)
PPC_FUNC(__imp__KeQuerySystemTime) {
    Trace(ctx, "KeQuerySystemTime", true);
    if (ctx.r3.u32) StoreBE64(base, ctx.r3.u32, SystemTime());
}

// TIME_FIELDS: year, month, day, hour, minute, second, milliseconds, weekday (s16 each).
// The times are FILETIMEs: 100 ns units since 1601, as on Windows.

// void RtlTimeToTimeFields(s64* time, TIME_FIELDS* fields)
PPC_FUNC(__imp__RtlTimeToTimeFields) {
    Trace(ctx, "RtlTimeToTimeFields", true);
    uint64_t time = LoadBE64(base, ctx.r3.u32);
    FILETIME file{DWORD(time), DWORD(time >> 32)};
    SYSTEMTIME s{};
    FileTimeToSystemTime(&file, &s);
    const uint16_t fields[8] = {s.wYear, s.wMonth, s.wDay, s.wHour, s.wMinute, s.wSecond, s.wMilliseconds, s.wDayOfWeek};
    for (int i = 0; i < 8; ++i) {
        base[ctx.r4.u32 + i * 2] = uint8_t(fields[i] >> 8);
        base[ctx.r4.u32 + i * 2 + 1] = uint8_t(fields[i]);
    }
}

// BOOLEAN RtlTimeFieldsToTime(TIME_FIELDS* fields, s64* time)
PPC_FUNC(__imp__RtlTimeFieldsToTime) {
    Trace(ctx, "RtlTimeFieldsToTime", true);
    auto field = [&](int i) { return uint16_t(base[ctx.r3.u32 + i * 2] << 8 | base[ctx.r3.u32 + i * 2 + 1]); };
    SYSTEMTIME s{field(0), field(1), field(7), field(2), field(3), field(4), field(5), field(6)};
    FILETIME file;
    if (!SystemTimeToFileTime(&s, &file)) {
        ctx.r3.u64 = 0;
        return;
    }
    StoreBE64(base, ctx.r4.u32, uint64_t(file.dwHighDateTime) << 32 | file.dwLowDateTime);
    ctx.r3.u64 = 1;
}

// NTSTATUS NtCreateTimer(HANDLE* out, OBJECT_ATTRIBUTES* attributes, u32 type)
// type 0: notification, 1: synchronization. Names aren't supported for timers yet.
PPC_FUNC(__imp__NtCreateTimer) {
    Trace(ctx, "NtCreateTimer", true);
    uint8_t type = ctx.r5.u32 ? kSynchronizationTimer : kNotificationTimer;
    uint32_t timer = NewObject(kTimerSize, type, 0);
    if (!timer) {
        ctx.r3.u64 = kStatusNoMemory;
        return;
    }
    StoreBE32(base, ctx.r3.u32, CreateHandle(timer));
    DereferenceObject(timer);
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS NtSetTimerEx(HANDLE timer, s64* dueTime, void* apcRoutine, u32 apcMode,
//                       void* apcArgument, BOOL resume, s32 periodMs, u32 unknown)
// Arms the timer (clearing it); when due it's signaled and, given a routine, queues
// routine(apcArgument, low time, high time) to the calling thread.
PPC_FUNC(__imp__NtSetTimerEx) {
    Trace(ctx, "NtSetTimerEx", true);
    uint32_t timer = LookupHandle(ctx.r3.u32);
    if (!timer || (base[timer] != kNotificationTimer && base[timer] != kSynchronizationTimer) || !ctx.r4.u32) {
        ctx.r3.u64 = timer ? kStatusInvalidParameter : kStatusInvalidHandle;
        return;
    }
    SignalObject(timer, 0);
    Timer t;
    int64_t dueTime = int64_t(LoadBE64(base, ctx.r4.u32));
    static std::atomic<uint32_t> logged{0};
    if (logged++ < 16)
        Log("  t%u NtSetTimerEx: due %lld, period %d ms\n", CurrentThreadNumber(), dueTime, ctx.r9.s32);
    t.due = DeadlineFor(dueTime);
    t.routine = ctx.r5.u32;
    t.arg = ctx.r7.u32;
    t.thread = CurrentThread();
    t.periodMs = ctx.r9.s32 > 0 ? ctx.r9.u32 : 0;
    {
        std::lock_guard lock(gTimersMutex);
        gTimers[timer] = t;
    }
    gTimersWake.notify_all();
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS NtCancelTimer(HANDLE timer, BOOL* currentState)
PPC_FUNC(__imp__NtCancelTimer) {
    Trace(ctx, "NtCancelTimer", true);
    uint32_t timer = LookupHandle(ctx.r3.u32);
    if (!timer) {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    {
        std::lock_guard lock(gTimersMutex);
        gTimers.erase(timer);
    }
    if (ctx.r4.u32) StoreBE32(base, ctx.r4.u32, LoadBE32(base, timer + 4) ? 1 : 0);
    ctx.r3.u64 = kStatusSuccess;
}
