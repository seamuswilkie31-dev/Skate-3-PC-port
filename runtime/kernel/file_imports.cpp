// Files. Not served yet: every open logs its path and fails with "not found", so the
// title sees a missing file instead of a fake handle. Serving game:\ from the ISO is next.

#include "kernel/kernel.h"
#include "memory.h"

using namespace s3n;
using namespace s3n::kernel;

namespace {

// Fails an open: clears the handle and fills the IO_STATUS_BLOCK (status, information).
uint32_t FailOpen(uint8_t* base, uint32_t handlePtr, uint32_t ioStatus, const char* caller, uint32_t attributes) {
    Log("  %s \"%s\": not found (files aren't served yet)\n", caller, ObjectName(base, attributes).c_str());
    if (handlePtr) StoreBE32(base, handlePtr, 0);
    if (ioStatus) {
        StoreBE32(base, ioStatus, kStatusObjectNameNotFound);
        StoreBE32(base, ioStatus + 4, 0);
    }
    return kStatusObjectNameNotFound;
}

}  // namespace

// NTSTATUS NtCreateFile(HANDLE* out, u32 access, OBJECT_ATTRIBUTES* attributes,
//                       IO_STATUS_BLOCK* ioStatus, s64* allocationSize, u32 fileAttributes,
//                       u32 shareAccess, u32 disposition, u32 options)
PPC_FUNC(__imp__NtCreateFile) {
    Trace(ctx, "NtCreateFile", true);
    ctx.r3.u64 = FailOpen(base, ctx.r3.u32, ctx.r6.u32, "NtCreateFile", ctx.r5.u32);
}

// NTSTATUS NtOpenFile(HANDLE* out, u32 access, OBJECT_ATTRIBUTES* attributes,
//                     IO_STATUS_BLOCK* ioStatus, u32 shareAccess, u32 options)
PPC_FUNC(__imp__NtOpenFile) {
    Trace(ctx, "NtOpenFile", true);
    ctx.r3.u64 = FailOpen(base, ctx.r3.u32, ctx.r6.u32, "NtOpenFile", ctx.r5.u32);
}

// NTSTATUS NtQueryFullAttributesFile(OBJECT_ATTRIBUTES* attributes, FILE_NETWORK_OPEN_INFORMATION* info)
PPC_FUNC(__imp__NtQueryFullAttributesFile) {
    Trace(ctx, "NtQueryFullAttributesFile", true);
    ctx.r3.u64 = FailOpen(base, 0, 0, "NtQueryFullAttributesFile", ctx.r3.u32);
}
