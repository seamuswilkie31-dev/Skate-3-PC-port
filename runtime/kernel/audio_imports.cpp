// Audio render driver (XAudio*): the clock the title's audio engine runs on.
//
// On the console the audio driver asks each registered client for a frame of 256
// samples at 48 kHz, about every 5.3 ms, by calling its callback. The title's audio
// engine does its mixing and timekeeping in that callback, and anything synchronized to
// audio (movie playback, for one) waits on it. A runtime thread calls the callbacks at
// that rate; the frames the title submits are counted and dropped until there's audio
// output (phase 4).

#include "kernel/kernel.h"
#include "memory.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

using namespace s3n;
using namespace s3n::kernel;

namespace {

constexpr uint32_t kDriverMagic = 0x41550000;  // driver handles: magic | client index
constexpr size_t kMaxClients = 8;
constexpr auto kFramePeriod = std::chrono::nanoseconds(1000000000ll * 256 / 48000);

struct Client {
    uint32_t callback = 0;
    uint32_t argument = 0;  // guest word holding the client's argument; the callback gets its address
};

std::mutex gClientsMutex;
std::array<Client, kMaxClients> gClients{};
std::atomic<bool> gClockStarted{false};
std::atomic<uint64_t> gFramesRequested{0};
std::atomic<uint64_t> gFramesSubmitted{0};

void AudioClock(PPCContext& ctx, uint8_t* base) {
    using Clock = std::chrono::steady_clock;
    Clock::time_point next = Clock::now();
    for (;;) {
        next += kFramePeriod;
        Clock::time_point now = Clock::now();
        if (next < now - std::chrono::milliseconds(50)) next = now;  // fell far behind: don't burst
        std::this_thread::sleep_until(next);
        std::array<Client, kMaxClients> clients;
        {
            std::lock_guard lock(gClientsMutex);
            clients = gClients;
        }
        for (const Client& client : clients) {
            if (!client.callback) continue;
            CallGuest(ctx, base, client.callback, client.argument, 0, 0);
            ++gFramesRequested;
        }
    }
}

std::string Describe() {
    char line[128];
    std::snprintf(line, sizeof line, "audio: %llu frames requested, %llu submitted (not played yet)\n",
                  gFramesRequested.load(), gFramesSubmitted.load());
    return line;
}

}  // namespace

// u32 XAudioRegisterRenderDriverClient(u32* callback, u32* driver)
// callback[0] is the function, callback[1] its argument. *driver gets a handle.
PPC_FUNC(__imp__XAudioRegisterRenderDriverClient) {
    Trace(ctx, "XAudioRegisterRenderDriverClient", true);
    uint32_t callback = LoadBE32(base, ctx.r3.u32), argument = LoadBE32(base, ctx.r3.u32 + 4);
    uint32_t wrapped = Memory().AllocSystem(4);
    size_t index = kMaxClients;
    if (wrapped) {
        StoreBE32(base, wrapped, argument);
        std::lock_guard lock(gClientsMutex);
        for (size_t i = 0; i < kMaxClients && index == kMaxClients; ++i)
            if (!gClients[i].callback) index = i;
        if (index < kMaxClients) gClients[index] = {callback, wrapped};
    }
    if (index == kMaxClients) {
        ctx.r3.u64 = 0x8007000E;  // E_OUTOFMEMORY
        return;
    }
    StoreBE32(base, ctx.r4.u32, kDriverMagic | uint32_t(index));
    Log("  audio: client %zu registered, callback %08X (%08X)\n", index, callback, argument);
    if (!gClockStarted.exchange(true)) {
        AddReportSection(Describe);
        ThreadStart start;
        start.hostBody = AudioClock;
        start.name = "Audio clock";
        start.stackSize = 0x10000;
        if (uint32_t thread = CreateGuestThread(start, nullptr)) DereferenceObject(thread);
    }
    ctx.r3.u64 = 0;
}

// u32 XAudioUnregisterRenderDriverClient(u32 driver)
PPC_FUNC(__imp__XAudioUnregisterRenderDriverClient) {
    Trace(ctx, "XAudioUnregisterRenderDriverClient", true);
    uint32_t index = ctx.r3.u32 & 0xFFFF;
    if ((ctx.r3.u32 & 0xFFFF0000) == kDriverMagic && index < kMaxClients) {
        std::lock_guard lock(gClientsMutex);
        gClients[index] = {};
    }
    ctx.r3.u64 = 0;
}

// u32 XAudioSubmitRenderDriverFrame(u32 driver, float* samples): one frame, 256 samples
// for each of 6 channels.
PPC_FUNC(__imp__XAudioSubmitRenderDriverFrame) {
    Trace(ctx, "XAudioSubmitRenderDriverFrame", true);
    ++gFramesSubmitted;
    ctx.r3.u64 = 0;
}

// u32 XAudioGetSpeakerConfig(u32* config)
PPC_FUNC(__imp__XAudioGetSpeakerConfig) {
    Trace(ctx, "XAudioGetSpeakerConfig", true);
    StoreBE32(base, ctx.r3.u32, 0x00010001);
    ctx.r3.u64 = 0;
}

// u32 XAudioGetVoiceCategoryVolume(u32 category, float* volume): full volume.
PPC_FUNC(__imp__XAudioGetVoiceCategoryVolume) {
    Trace(ctx, "XAudioGetVoiceCategoryVolume", true);
    float volume = 1.0f;
    uint32_t bits;
    std::memcpy(&bits, &volume, 4);
    StoreBE32(base, ctx.r4.u32, bits);
    ctx.r3.u64 = 0;
}
