// XAM's users: one local (offline) profile, signed in on user 0 and named after the
// launcher's player name, so pressing Start has someone to play as. It has no
// achievements yet.

#include "config.h"
#include "kernel/kernel.h"
#include "kernel/xam.h"
#include "memory.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace s3n::kernel {

namespace {
constexpr uint32_t kSignedInLocally = 1;
constexpr uint32_t kAchievementSize = 0x38;  // what a title's buffer holds per achievement
}  // namespace

bool SignedIn(uint32_t user) { return user == 0; }

const std::string& PlayerName() {
    static const std::string name = [] {
        Config config;
        LoadConfig(ConfigPath(), config);
        return config.net.playerName.empty() ? std::string("Skater") : config.net.playerName.substr(0, 15);
    }();
    return name;
}

}  // namespace s3n::kernel

using namespace s3n;
using namespace s3n::kernel;

// u32 XamUserGetSigninState(u32 user)
PPC_FUNC(__imp__XamUserGetSigninState) {
    Trace(ctx, "XamUserGetSigninState", true);
    ctx.r3.u64 = SignedIn(ctx.r3.u32) ? kSignedInLocally : 0;
}

// u32 XamUserGetSigninInfo(u32 user, u32 flags, XUSER_SIGNIN_INFO* info)
// XUSER_SIGNIN_INFO: XUID, info flags, sign-in state, guest number, sponsor user, name[16].
PPC_FUNC(__imp__XamUserGetSigninInfo) {
    Trace(ctx, "XamUserGetSigninInfo", true);
    uint32_t user = ctx.r3.u32, info = ctx.r5.u32;
    if (!SignedIn(user) || !info) {
        ctx.r3.u64 = kErrorNoSuchUser;
        return;
    }
    std::memset(base + info, 0, 40);
    StoreBE64(base, info, kLocalXuid);
    StoreBE32(base, info + 12, kSignedInLocally);
    std::memcpy(base + info + 24, PlayerName().data(), PlayerName().size());
    ctx.r3.u64 = kErrorSuccess;
}

// u32 XamUserGetXUID(u32 user, u32 type, XUID* xuid)
PPC_FUNC(__imp__XamUserGetXUID) {
    Trace(ctx, "XamUserGetXUID", true);
    if (!SignedIn(ctx.r3.u32) || !ctx.r5.u32) {
        ctx.r3.u64 = kErrorNoSuchUser;
        return;
    }
    StoreBE64(base, ctx.r5.u32, kLocalXuid);
    ctx.r3.u64 = kErrorSuccess;
}

// u32 XamUserGetName(u32 user, char* name, u32 size)
PPC_FUNC(__imp__XamUserGetName) {
    Trace(ctx, "XamUserGetName", true);
    uint32_t name = ctx.r4.u32, size = ctx.r5.u32;
    if (!SignedIn(ctx.r3.u32) || !name || !size) {
        ctx.r3.u64 = kErrorNoSuchUser;
        return;
    }
    size_t length = std::min<size_t>(PlayerName().size(), size - 1);
    std::memcpy(base + name, PlayerName().data(), length);
    base[name + length] = 0;
    ctx.r3.u64 = kErrorSuccess;
}

// u32 XamUserCreateAchievementEnumerator(u32 title, u32 user, XUID xuid, u32 flags,
//         u32 start, u32 count, u32* bufferSize, HANDLE* enumerator)
// An empty enumeration, with room for `count` achievements all the same.
PPC_FUNC(__imp__XamUserCreateAchievementEnumerator) {
    Trace(ctx, "XamUserCreateAchievementEnumerator", true);
    uint32_t count = ctx.r8.u32, bufferSize = ctx.r9.u32, out = ctx.r10.u32;
    if (bufferSize) StoreBE32(base, bufferSize, std::max(count, 1u) * kAchievementSize);
    uint32_t handle = NewEnumerator(kAchievementSize, {});
    if (out) StoreBE32(base, out, handle);
    ctx.r3.u64 = handle ? kErrorSuccess : 0x8;  // ERROR_NOT_ENOUGH_MEMORY
}

