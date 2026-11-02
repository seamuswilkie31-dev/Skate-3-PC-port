// XAM's users: one local (offline) profile, signed in on user 0 and named after the
// launcher's player name, so pressing Start has someone to play as. Achievements and other
// enumerations are empty for now; real profiles and saves are phase 5.

#include "config.h"
#include "kernel/kernel.h"
#include "memory.h"

#include <algorithm>
#include <cstring>
#include <string>

using namespace s3n;
using namespace s3n::kernel;

namespace {

constexpr uint32_t kErrorNoMoreFiles = 0x12;
constexpr uint32_t kErrorNoSuchUser = 0x525;
constexpr uint32_t kSignedInLocally = 1;
// Offline profiles' XUIDs start with 0xE000. Fixed for now; an imported save's profile
// will choose it later.
constexpr uint64_t kLocalXuid = 0xE000000000000001ull;
constexpr uint32_t kEnumeratorTag = 0x454E554D;  // "ENUM", after the dispatcher header
constexpr uint32_t kAchievementSize = 0x38;      // what a title's buffer holds per achievement

bool SignedIn(uint32_t user) { return user == 0; }

const std::string& PlayerName() {
    static const std::string name = [] {
        Config config;
        LoadConfig(ConfigPath(), config);
        return config.net.playerName.empty() ? std::string("Skater") : config.net.playerName.substr(0, 15);
    }();
    return name;
}

// An enumeration with nothing in it: a handle XamEnumerate recognises, closed with NtClose.
uint32_t NewEnumerator() {
    uint32_t object = NewObject(0x20, kNotificationEvent, 1);
    if (!object) return 0;
    StoreBE32(Memory().base(), object + 0x10, kEnumeratorTag);
    uint32_t handle = CreateHandle(object);
    DereferenceObject(object);
    return handle;
}

}  // namespace

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
    ctx.r3.u64 = 0;
}

// u32 XamUserGetXUID(u32 user, u32 type, XUID* xuid)
PPC_FUNC(__imp__XamUserGetXUID) {
    Trace(ctx, "XamUserGetXUID", true);
    if (!SignedIn(ctx.r3.u32) || !ctx.r5.u32) {
        ctx.r3.u64 = kErrorNoSuchUser;
        return;
    }
    StoreBE64(base, ctx.r5.u32, kLocalXuid);
    ctx.r3.u64 = 0;
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
    ctx.r3.u64 = 0;
}

// u32 XamUserCreateAchievementEnumerator(u32 title, u32 user, XUID xuid, u32 flags,
//         u32 start, u32 count, u32* bufferSize, HANDLE* enumerator)
// An empty enumeration, with room for `count` achievements all the same.
PPC_FUNC(__imp__XamUserCreateAchievementEnumerator) {
    Trace(ctx, "XamUserCreateAchievementEnumerator", true);
    uint32_t count = ctx.r8.u32, bufferSize = ctx.r9.u32, out = ctx.r10.u32;
    if (bufferSize) StoreBE32(base, bufferSize, std::max(count, 1u) * kAchievementSize);
    uint32_t handle = NewEnumerator();
    if (out) StoreBE32(base, out, handle);
    ctx.r3.u64 = handle ? 0 : 0x8;  // ERROR_NOT_ENOUGH_MEMORY
}

// u32 XamEnumerate(HANDLE enumerator, u32 flags, void* buffer, u32 size, u32* returned,
//         XOVERLAPPED* overlapped)
// Every enumeration is empty so far: no more items, at once (the overlapped completes too).
PPC_FUNC(__imp__XamEnumerate) {
    Trace(ctx, "XamEnumerate", true);
    uint32_t returned = ctx.r7.u32, overlapped = ctx.r8.u32;
    if (returned) StoreBE32(base, returned, 0);
    if (overlapped) {
        StoreBE32(base, overlapped, kErrorNoMoreFiles);       // InternalLow: the result
        StoreBE32(base, overlapped + 4, 0);                   // InternalHigh: items
        StoreBE32(base, overlapped + 0x18, kErrorNoMoreFiles);  // extended error
        if (uint32_t event = LoadBE32(base, overlapped + 0xC))
            if (uint32_t object = LookupHandle(event)) SignalObject(object, 1);
    }
    ctx.r3.u64 = kErrorNoMoreFiles;
}
