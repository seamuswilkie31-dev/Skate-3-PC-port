// XAM: the system services library titles link against (xam.xex).

#include "kernel/kernel.h"
#include "memory.h"

using namespace s3n;
using namespace s3n::kernel;

// HANDLE XamNotifyCreateListener(u64 areas, u32 version)
// A listener is waitable: it's signaled while notifications are queued. None are sent
// yet, so it stays unsignaled and XNotifyGetNext (still a stub) reports nothing.
PPC_FUNC(__imp__XamNotifyCreateListener) {
    Trace(ctx, "XamNotifyCreateListener", true);
    uint32_t listener = NewObject(0x10, kNotificationEvent, 0);
    if (!listener) {
        ctx.r3.u64 = 0;
        return;
    }
    ctx.r3.u64 = CreateHandle(listener);
    DereferenceObject(listener);
}
