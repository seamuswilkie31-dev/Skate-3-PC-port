#include "apu/xma.h"

#include "kernel/kernel.h"
#include "memory.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace s3n::apu {
namespace {

constexpr uint32_t kContexts = 256;
constexpr uint32_t kBlockBytes = 256;
constexpr uint32_t kPacketBits = 2048 * 8;
constexpr uint32_t kFirstFrameBits = 32;       // after a packet's header
constexpr uint32_t kInputBitsPerFrame = 4096;  // the silent decoder's pace: 4 frames per packet
constexpr uint32_t kFramesPerTick = 4;         // per context, per millisecond at most

GuestMemory* gMemory = nullptr;
std::mutex gMutex;
uint32_t gPool = 0;  // guest address of the context array, 0 until first use
std::vector<bool> gInUse(kContexts);
std::vector<bool> gEnabled(kContexts);
std::atomic<uint64_t> gFrames{0};
std::atomic<uint32_t> gCreated{0};

uint8_t* Base() { return gMemory->base(); }

uint32_t Get(uint32_t context, XmaField f) {
    uint32_t word = LoadBE32(Base(), context + f.word * 4);
    return f.bits == 32 ? word : (word >> f.shift) & ((1u << f.bits) - 1);
}

void Set(uint32_t context, XmaField f, uint32_t value) {
    uint32_t address = context + f.word * 4;
    if (f.bits == 32) {
        StoreBE32(Base(), address, value);
        return;
    }
    uint32_t mask = ((1u << f.bits) - 1) << f.shift;
    StoreBE32(Base(), address, (LoadBE32(Base(), address) & ~mask) | ((value << f.shift) & mask));
}

int32_t IndexOf(uint32_t context) {
    if (!gPool || context < gPool || context >= gPool + kContexts * kContextSize) return -1;
    if ((context - gPool) % kContextSize) return -1;
    return int32_t((context - gPool) / kContextSize);
}

// Decodes up to a few frames of one enabled context: each frame is silence written to the
// output ring, paid for by advancing through the current input buffer.
void Step(uint32_t context) {
    if (!Get(context, kOutputValid)) return;
    for (uint32_t frame = 0; frame < kFramesPerTick; ++frame) {
        bool second = Get(context, kCurrentBuffer);
        XmaField valid = second ? kInput1Valid : kInput0Valid;
        if (!Get(context, valid)) {
            XmaField other = second ? kInput0Valid : kInput1Valid;
            if (!Get(context, other)) return;  // starved until the title refills a buffer
            second = !second;
            valid = other;
            Set(context, kCurrentBuffer, second);
            Set(context, kInputReadOffset, kFirstFrameBits);
        }

        uint32_t blocks = Get(context, kOutputBlockCount);
        if (!blocks) blocks = 32;
        uint32_t frameBlocks = std::min<uint32_t>(Get(context, kStereo) ? 8 : 4, blocks - 1);
        uint32_t write = Get(context, kOutputWriteOffset), read = Get(context, kOutputReadOffset);
        uint32_t space = (read + blocks - write - 1) % blocks;
        if (frameBlocks == 0 || space < frameBlocks) return;  // the title hasn't read enough yet

        uint32_t output = Get(context, kOutputAddress);
        for (uint32_t b = 0; b < frameBlocks; ++b)
            std::memset(gMemory->Physical(output + ((write + b) % blocks) * kBlockBytes), 0, kBlockBytes);
        Set(context, kOutputWriteOffset, (write + frameBlocks) % blocks);

        uint32_t packets = Get(context, second ? kInput1PacketCount : kInput0PacketCount);
        uint32_t offset = Get(context, kInputReadOffset) + kInputBitsPerFrame;
        if (offset >= packets * kPacketBits) {  // buffer used up: hand it back, move to the other
            Set(context, valid, 0);
            Set(context, kCurrentBuffer, !second);
            offset = kFirstFrameBits;
        }
        Set(context, kInputReadOffset, offset);
        ++gFrames;
    }
}

void Decoder() {
    for (;;) {
        {
            std::lock_guard lock(gMutex);
            for (uint32_t i = 0; gPool && i < kContexts; ++i)
                if (gEnabled[i]) Step(gPool + i * kContextSize);
        }
        Sleep(1);
    }
}

std::string Describe() {
    uint32_t inUse = 0, enabled = 0;
    {
        std::unique_lock lock(gMutex, std::try_to_lock);
        for (uint32_t i = 0; lock.owns_lock() && i < kContexts; ++i) {
            inUse += gInUse[i];
            enabled += gEnabled[i];
        }
    }
    char line[160];
    std::snprintf(line, sizeof line, "xma: %u contexts created, %u in use, %u enabled; %llu silent frames decoded\n",
                  gCreated.load(), inUse, enabled, gFrames.load());
    return line;
}

}  // namespace

