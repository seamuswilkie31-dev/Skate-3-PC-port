// XAM's system UI: the dialogs the dashboard draws over a title. There's no dashboard, so
// each one answers at once, as a player taking the default would, and posts XN_SYS_UI as
// the dashboard does: shown at once, closed half a second later.

#include "kernel/kernel.h"
#include "kernel/xam.h"
#include "memory.h"

#include <algorithm>
#include <string>

using namespace s3n;
using namespace s3n::kernel;

namespace {

// A guest UTF-16 (big-endian) string, as host UTF-16.
std::u16string ReadWide(const uint8_t* base, uint32_t address) {
    std::u16string text;
    if (!address) return text;
    for (uint32_t i = 0; i < 4096; ++i) {
        char16_t c = char16_t(base[address + i * 2] << 8 | base[address + i * 2 + 1]);
        if (!c) break;
        text += c;
    }
    return text;
}

void WriteWide(uint8_t* base, uint32_t address, const std::u16string& text, uint32_t capacity) {
    if (!address || !capacity) return;
    size_t length = std::min<size_t>(text.size(), capacity - 1);
    for (size_t i = 0; i < length; ++i) {
        base[address + i * 2] = uint8_t(text[i] >> 8);
        base[address + i * 2 + 1] = uint8_t(text[i]);
    }
    base[address + length * 2] = base[address + length * 2 + 1] = 0;
}

}  // namespace

// u32 XamShowKeyboardUI(u32 user, u32 flags, wchar* defaultText, wchar* title,
//         wchar* description, wchar* result, u32 maxLength, XOVERLAPPED* overlapped)
// The on-screen keyboard: the title's suggestion is accepted as typed, or the player's
// name when it suggests nothing.
PPC_FUNC(__imp__XamShowKeyboardUI) {
    Trace(ctx, "XamShowKeyboardUI", true);
    std::u16string text = ReadWide(base, ctx.r5.u32);
    if (text.empty()) text.assign(PlayerName().begin(), PlayerName().end());
    std::u16string title = ReadWide(base, ctx.r6.u32);
    uint32_t overlapped = ctx.r10.u32;
    Log("  t%u XamShowKeyboardUI \"%s\": answered \"%s\" (flags %X, %u characters, overlapped %08X: event %08X, "
        "routine %08X)\n", CurrentThreadNumber(), std::string(title.begin(), title.end()).c_str(),
        std::string(text.begin(), text.end()).c_str(), ctx.r4.u32, ctx.r9.u32, overlapped,
        overlapped ? LoadBE32(base, overlapped + 0xC) : 0, overlapped ? LoadBE32(base, overlapped + 0x10) : 0);
    WriteWide(base, ctx.r8.u32, text, ctx.r9.u32);
    Notify(kXnSysUi, 1);
    ctx.r3.u64 = CompleteOverlapped(ctx.r10.u32, kErrorSuccess);
    NotifyAfter(kXnSysUi, 0, kDialogClosingMilliseconds);
}

// u32 XamShowMessageBoxUIEx(u32 user, wchar* title, wchar* text, u32 buttonCount,
//         wchar** buttons, u32 focusButton, u32 flags, u32* result, XOVERLAPPED* overlapped)
// The player picks the button the title focused. The ninth argument is on the stack.
PPC_FUNC(__imp__XamShowMessageBoxUIEx) {
    Trace(ctx, "XamShowMessageBoxUIEx", true);
    uint32_t buttonCount = ctx.r6.u32, focus = ctx.r8.u32, result = ctx.r10.u32;
    uint32_t overlapped = LoadBE32(base, ctx.r1.u32 + 0x54);
    std::u16string text = ReadWide(base, ctx.r5.u32);
    uint32_t chosen = focus < buttonCount ? focus : 0;
    Log("  t%u XamShowMessageBoxUIEx \"%s\": button %u of %u\n", CurrentThreadNumber(),
        std::string(text.begin(), text.end()).substr(0, 120).c_str(), chosen, buttonCount);
    if (result) StoreBE32(base, result, chosen);
    Notify(kXnSysUi, 1);
    ctx.r3.u64 = CompleteOverlapped(overlapped, kErrorSuccess);
    NotifyAfter(kXnSysUi, 0, kDialogClosingMilliseconds);
}

// u32 XamShowSigninUI(u32 users, u32 flags): the profile is signed in already.
PPC_FUNC(__imp__XamShowSigninUI) {
    Trace(ctx, "XamShowSigninUI", true);
    Notify(kXnSysUi, 1);
    NotifyAfter(kXnSysUi, 0, kDialogClosingMilliseconds);
    ctx.r3.u64 = kErrorSuccess;
}