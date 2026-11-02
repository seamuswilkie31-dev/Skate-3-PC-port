// Video (Vd*): the display mode, and the hand-off to the GPU front end in gpu/gpu.cpp.
//
// The display is reported as 1280x720 at 60 Hz, widescreen and high definition, the
// mode Skate 3 renders in on the console.

#include "gpu/gpu.h"
#include "kernel/kernel.h"
#include "memory.h"
#include "render/presenter.h"

#include <cstring>

using namespace s3n;
using namespace s3n::kernel;

namespace {

constexpr uint32_t kWidth = 1280;
constexpr uint32_t kHeight = 720;
constexpr float kRefreshRate = 60.0f;

uint32_t FloatBits(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    return bits;
}

void StoreBE16(uint8_t* base, uint32_t address, uint16_t value) {
    base[address] = uint8_t(value >> 8);
    base[address + 1] = uint8_t(value);
}

// XVIDEO_MODE (48 bytes): width, height, interlaced, widescreen, high definition,
// refresh rate (float), video standard (1: NTSC), two fields the console sets to 0x4A
// and 1, then reserved words.
void WriteVideoMode(uint8_t* base, uint32_t mode) {
    std::memset(base + mode, 0, 48);
    StoreBE32(base, mode + 0x00, kWidth);
    StoreBE32(base, mode + 0x04, kHeight);
    StoreBE32(base, mode + 0x08, 0);
    StoreBE32(base, mode + 0x0C, 1);
    StoreBE32(base, mode + 0x10, 1);
    StoreBE32(base, mode + 0x14, FloatBits(kRefreshRate));
    StoreBE32(base, mode + 0x18, 1);
    StoreBE32(base, mode + 0x1C, 0x4A);
    StoreBE32(base, mode + 0x20, 1);
}

}  // namespace

// void VdQueryVideoMode(XVIDEO_MODE* mode)
PPC_FUNC(__imp__VdQueryVideoMode) {
    Trace(ctx, "VdQueryVideoMode", true);
    WriteVideoMode(base, ctx.r3.u32);
}

// void XGetVideoMode(XVIDEO_MODE* mode)
PPC_FUNC(__imp__XGetVideoMode) {
    Trace(ctx, "XGetVideoMode", true);
    WriteVideoMode(base, ctx.r3.u32);
}

// u32 VdQueryVideoFlags(): 1 widescreen, 2 at least 1024 wide, 4 at least 1920 wide.
PPC_FUNC(__imp__VdQueryVideoFlags) {
    Trace(ctx, "VdQueryVideoFlags", true);
    ctx.r3.u64 = 1 | 2;
}

// void VdGetCurrentDisplayInformation(DISPLAY_INFO* info)
// 0x58 bytes: front buffer size and formats, scaler parameters (source rectangle, output
// size, filters), overscan, display size, refresh rate, interlacing, actual width.
PPC_FUNC(__imp__VdGetCurrentDisplayInformation) {
    Trace(ctx, "VdGetCurrentDisplayInformation", true);
    uint32_t info = ctx.r3.u32;
    std::memset(base + info, 0, 0x58);
    StoreBE16(base, info + 0x00, kWidth);
    StoreBE16(base, info + 0x02, kHeight);
    StoreBE32(base, info + 0x10, kWidth);   // scaler source rectangle x2, y2
    StoreBE32(base, info + 0x14, kHeight);
    StoreBE32(base, info + 0x18, kWidth);   // scaled output size
    StoreBE32(base, info + 0x1C, kHeight);
    StoreBE32(base, info + 0x20, 1);        // vertical filter type
    StoreBE32(base, info + 0x30, 1);        // horizontal filter type
    StoreBE16(base, info + 0x48, kWidth);   // display size
    StoreBE16(base, info + 0x4A, kHeight);
    StoreBE32(base, info + 0x4C, FloatBits(kRefreshRate));
    StoreBE16(base, info + 0x56, kWidth);
}

// void VdGetCurrentDisplayGamma(u32* type, float* power): type 2 is a TV (BT.709) curve.
PPC_FUNC(__imp__VdGetCurrentDisplayGamma) {
    Trace(ctx, "VdGetCurrentDisplayGamma", true);
    StoreBE32(base, ctx.r3.u32, 2);
    StoreBE32(base, ctx.r4.u32, FloatBits(2.2222223f));
}

// BOOL VdInitializeEngines(u32 unknown, void* callback, void* argument, u32* pfp, u32* me)
// The microcode for the GPU's prefetch parser and micro engine isn't needed here.
PPC_FUNC(__imp__VdInitializeEngines) {
    Trace(ctx, "VdInitializeEngines", true);
    ctx.r3.u64 = 1;
}

// void VdShutdownEngines()
PPC_FUNC(__imp__VdShutdownEngines) { Trace(ctx, "VdShutdownEngines", true); }

// void VdSetGraphicsInterruptCallback(void* callback, void* userData)
PPC_FUNC(__imp__VdSetGraphicsInterruptCallback) {
    Trace(ctx, "VdSetGraphicsInterruptCallback", true);
    gpu::SetInterruptCallback(ctx.r3.u32, ctx.r4.u32);
}