void InitXma(GuestMemory& memory) {
    gMemory = &memory;
    kernel::AddReportSection(Describe);
    std::thread(Decoder).detach();
}

uint32_t CreateContext() {
    std::lock_guard lock(gMutex);
    if (!gPool) {
        uint32_t offset = gMemory->AllocPhysical(0x10000, 0x10000, GuestMemory::kPhysicalSize, false);
        if (offset == GuestMemory::kNoPhysical) return 0;
        gPool = GuestMemory::kPhysicalWindows[0] + offset;
    }
    for (uint32_t i = 0; i < kContexts; ++i) {
        if (gInUse[i]) continue;
        gInUse[i] = true;
        gEnabled[i] = false;
        uint32_t context = gPool + i * kContextSize;
        std::memset(Base() + context, 0, kContextSize);
        ++gCreated;
        return context;
    }
    return 0;
}

void ReleaseContext(uint32_t context) {
    std::lock_guard lock(gMutex);
    if (int32_t i = IndexOf(context); i >= 0) gInUse[i] = gEnabled[i] = false;
}

uint32_t GetField(uint32_t context, XmaField field) {
    std::lock_guard lock(gMutex);
    return IndexOf(context) >= 0 ? Get(context, field) : 0;
}

void SetField(uint32_t context, XmaField field, uint32_t value) {
    std::lock_guard lock(gMutex);
    if (IndexOf(context) >= 0) Set(context, field, value);
}

// XMA_CONTEXT_INIT: input buffer 0 and 1 (address, packet count), input read offset,
// output buffer (address, block count), work buffer, subframes to decode, channel count,
// sample rate, then loop data (start, end, count, subframe end, subframe skip).
void InitializeContext(uint32_t context, uint32_t init) {
    std::lock_guard lock(gMutex);
    if (IndexOf(context) < 0) return;
    const uint8_t* base = Base();
    auto physical = [&](uint32_t address) { return address ? gMemory->ConsolePhysical(address) : 0; };
    std::memset(Base() + context, 0, kContextSize);
    Set(context, kInput0Address, physical(LoadBE32(base, init + 0x00)));
    Set(context, kInput0PacketCount, LoadBE32(base, init + 0x04));
    Set(context, kInput1Address, physical(LoadBE32(base, init + 0x08)));
    Set(context, kInput1PacketCount, LoadBE32(base, init + 0x0C));
    Set(context, kInputReadOffset, LoadBE32(base, init + 0x10));
    Set(context, kOutputAddress, physical(LoadBE32(base, init + 0x14)));
    Set(context, kOutputBlockCount, LoadBE32(base, init + 0x18));
    Set(context, kSubframeDecodeCount, LoadBE32(base, init + 0x20));
    Set(context, kStereo, LoadBE32(base, init + 0x24) >= 1);
    Set(context, kSampleRate, LoadBE32(base, init + 0x28));
    Set(context, kLoopStart, LoadBE32(base, init + 0x2C));
    Set(context, kLoopEnd, LoadBE32(base, init + 0x30));
    Set(context, kLoopCount, base[init + 0x34]);
    Set(context, kLoopSubframeEnd, base[init + 0x35]);
    Set(context, kLoopSubframeSkip, base[init + 0x36]);
}

void EnableContext(uint32_t context, bool enabled) {
    std::lock_guard lock(gMutex);
    if (int32_t i = IndexOf(context); i >= 0) gEnabled[i] = enabled;
}

}  // namespace s3n::apu
