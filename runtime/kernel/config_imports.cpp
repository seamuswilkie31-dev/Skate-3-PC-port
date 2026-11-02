// The console's settings (XConfig), as a freshly set-up console would have them: English,
// UTC with no daylight saving, and zero for everything else.

#include "kernel/kernel.h"
#include "memory.h"

#include <cstring>

using namespace s3n;
using namespace s3n::kernel;

namespace {

constexpr uint32_t kStatusBufferTooSmall = 0xC0000023;
constexpr uint16_t kUserCategory = 3;
constexpr uint16_t kUserLanguage = 9;
constexpr uint32_t kLanguageEnglish = 1;

}  // namespace

// NTSTATUS ExGetXConfigSetting(u16 category, u16 setting, void* buffer, u16 size, u16* required)
// Every setting the title reads is a 32-bit value.
PPC_FUNC(__imp__ExGetXConfigSetting) {
    Trace(ctx, "ExGetXConfigSetting", true);
    uint16_t category = uint16_t(ctx.r3.u32), setting = uint16_t(ctx.r4.u32);
    uint32_t buffer = ctx.r5.u32, size = ctx.r6.u32 & 0xFFFF, requiredOut = ctx.r7.u32;
    constexpr uint32_t kRequired = 4;
    if (requiredOut) {
        base[requiredOut] = 0;
        base[requiredOut + 1] = kRequired;
    }
    if (buffer) {
        if (size < kRequired) {
            ctx.r3.u64 = kStatusBufferTooSmall;
            return;
        }
        std::memset(base + buffer, 0, kRequired);
        if (category == kUserCategory && setting == kUserLanguage) StoreBE32(base, buffer, kLanguageEnglish);
    }
    ctx.r3.u64 = kStatusSuccess;
}
