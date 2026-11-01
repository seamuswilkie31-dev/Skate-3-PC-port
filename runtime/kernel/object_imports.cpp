// Kernel synchronization objects.
//
// For now every object is a handle with no state behind it, and waits succeed at once.
// That holds while the title runs a single thread; real objects come with threads.

#include "kernel/kernel.h"
#include "memory.h"

using namespace s3n;
using namespace s3n::kernel;

namespace {

// The Nt*Create functions all take the handle out-pointer first.
void CreateObject(PPCContext& ctx, uint8_t* base, const char* name) {
    Trace(ctx, name, true);
    StoreBE32(base, ctx.r3.u32, NewHandle());
    ctx.r3.u64 = kStatusSuccess;
}

}  // namespace

// NTSTATUS NtCreateMutant(HANDLE* out, OBJECT_ATTRIBUTES* attributes, BOOL initialOwner)
PPC_FUNC(__imp__NtCreateMutant) { CreateObject(ctx, base, "NtCreateMutant"); }

// NTSTATUS NtCreateSemaphore(HANDLE* out, OBJECT_ATTRIBUTES* attributes, s32 initial, s32 maximum)
PPC_FUNC(__imp__NtCreateSemaphore) { CreateObject(ctx, base, "NtCreateSemaphore"); }

// NTSTATUS NtCreateEvent(HANDLE* out, OBJECT_ATTRIBUTES* attributes, u32 type, BOOL initialState)
PPC_FUNC(__imp__NtCreateEvent) { CreateObject(ctx, base, "NtCreateEvent"); }

// NTSTATUS NtWaitForSingleObjectEx(HANDLE handle, u32 waitMode, BOOL alertable, s64* timeout)
PPC_FUNC(__imp__NtWaitForSingleObjectEx) {
    Trace(ctx, "NtWaitForSingleObjectEx", true);
    ctx.r3.u64 = kStatusSuccess;  // signaled (STATUS_WAIT_0)
}
