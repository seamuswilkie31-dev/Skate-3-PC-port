#pragma once

// The console side of the runtime: kernel/XAM imports, threads, tracing and run reports.

#include <ppc_config.h>
#include <ppc_context.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

namespace s3n {
class Disc;
class GuestMemory;
}

namespace s3n::kernel {

struct Options {
    std::FILE* log = nullptr;
    uint32_t stallSeconds = 15;  // end the run if no kernel call happens for this long
    uint32_t timeLimitSeconds = 120;
    uint32_t watchAddress = 0;  // log every guest write to this word (a hardware breakpoint)
};

// Installs the fault handler, starts the watchdog and the clock.
void Init(GuestMemory& memory, const Options& options);
GuestMemory& Memory();
// Arms the --watch breakpoint on a host thread that hasn't started yet.
void WatchThread(void* hostThread);

// Gives the kernel variables the title imports their contents (loader storage, zeroed).
void DefineVariable(const std::string& name, uint32_t address);

// NTSTATUS values.
constexpr uint32_t kStatusSuccess = 0x00000000;
constexpr uint32_t kStatusAbandoned = 0x00000080;
constexpr uint32_t kStatusUserApc = 0x000000C0;
constexpr uint32_t kStatusTimeout = 0x00000102;
constexpr uint32_t kStatusObjectNameExists = 0x40000000;
constexpr uint32_t kStatusInvalidHandle = 0xC0000008;
constexpr uint32_t kStatusInvalidParameter = 0xC000000D;
constexpr uint32_t kStatusNoMemory = 0xC0000017;
constexpr uint32_t kStatusObjectTypeMismatch = 0xC0000024;
constexpr uint32_t kStatusMutantNotOwned = 0xC0000046;
constexpr uint32_t kStatusSemaphoreLimitExceeded = 0xC0000047;
constexpr uint32_t kStatusNoMoreFiles = 0x80000006;
constexpr uint32_t kStatusBufferOverflow = 0x80000005;
constexpr uint32_t kStatusInvalidInfoClass = 0xC0000003;
constexpr uint32_t kStatusInfoLengthMismatch = 0xC0000004;
constexpr uint32_t kStatusAccessViolation = 0xC0000005;
constexpr uint32_t kStatusNoSuchFile = 0xC000000F;
constexpr uint32_t kStatusInvalidDeviceRequest = 0xC0000010;
constexpr uint32_t kStatusEndOfFile = 0xC0000011;
constexpr uint32_t kStatusObjectNameNotFound = 0xC0000034;
constexpr uint32_t kStatusObjectNameCollision = 0xC0000035;
constexpr uint32_t kStatusObjectPathNotFound = 0xC000003A;
constexpr uint32_t kStatusMediaWriteProtected = 0xC00000A2;
constexpr uint32_t kStatusFileIsADirectory = 0xC00000BA;
constexpr uint32_t kStatusNotADirectory = 0xC0000103;

// ---- Threads (threads.cpp) ----

// Per-thread TLS block: staticSize bytes of the XEX's TLS template, then slotCount
// dynamic slots. The processor block (r13) points at its start.
struct TlsLayout {
    uint32_t staticSize = 0;
    uint32_t slotCount = 0;
    uint32_t templateAddress = 0;  // initial static data (templateSize bytes, the rest zeroed)
    uint32_t templateSize = 0;
};
void SetTlsLayout(const TlsLayout& layout);
const TlsLayout& GetTlsLayout();
void SetDefaultStackSize(uint32_t size);

struct ThreadStart {
    uint32_t entry = 0;  // guest function, called with r3 and r4
    uint32_t r3 = 0;
    uint32_t r4 = 0;
    uint32_t stackSize = 0;  // 0: the XEX's default
    uint32_t flags = 0;      // ExCreateThread's creation flags (bit 0: start suspended)
    bool main = false;       // the title's main thread: its return ends the run
};
// Creates a guest thread (processor block, KTHREAD, TLS, stack) and the host thread that
// runs it. Returns its KTHREAD with one reference for the caller, or 0.
uint32_t CreateGuestThread(const ThreadStart& start, uint32_t* threadId);

// The calling thread's KTHREAD, or 0 on a host-only thread.
uint32_t CurrentThread();
// Small sequential number of the calling guest thread for logs (1 = main), or 0.
uint32_t CurrentThreadNumber();
// The calling guest thread's registers, or nullptr on a host-only thread.
PPCContext* CurrentContext();
// Names a guest thread (by thread ID; 0xFFFFFFFF: the caller) in reports.
void SetThreadName(uint32_t threadId, const std::string& name);

// Calls a guest function from kernel code on the calling thread; ctx is restored after.
void CallGuest(PPCContext& ctx, uint8_t* base, uint32_t function, uint32_t r3, uint32_t r4, uint32_t r5);

// Records what the calling thread is blocked in (nullptr: running again), for reports.
void SetWaitState(const char* caller, uint32_t object);

// One line per guest thread: entry, state, registers. Never blocks.
std::string DescribeThreads();

// ---- Kernel objects, handles and waits (dispatcher.cpp) ----

// Dispatcher object types: the first byte of every waitable object's header.
//   +0 u8 type, +1 u8 absolute, +2 u8 size, +3 u8 inserted, +4 s32 signal state, +8 wait list
enum ObjectType : uint8_t {
    kNotificationEvent = 0,
    kSynchronizationEvent = 1,
    kMutantObject = 2,
    kSemaphoreObject = 5,
    kThreadObject = 6,
    kNotificationTimer = 8,
    kSynchronizationTimer = 9,
    // The runtime's tag for file objects: waitable, and signaled while no I/O is pending
    // (always, since I/O completes before the call returns).
    kFileObject = 0x40,
};

// An ANSI_STRING (u16 length, u16 maximum length, char* buffer); "" for a null pointer.
std::string AnsiString(const uint8_t* base, uint32_t string);
// The name in an OBJECT_ATTRIBUTES (root directory, ANSI_STRING* name, attributes).
std::string ObjectName(const uint8_t* base, uint32_t attributes);

// A zeroed kernel object in system memory with one reference and its dispatcher header
// set. `cleanup` runs when the last reference goes. Returns 0 when system memory is full.
using ObjectCleanup = void (*)(uint32_t object);
uint32_t NewObject(uint32_t size, uint8_t type, int32_t signalState, ObjectCleanup cleanup = nullptr);
// References count only for objects from NewObject; others belong to the title.
void ReferenceObject(uint32_t object);
void DereferenceObject(uint32_t object);

// CreateHandle takes a reference; LookupHandle resolves the current-thread pseudo-handle
// and returns 0 for a bad handle.
uint32_t CreateHandle(uint32_t object);
uint32_t LookupHandle(uint32_t handle);
bool CloseHandle(uint32_t handle);

// Sets an object's signal state and wakes threads waiting on it. Returns the old state.
int32_t SignalObject(uint32_t object, int32_t state);

// Queues a user APC (routine(r3, r4, r5)) for a guest thread; it runs during that
// thread's next alertable wait.
void QueueUserApc(uint32_t thread, uint32_t routine, uint32_t r3, uint32_t r4, uint32_t r5);

// When a wait gives up; no value waits forever.
using Deadline = std::optional<std::chrono::steady_clock::time_point>;

// Waits as the calling guest thread for any or all of `objects` (none: just sleeps).
// Returns STATUS_WAIT_0 + index, kStatusTimeout or kStatusUserApc.
uint32_t WaitForObjects(PPCContext& ctx, uint8_t* base, const char* caller, const uint32_t* objects, uint32_t count,
                        bool waitAll, bool alertable, const Deadline& deadline);

// ---- Files (file_imports.cpp) ----

// The game disc, served as d:\, game:\ and \Device\Cdrom0.
void MountDisc(const Disc* disc);

// ---- Time (time.cpp) ----

// System time: 100 ns units since 1601, like FILETIME.
uint64_t SystemTime();
// When an NT timeout expires: negative is relative (100 ns units), positive absolute.
std::chrono::steady_clock::time_point DeadlineFor(int64_t timeout);
// The deadline for a guest s64* timeout argument (null: none).
Deadline GuestTimeout(const uint8_t* base, uint32_t timeoutPtr);
// The deadline for a timeout in milliseconds (0xFFFFFFFF: none).
Deadline MillisecondTimeout(uint32_t milliseconds);
// Starts the clock thread (time stamp bundle, timers). Called by Init.
void StartClock();
// Where the title's KeTimeStampBundle lives; the clock keeps it current.
void SetTimeStampBundle(uint32_t address);

// ---- Tracing and reports (trace.cpp) ----

void Log(const char* format, ...);

// Records one kernel call (name, first arguments, return address).
void Trace(PPCContext& ctx, const char* name, bool implemented);

// What every import we haven't written yet does: trace it and return 0 (STATUS_SUCCESS).
void UnimplementedImport(PPCContext& ctx, uint8_t* base, const char* name);

// Prints why the run ended, every guest thread's state, the last kernel calls and a
// per-import summary, then exits the process.
[[noreturn]] void Finish(const char* reason, int exitCode);

}  // namespace s3n::kernel
