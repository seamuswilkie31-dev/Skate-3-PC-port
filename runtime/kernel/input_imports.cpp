// XAM's controller functions, answered from runtime/input. The structures are XInput's,
// big-endian.

#include "input/input.h"
#include "kernel/kernel.h"
#include "memory.h"

#include <cstring>

using namespace s3n;
using namespace s3n::kernel;

namespace {

constexpr uint32_t kErrorDeviceNotConnected = 0x48F;
constexpr uint32_t kAnyUser = 0xFF;

void StoreBE16(uint8_t* base, uint32_t address, uint16_t value) {
    base[address] = uint8_t(value >> 8);
    base[address + 1] = uint8_t(value);
}

// XINPUT_GAMEPAD: buttons, left and right trigger, then the four stick axes.
void StoreGamepad(uint8_t* base, uint32_t address, const input::Gamepad& pad) {
    StoreBE16(base, address, pad.buttons);
    base[address + 2] = pad.leftTrigger;
    base[address + 3] = pad.rightTrigger;
    StoreBE16(base, address + 4, uint16_t(pad.thumbLX));
    StoreBE16(base, address + 6, uint16_t(pad.thumbLY));
    StoreBE16(base, address + 8, uint16_t(pad.thumbRX));
    StoreBE16(base, address + 10, uint16_t(pad.thumbRY));
}

uint32_t User(uint32_t user) { return user == kAnyUser ? 0 : user; }

}  // namespace

// u32 XamInputGetCapabilities(u32 user, u32 flags, XINPUT_CAPABILITIES* capabilities)
// A standard gamepad: every button, full-range triggers and sticks, two motors.
PPC_FUNC(__imp__XamInputGetCapabilities) {
    Trace(ctx, "XamInputGetCapabilities", true);
    uint32_t user = User(ctx.r3.u32), out = ctx.r5.u32;
    if (!input::Connected(user) || !out) {
        ctx.r3.u64 = kErrorDeviceNotConnected;
        return;
    }
    std::memset(base + out, 0, 20);
    base[out] = 1;      // type: gamepad
    base[out + 1] = 1;  // subtype: gamepad
    input::Gamepad all;
    all.buttons = 0xF3FF;
    all.leftTrigger = all.rightTrigger = 0xFF;
    all.thumbLX = all.thumbLY = all.thumbRX = all.thumbRY = int16_t(0xFFC0);
    StoreGamepad(base, out + 4, all);
    StoreBE16(base, out + 16, 0xFFFF);  // both motors
    StoreBE16(base, out + 18, 0xFFFF);
    ctx.r3.u64 = 0;
}

// u32 XamInputGetState(u32 user, u32 flags, XINPUT_STATE* state)
PPC_FUNC(__imp__XamInputGetState) {
    Trace(ctx, "XamInputGetState", true);
    uint32_t user = User(ctx.r3.u32), out = ctx.r5.u32;
    input::Gamepad pad;
    uint32_t packet = 0;
    if (!out || !input::State(user, pad, packet)) {
        ctx.r3.u64 = kErrorDeviceNotConnected;
        return;
    }
    StoreBE32(base, out, packet);
    StoreGamepad(base, out + 4, pad);
    ctx.r3.u64 = 0;
}

// u32 XamInputSetState(u32 user, u32 unknown, XINPUT_VIBRATION* vibration)
PPC_FUNC(__imp__XamInputSetState) {
    Trace(ctx, "XamInputSetState", true);
    uint32_t user = User(ctx.r3.u32), in = ctx.r5.u32;
    if (!input::Connected(user)) {
        ctx.r3.u64 = kErrorDeviceNotConnected;
        return;
    }
    if (in) input::SetVibration(user, uint16_t(base[in] << 8 | base[in + 1]), uint16_t(base[in + 2] << 8 | base[in + 3]));
    ctx.r3.u64 = 0;
}
