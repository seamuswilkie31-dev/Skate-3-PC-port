// Dynamic thread-local storage (KeTlsAlloc and friends).
//
// Slot numbers are process-wide; each thread's values live in its own TLS block, right
// after the static TLS data that the processor block (r13 + 0) points at.

#include "kernel/kernel.h"
#include "memory.h"

#include <mutex>
#include <vector>

namespace s3n::kernel {
namespace {

constexpr uint32_t kTlsOutOfIndexes = 0xFFFFFFFF;

std::mutex gTlsMutex;
std::vector<bool> gTlsUsed;

// Guest address of the calling thread's slot, or 0 for a bad index.
uint32_t SlotAddress(PPCContext& ctx, uint8_t* base, uint32_t index) {
    const TlsLayout& layout = GetTlsLayout();
    if (index >= layout.slotCount) return 0;
    uint32_t tls = LoadBE32(base, ctx.r13.u32);  // processor block +0: TLS pointer
    return tls + layout.staticSize + index * 4;
}

}  // namespace
}  // namespace s3n::kernel

using namespace s3n;
using namespace s3n::kernel;

// u32 KeTlsAlloc(): lowest free slot, or 0xFFFFFFFF.
PPC_FUNC(__imp__KeTlsAlloc) {
    Trace(ctx, "KeTlsAlloc", true);
    std::lock_guard lock(gTlsMutex);
    gTlsUsed.resize(GetTlsLayout().slotCount);
    uint32_t index = kTlsOutOfIndexes;
    for (uint32_t i = 0; i < gTlsUsed.size(); ++i) {
        if (!gTlsUsed[i]) {
            gTlsUsed[i] = true;
            index = i;
            break;
        }
    }
    ctx.r3.u64 = index;
}

// BOOL KeTlsFree(u32 index)
PPC_FUNC(__imp__KeTlsFree) {
    Trace(ctx, "KeTlsFree", true);
    std::lock_guard lock(gTlsMutex);
    uint32_t index = ctx.r3.u32;
    bool ok = index < gTlsUsed.size() && gTlsUsed[index];
    if (ok) gTlsUsed[index] = false;
    ctx.r3.u64 = ok;
}

// u32 KeTlsGetValue(u32 index)
PPC_FUNC(__imp__KeTlsGetValue) {
    Trace(ctx, "KeTlsGetValue", true);
    uint32_t slot = SlotAddress(ctx, base, ctx.r3.u32);
    ctx.r3.u64 = slot ? LoadBE32(base, slot) : 0;
}

// BOOL KeTlsSetValue(u32 index, u32 value)
PPC_FUNC(__imp__KeTlsSetValue) {
    Trace(ctx, "KeTlsSetValue", true);
    uint32_t slot = SlotAddress(ctx, base, ctx.r3.u32);
    if (slot) StoreBE32(base, slot, ctx.r4.u32);
    ctx.r3.u64 = slot != 0;
}
