#pragma once

// The console GPU's front end: its registers, the command processor that consumes the
// ring buffer the title's Direct3D writes, and the interrupts it raises. The packets that
// synchronize with the CPU (memory writes, fences, waits, interrupts) are carried out so
// the title keeps going. Resolves, Direct3D's clears and swaps go to the renderer
// (render/targets.h); other draws are only counted so far.

#include <cstdint>
#include <string>

namespace s3n {
class GuestMemory;
}

namespace s3n::gpu {

// The registers the CPU sees, memory-mapped (register n at kRegisterWindow + n * 4).
constexpr uint32_t kRegisterWindow = 0x7FC80000;
constexpr uint32_t kRegisterWindowSize = 0x10000;

// Maps the register window and starts the command processor.
bool Init(GuestMemory& memory);

// VdInitializeRingBuffer / VdEnableRingBufferRPtrWriteBack (physical addresses).
void InitializeRingBuffer(uint32_t physical, uint32_t sizeLog2);
void EnableReadPointerWriteBack(uint32_t physical, uint32_t blockSizeLog2);

// VdSetGraphicsInterruptCallback: callback(source, userData), source 0 for vertical blank
// (60 Hz) and 1 for an interrupt packet. Starts the interrupt thread on first use.
void SetInterruptCallback(uint32_t callback, uint32_t userData);

// Counts a VdSwap (a frame presented).
void CountSwap();

// VdSwap ends its packets with a type-3 NOP that starts with this marker, followed by
// the front buffer's physical address, width and height. The command processor hands it
// to the renderer when it gets there.
constexpr uint32_t kSwapMarker = 0x53574150;  // "SWAP"

// For the renderer, on the command processor's thread: a register's current value, and
// physical memory.
uint32_t Register(uint32_t index);
uint8_t* PhysicalMemory(uint32_t address);

}  // namespace s3n::gpu