// void VdInitializeRingBuffer(u32 physicalAddress, u32 sizeLog2)
PPC_FUNC(__imp__VdInitializeRingBuffer) {
    Trace(ctx, "VdInitializeRingBuffer", true);
    gpu::InitializeRingBuffer(ctx.r3.u32, ctx.r4.u32);
}

// void VdEnableRingBufferRPtrWriteBack(u32 physicalAddress, u32 blockSizeLog2)
PPC_FUNC(__imp__VdEnableRingBufferRPtrWriteBack) {
    Trace(ctx, "VdEnableRingBufferRPtrWriteBack", true);
    gpu::EnableReadPointerWriteBack(ctx.r3.u32, ctx.r4.u32);
}

// void VdSetSystemCommandBufferGpuIdentifierAddress(void* address): nothing to track.
PPC_FUNC(__imp__VdSetSystemCommandBufferGpuIdentifierAddress) {
    Trace(ctx, "VdSetSystemCommandBufferGpuIdentifierAddress", true);
}

// void VdGetSystemCommandBuffer(void* buffer, u32* identifier)
// The title only hands these back to VdSwap, which doesn't use them.
PPC_FUNC(__imp__VdGetSystemCommandBuffer) {
    Trace(ctx, "VdGetSystemCommandBuffer", true);
    if (ctx.r3.u32) {
        std::memset(base + ctx.r3.u32, 0, 0x94);
        StoreBE32(base, ctx.r3.u32, 0x53334E00);
    }
    if (ctx.r4.u32) StoreBE32(base, ctx.r4.u32, 0x53334E01);
}

// void VdSwap(u32* ringSpace, void* fetchConstant, ..., u32* frontBuffer, u32* format,
//             u32* colorSpace, u32* width, u32* height)
// The title reserves 64 dwords in the ring buffer for the swap's GPU commands. Until
// there's a renderer they're one no-op packet the command processor skips.
PPC_FUNC(__imp__VdSwap) {
    Trace(ctx, "VdSwap", true);
    uint32_t ringSpace = ctx.r3.u32;
    std::memset(base + ringSpace, 0, 64 * 4);
    StoreBE32(base, ringSpace, 0xC0000000 | (62u << 16) | (0x10u << 8));  // type 3 NOP, 63 dwords of payload
    gpu::CountSwap();
    render::NotifySwap();
}

// u32 VdInitializeScalerCommandBuffer(u32 sourceXY, u32 sourceWH, u32 outputXY, u32 outputWH,
//         u32 frontBufferWH, u32 verticalFilter, void* verticalParameters,
//         u32 horizontalFilter, void* horizontalParameters, void* unknown,
//         u32* destination, u32 count)
// Writes the display scaler's GPU commands and returns how many dwords it wrote. There's
// no scaler to program yet, so they're all type-2 filler packets. The last four arguments
// are on the stack.
PPC_FUNC(__imp__VdInitializeScalerCommandBuffer) {
    Trace(ctx, "VdInitializeScalerCommandBuffer", true);
    uint32_t destination = LoadBE32(base, ctx.r1.u32 + 0x64);
    uint32_t count = LoadBE32(base, ctx.r1.u32 + 0x6C);
    for (uint32_t i = 0; destination && i < count; ++i) StoreBE32(base, destination + i * 4, 0x80000000);
    ctx.r3.u64 = destination ? count : 0;
}

// BOOL VdPersistDisplay(u32 unknown, void** buffer): *buffer gets a small allocation the
// title later frees with MmFreePhysicalMemory.
PPC_FUNC(__imp__VdPersistDisplay) {
    Trace(ctx, "VdPersistDisplay", true);
    if (ctx.r4.u32) {
        uint32_t physical = Memory().AllocPhysical(0x1000, 0x1000, GuestMemory::kPhysicalSize, true);
        StoreBE32(base, ctx.r4.u32, physical == GuestMemory::kNoPhysical ? 0 : GuestMemory::kPhysicalWindows[2] + physical);
    }
    ctx.r3.u64 = 1;
}

// u32 VdCallGraphicsNotificationRoutines(u32 unknown, void* scaling): no routines registered.
PPC_FUNC(__imp__VdCallGraphicsNotificationRoutines) {
    Trace(ctx, "VdCallGraphicsNotificationRoutines", true);
    ctx.r3.u64 = 0;
}

// Display hardware set-up the runtime has no equivalent for: all succeed.
PPC_FUNC(__imp__VdSetDisplayMode) {
    Trace(ctx, "VdSetDisplayMode", true);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__VdEnableDisableClockGating) {
    Trace(ctx, "VdEnableDisableClockGating", true);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__VdIsHSIOTrainingSucceeded) {
    Trace(ctx, "VdIsHSIOTrainingSucceeded", true);
    ctx.r3.u64 = 1;
}
PPC_FUNC(__imp__VdRetrainEDRAM) {
    Trace(ctx, "VdRetrainEDRAM", true);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__VdRetrainEDRAMWorker) {
    Trace(ctx, "VdRetrainEDRAMWorker", true);
    ctx.r3.u64 = 0;
}
