#pragma once

// The console's XMA audio decoder. Titles hand it compressed packets through contexts in
// physical memory and read 16-bit PCM back from an output ring. For now the decoder is
// silent: it consumes input at a steady rate and fills the output with zeros, which is
// enough for a title's audio engine to keep streaming. Real decoding comes in phase 4.

#include <cstdint>

namespace s3n {
class GuestMemory;
}

namespace s3n::apu {

// A context's 64 bytes are 16 big-endian words of bit fields, the hardware's layout.
struct XmaField {
    uint8_t word;
    uint8_t shift;
    uint8_t bits;
};
inline constexpr XmaField kInput0PacketCount{0, 0, 12};
inline constexpr XmaField kLoopCount{0, 12, 8};
inline constexpr XmaField kInput0Valid{0, 20, 1};
inline constexpr XmaField kInput1Valid{0, 21, 1};
inline constexpr XmaField kOutputBlockCount{0, 22, 5};  // 256-byte blocks; 0 means 32
inline constexpr XmaField kOutputWriteOffset{0, 27, 5};
inline constexpr XmaField kInput1PacketCount{1, 0, 12};
inline constexpr XmaField kLoopSubframeEnd{1, 14, 3};
inline constexpr XmaField kLoopSubframeSkip{1, 17, 3};
inline constexpr XmaField kSubframeDecodeCount{1, 20, 4};
inline constexpr XmaField kSampleRate{1, 27, 2};
inline constexpr XmaField kStereo{1, 29, 1};
inline constexpr XmaField kOutputValid{1, 31, 1};
inline constexpr XmaField kInputReadOffset{2, 0, 26};  // in bits
inline constexpr XmaField kLoopStart{3, 0, 26};
inline constexpr XmaField kLoopEnd{4, 0, 26};
inline constexpr XmaField kCurrentBuffer{4, 31, 1};
inline constexpr XmaField kInput0Address{5, 0, 32};  // physical
inline constexpr XmaField kInput1Address{6, 0, 32};
inline constexpr XmaField kOutputAddress{7, 0, 32};
inline constexpr XmaField kOutputReadOffset{9, 0, 5};
inline constexpr uint32_t kContextSize = 64;

// Starts the decoder thread. The context pool is made on first use.
void InitXma(GuestMemory& memory);

// XMACreateContext / XMAReleaseContext: a context's guest address, or 0 when all are in use.
uint32_t CreateContext();
void ReleaseContext(uint32_t context);

// Field access, under the decoder's lock so it never sees half an update.
uint32_t GetField(uint32_t context, XmaField field);
void SetField(uint32_t context, XmaField field, uint32_t value);
// Clears a context and fills it from an XMA_CONTEXT_INIT.
void InitializeContext(uint32_t context, uint32_t init);

// XMAEnableContext / XMADisableContext: whether the decoder works on a context.
void EnableContext(uint32_t context, bool enabled);

}  // namespace s3n::apu
