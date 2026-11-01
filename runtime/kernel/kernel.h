#pragma once

// The console side of the runtime: kernel/XAM imports, tracing and run reports.

#include <ppc_config.h>
#include <ppc_context.h>

#include <cstdint>
#include <cstdio>

namespace s3n {
class GuestMemory;
}

namespace s3n::kernel {

struct Options {
    std::FILE* log = nullptr;
    uint32_t stallSeconds = 15;  // end the run if no kernel call happens for this long
    uint32_t timeLimitSeconds = 120;
};

// Installs the fault handler and starts the watchdog.
void Init(GuestMemory& memory, const Options& options);
void SetMainContext(PPCContext* ctx);
GuestMemory& Memory();

// Per-thread TLS block: staticSize bytes of the XEX's TLS template, then slotCount
// dynamic slots. The processor block (r13) points at its start.
struct TlsLayout {
    uint32_t staticSize = 0;
    uint32_t slotCount = 0;
};
void SetTlsLayout(const TlsLayout& layout);
const TlsLayout& GetTlsLayout();

// A new handle for a kernel object.
uint32_t NewHandle();

// NTSTATUS values the kernel functions return.
constexpr uint32_t kStatusSuccess = 0x00000000;
constexpr uint32_t kStatusInvalidParameter = 0xC000000D;
constexpr uint32_t kStatusNoMemory = 0xC0000017;

void Log(const char* format, ...);

// Records one kernel call (name, first arguments, return address).
void Trace(PPCContext& ctx, const char* name, bool implemented);

// What every import we haven't written yet does: trace it and return 0 (STATUS_SUCCESS).
void UnimplementedImport(PPCContext& ctx, uint8_t* base, const char* name);

// Prints why the run ended, the main thread's registers, the last kernel calls and a
// per-import summary, then exits the process.
[[noreturn]] void Finish(const char* reason, int exitCode);

}  // namespace s3n::kernel
