// XNet / Winsock (xam.xex NetDll_*). Only the event functions so far: WSA events are
// ordinary kernel events, and the title's network thread waits on them.

#include "kernel/kernel.h"
#include "memory.h"

using namespace s3n;
using namespace s3n::kernel;

namespace {

constexpr uint32_t kWsaWaitFailed = 0xFFFFFFFF;
constexpr uint32_t kEventSize = 0x10;

}  // namespace

// HANDLE NetDll_WSACreateEvent(): a manual-reset event, not signaled.
PPC_FUNC(__imp__NetDll_WSACreateEvent) {
    Trace(ctx, "NetDll_WSACreateEvent", true);
    uint32_t event = NewObject(kEventSize, kNotificationEvent, 0);
    if (!event) {
        ctx.r3.u64 = 0;
        return;
    }
    ctx.r3.u64 = CreateHandle(event);
    DereferenceObject(event);
}

// BOOL NetDll_WSACloseEvent(HANDLE event)
PPC_FUNC(__imp__NetDll_WSACloseEvent) {
    Trace(ctx, "NetDll_WSACloseEvent", true);
    ctx.r3.u64 = CloseHandle(ctx.r3.u32) ? 1 : 0;
}

// BOOL NetDll_WSASetEvent(HANDLE event)
PPC_FUNC(__imp__NetDll_WSASetEvent) {
    Trace(ctx, "NetDll_WSASetEvent", true);
    uint32_t event = LookupHandle(ctx.r3.u32);
    if (event) SignalObject(event, 1);
    ctx.r3.u64 = event ? 1 : 0;
}

// BOOL NetDll_WSAResetEvent(HANDLE event)
PPC_FUNC(__imp__NetDll_WSAResetEvent) {
    Trace(ctx, "NetDll_WSAResetEvent", true);
    uint32_t event = LookupHandle(ctx.r3.u32);
    if (event) SignalObject(event, 0);
    ctx.r3.u64 = event ? 1 : 0;
}

// u32 NetDll_WSAWaitForMultipleEvents(u32 count, HANDLE* events, BOOL waitAll,
//                                     u32 timeoutMs, BOOL alertable)
// Returns WSA_WAIT_EVENT_0 + index, WSA_WAIT_TIMEOUT (0x102) or WSA_WAIT_IO_COMPLETION (0xC0),
// the same values as the kernel's wait statuses.
PPC_FUNC(__imp__NetDll_WSAWaitForMultipleEvents) {
    Trace(ctx, "NetDll_WSAWaitForMultipleEvents", true);
    uint32_t count = ctx.r3.u32;
    if (count == 0 || count > 64) {
        ctx.r3.u64 = kWsaWaitFailed;
        return;
    }
    uint32_t objects[64];
    for (uint32_t i = 0; i < count; ++i) {
        objects[i] = LookupHandle(LoadBE32(base, ctx.r4.u32 + i * 4));
        if (!objects[i]) {
            ctx.r3.u64 = kWsaWaitFailed;
            return;
        }
    }
    ctx.r3.u64 = WaitForObjects(ctx, base, "NetDll_WSAWaitForMultipleEvents", objects, count, ctx.r5.u32 != 0,
                                ctx.r7.u32 != 0, MillisecondTimeout(ctx.r6.u32));
}
