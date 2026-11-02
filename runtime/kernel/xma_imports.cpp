// XMA* (xboxkrnl): the title's interface to the XMA audio decoder in apu/xma.cpp.
// Buffer addresses arrive as CPU addresses and are stored as physical ones, as the
// hardware reads them.

#include "apu/xma.h"
#include "kernel/kernel.h"
#include "memory.h"

using namespace s3n;
using namespace s3n::kernel;
using namespace s3n::apu;

namespace {

uint32_t PhysicalOf(uint32_t address) { return address ? Memory().ConsolePhysical(address) : 0; }

}  // namespace

// u32 XMACreateContext(void** context)
PPC_FUNC(__imp__XMACreateContext) {
    Trace(ctx, "XMACreateContext", true);
    uint32_t context = CreateContext();
    StoreBE32(base, ctx.r3.u32, context);
    ctx.r3.u64 = context ? kStatusSuccess : kStatusNoMemory;
}

// u32 XMAReleaseContext(void* context)
PPC_FUNC(__imp__XMAReleaseContext) {
    Trace(ctx, "XMAReleaseContext", true);
    ReleaseContext(ctx.r3.u32);
    ctx.r3.u64 = 0;
}

// u32 XMAInitializeContext(void* context, XMA_CONTEXT_INIT* init)
PPC_FUNC(__imp__XMAInitializeContext) {
    Trace(ctx, "XMAInitializeContext", true);
    InitializeContext(ctx.r3.u32, ctx.r4.u32);
    ctx.r3.u64 = 0;
}

// u32 XMAEnableContext(void* context)
PPC_FUNC(__imp__XMAEnableContext) {
    Trace(ctx, "XMAEnableContext", true);
    EnableContext(ctx.r3.u32, true);
    ctx.r3.u64 = 0;
}

// u32 XMADisableContext(void* context, BOOL wait): the decoder works under a lock, so a
// disabled context is already idle when this returns.
PPC_FUNC(__imp__XMADisableContext) {
    Trace(ctx, "XMADisableContext", true);
    EnableContext(ctx.r3.u32, false);
    ctx.r3.u64 = 0;
}

// u32 XMASetInputBuffer0(void* context, void* buffer, u32 packetCount), and buffer 1.
PPC_FUNC(__imp__XMASetInputBuffer0) {
    Trace(ctx, "XMASetInputBuffer0", true);
    SetField(ctx.r3.u32, kInput0Address, PhysicalOf(ctx.r4.u32));
    SetField(ctx.r3.u32, kInput0PacketCount, ctx.r5.u32);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__XMASetInputBuffer1) {
    Trace(ctx, "XMASetInputBuffer1", true);
    SetField(ctx.r3.u32, kInput1Address, PhysicalOf(ctx.r4.u32));
    SetField(ctx.r3.u32, kInput1PacketCount, ctx.r5.u32);
    ctx.r3.u64 = 0;
}

// BOOL XMAIsInputBuffer0Valid(void* context) / u32 XMASetInputBuffer0Valid(void* context),
// and the same for buffer 1: the title marks a filled buffer valid, the decoder clears the
// mark once it's used up.
PPC_FUNC(__imp__XMAIsInputBuffer0Valid) {
    Trace(ctx, "XMAIsInputBuffer0Valid", true);
    ctx.r3.u64 = GetField(ctx.r3.u32, kInput0Valid);
}
PPC_FUNC(__imp__XMASetInputBuffer0Valid) {
    Trace(ctx, "XMASetInputBuffer0Valid", true);
    SetField(ctx.r3.u32, kInput0Valid, 1);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__XMAIsInputBuffer1Valid) {
    Trace(ctx, "XMAIsInputBuffer1Valid", true);
    ctx.r3.u64 = GetField(ctx.r3.u32, kInput1Valid);
}
PPC_FUNC(__imp__XMASetInputBuffer1Valid) {
    Trace(ctx, "XMASetInputBuffer1Valid", true);
    SetField(ctx.r3.u32, kInput1Valid, 1);
    ctx.r3.u64 = 0;
}

// u32 XMASetInputBufferReadOffset(void* context, u32 bitOffset)
PPC_FUNC(__imp__XMASetInputBufferReadOffset) {
    Trace(ctx, "XMASetInputBufferReadOffset", true);
    SetField(ctx.r3.u32, kInputReadOffset, ctx.r4.u32);
    ctx.r3.u64 = 0;
}

// BOOL XMAIsOutputBufferValid(void* context) / u32 XMASetOutputBufferValid(void* context)
PPC_FUNC(__imp__XMAIsOutputBufferValid) {
    Trace(ctx, "XMAIsOutputBufferValid", true);
    ctx.r3.u64 = GetField(ctx.r3.u32, kOutputValid);
}
PPC_FUNC(__imp__XMASetOutputBufferValid) {
    Trace(ctx, "XMASetOutputBufferValid", true);
    SetField(ctx.r3.u32, kOutputValid, 1);
    ctx.r3.u64 = 0;
}

// Output ring offsets, in 256-byte blocks: the decoder writes, the title reads.
PPC_FUNC(__imp__XMAGetOutputBufferWriteOffset) {
    Trace(ctx, "XMAGetOutputBufferWriteOffset", true);
    ctx.r3.u64 = GetField(ctx.r3.u32, kOutputWriteOffset);
}
PPC_FUNC(__imp__XMAGetOutputBufferReadOffset) {
    Trace(ctx, "XMAGetOutputBufferReadOffset", true);
    ctx.r3.u64 = GetField(ctx.r3.u32, kOutputReadOffset);
}
PPC_FUNC(__imp__XMASetOutputBufferReadOffset) {
    Trace(ctx, "XMASetOutputBufferReadOffset", true);
    SetField(ctx.r3.u32, kOutputReadOffset, ctx.r4.u32);
    ctx.r3.u64 = 0;
}
