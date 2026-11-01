// Kernel runtime library: strings and exceptions.

#include "kernel/kernel.h"
#include "memory.h"

#include <cstring>

namespace {

// The exception code debuggers (and Visual Studio's SetThreadName) use to name threads.
constexpr uint32_t kSetThreadNameException = 0x406D1388;

}  // namespace

using namespace s3n;
using namespace s3n::kernel;

// void RtlInitAnsiString(ANSI_STRING* string, const char* source)
// ANSI_STRING: u16 length, u16 maximum length (with the terminator), char* buffer.
PPC_FUNC(__imp__RtlInitAnsiString) {
    Trace(ctx, "RtlInitAnsiString", true);
    uint32_t string = ctx.r3.u32, source = ctx.r4.u32;
    uint16_t length = source ? uint16_t(std::strlen(reinterpret_cast<const char*>(base + source))) : 0;
    uint16_t maximum = source ? uint16_t(length + 1) : 0;
    base[string + 0] = uint8_t(length >> 8);
    base[string + 1] = uint8_t(length);
    base[string + 2] = uint8_t(maximum >> 8);
    base[string + 3] = uint8_t(maximum);
    StoreBE32(base, string + 4, source);
}

// void RtlFillMemoryUlong(void* destination, u32 length, u32 pattern): length in bytes.
PPC_FUNC(__imp__RtlFillMemoryUlong) {
    Trace(ctx, "RtlFillMemoryUlong", true);
    uint32_t destination = ctx.r3.u32, words = ctx.r4.u32 / 4, pattern = ctx.r5.u32;
    for (uint32_t i = 0; i < words; ++i) StoreBE32(base, destination + i * 4, pattern);
}

// void RtlRaiseException(EXCEPTION_RECORD* record)
// EXCEPTION_RECORD: code, flags, nested record, address, parameter count, parameters[15].
// The title raises the SetThreadName exception to name each thread it creates; with no
// debugger attached that's a no-op, and here it names the thread in reports. Any other
// exception would need structured exception handling, which the runtime doesn't have yet.
PPC_FUNC(__imp__RtlRaiseException) {
    Trace(ctx, "RtlRaiseException", true);
    uint32_t record = ctx.r3.u32;
    uint32_t code = LoadBE32(base, record);
    if (code == kSetThreadNameException && LoadBE32(base, record + 0x10) >= 3 && LoadBE32(base, record + 0x14) == 0x1000) {
        uint32_t name = LoadBE32(base, record + 0x18), threadId = LoadBE32(base, record + 0x1C);
        if (name) {
            std::string text(reinterpret_cast<const char*>(base + name), strnlen(reinterpret_cast<const char*>(base + name), 64));
            SetThreadName(threadId, text);
            Log("  thread %d named \"%s\"\n", int32_t(threadId), text.c_str());
        }
        return;
    }
    char reason[128];
    std::snprintf(reason, sizeof reason, "t%u raised exception %08X (flags %08X), and there's no SEH support yet",
                  CurrentThreadNumber(), code, LoadBE32(base, record + 4));
    Finish(reason, 7);
}