// u32 XamUserReadProfileSettings(u32 title, u32 user, u32 xuidCount, XUID* xuids,
//         u32 settingCount, u32* settingIds, u32* bufferSize,
//         XUSER_READ_PROFILE_SETTING_RESULT* results, XOVERLAPPED* overlapped)
// The ninth argument is on the stack. A setting ID holds its type (bits 28-31: 1 int32,
// 2 int64, 3 double, 4 string, 5 float, 6 binary, 7 time) and its largest size (16-27).
// Nothing is stored yet, so every setting has its default: the title's own settings
// (title-specific 1-3) have no value, the rest are zero, apart from vibration (on).
// The result: count, pointer to the settings, then XUSER_PROFILE_SETTING each (source,
// user index or XUID, ID, then XUSER_DATA: type, and the value at +8), then their data.
PPC_FUNC(__imp__XamUserReadProfileSettings) {
    Trace(ctx, "XamUserReadProfileSettings", true);
    constexpr uint32_t kSettingSize = 0x28, kNoValue = 0, kDefault = 1;
    constexpr uint32_t kVibration = 0x10040003, kVibrationOn = 3;
    uint32_t user = ctx.r4.u32, xuidCount = ctx.r5.u32, xuids = ctx.r6.u32;
    uint32_t settingCount = ctx.r7.u32, ids = ctx.r8.u32, bufferSize = ctx.r9.u32, results = ctx.r10.u32;
    uint32_t overlapped = LoadBE32(base, ctx.r1.u32 + 0x54);
    if (!xuidCount && !SignedIn(user)) {
        ctx.r3.u64 = kErrorNoSuchUser;
        return;
    }
    uint32_t owners = std::max(xuidCount, 1u), count = owners * settingCount;
    uint32_t required = 8 + count * kSettingSize;
    for (uint32_t i = 0; i < settingCount; ++i) {
        uint32_t id = LoadBE32(base, ids + i * 4), type = id >> 28;
        if (type == 4 || type == 6) required += owners * ((id >> 16 & 0xFFF) + 3 & ~3u);
    }
    uint32_t available = bufferSize ? LoadBE32(base, bufferSize) : 0;
    if (!results || available < required) {
        if (bufferSize) StoreBE32(base, bufferSize, required);
        ctx.r3.u64 = kErrorInsufficientBuffer;
        return;
    }
    std::memset(base + results, 0, required);
    StoreBE32(base, results, count);
    StoreBE32(base, results + 4, results + 8);
    uint32_t data = results + 8 + count * kSettingSize;
    for (uint32_t owner = 0; owner < owners; ++owner) {
        for (uint32_t i = 0; i < settingCount; ++i) {
            uint32_t setting = results + 8 + (owner * settingCount + i) * kSettingSize;
            uint32_t id = LoadBE32(base, ids + i * 4), type = id >> 28, maxSize = id >> 16 & 0xFFF;
            bool titleSpecific = (id & 0xFFFF) >= 0x3FFD;
            StoreBE32(base, setting, titleSpecific ? kNoValue : kDefault);
            if (xuidCount) StoreBE64(base, setting + 8, LoadBE64(base, xuids + owner * 8));
            else StoreBE32(base, setting + 8, user);
            StoreBE32(base, setting + 0x10, id);
            base[setting + 0x18] = uint8_t(type);
            if (type == 4 || type == 6) {  // size, then a pointer to (empty) data
                StoreBE32(base, setting + 0x24, data);
                data += maxSize + 3 & ~3u;
            } else if (id == kVibration) {
                StoreBE32(base, setting + 0x20, kVibrationOn);
            }
        }
    }
    if (bufferSize) StoreBE32(base, bufferSize, required);
    ctx.r3.u64 = CompleteOverlapped(overlapped, kErrorSuccess);
}

// u32 XamUserCheckPrivilege(u32 user, u32 privilege, BOOL* result): everything is allowed.
PPC_FUNC(__imp__XamUserCheckPrivilege) {
    Trace(ctx, "XamUserCheckPrivilege", true);
    if (ctx.r5.u32) StoreBE32(base, ctx.r5.u32, 1);
    ctx.r3.u64 = SignedIn(ctx.r3.u32) ? kErrorSuccess : kErrorNoSuchUser;
}