#pragma once

// The console GPU's front end: its registers, the command processor that consumes the
// ring buffer the title's Direct3D writes, and the interrupts it raises. Nothing is drawn
// yet: draw packets are counted and skipped, while the packets that synchronize with the
// CPU (memory writes, fences, waits, interrupts) are carried out so the title keeps going.

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

}  // namespace s3n::gpu
