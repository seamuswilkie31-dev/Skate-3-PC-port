#include "apu/output.h"

#include "kernel/kernel.h"
#include "memory.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace s3n::apu {
namespace {

// Rear and centre channels fold into the front pair at -3 dB; the LFE is left out, as most
// stereo downmixes do.
constexpr float kFold = 0.70710678f;
constexpr uint32_t kWavHeaderBytes = 44;
constexpr uint32_t kWavUpdateFrames = 188;  // rewrite the header about once a second

SDL_AudioStream* gStream = nullptr;
bool gMute = false;
std::atomic<uint64_t> gPlayed{0};
std::atomic<uint64_t> gUnderruns{0};  // the device had played everything when a frame came: a gap
std::mutex gLevelsMutex;
std::array<double, kFrameChannels> gEnergy{};  // sum of squares per channel, for the report
std::array<float, kFrameChannels> gPeak{};
std::FILE* gWav = nullptr;
uint32_t gWavBytes = 0;

void PutLE32(uint8_t* at, uint32_t value) {
    for (int i = 0; i < 4; ++i) at[i] = uint8_t(value >> (8 * i));
}

void WriteWavHeader() {
    uint8_t header[kWavHeaderBytes] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
                                       16, 0, 0, 0, 1, 0, 2, 0};
    PutLE32(header + 4, 36 + gWavBytes);
    PutLE32(header + 24, kOutputRate);
    PutLE32(header + 28, kOutputRate * 4);
    header[32] = 4;  // bytes per sample frame
    header[34] = 16;  // bits per sample
    std::memcpy(header + 36, "data", 4);
    PutLE32(header + 40, gWavBytes);
    std::fseek(gWav, 0, SEEK_SET);
    std::fwrite(header, 1, sizeof header, gWav);
    std::fseek(gWav, 0, SEEK_END);
    std::fflush(gWav);
}

std::string Describe() {
    std::string text;
    char line[160];
    std::snprintf(line, sizeof line, "audio out: %llu frames played%s, %llu underruns; levels (RMS/peak) per channel:",
                  gPlayed.load(), gMute ? " (muted)" : "", gUnderruns.load());
    text += line;
    std::lock_guard lock(gLevelsMutex);
    uint64_t samples = std::max<uint64_t>(gPlayed.load() * kFrameSamples, 1);
    for (uint32_t c = 0; c < kFrameChannels; ++c) {
        std::snprintf(line, sizeof line, " %.3f/%.2f", std::sqrt(gEnergy[c] / double(samples)), gPeak[c]);
        text += line;
    }
    return text + "\n";
}

}  // namespace

bool OpenOutput(bool mute, const std::string& wavPath) {
    gMute = mute;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        kernel::Log("  audio: no sound: SDL_InitSubSystem: %s\n", SDL_GetError());
        return false;
    }
    SDL_AudioSpec spec{SDL_AUDIO_F32, 2, int(kOutputRate)};
    gStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!gStream) {
        kernel::Log("  audio: no sound: SDL_OpenAudioDeviceStream: %s\n", SDL_GetError());
        return false;
    }
    SDL_ResumeAudioStreamDevice(gStream);
    if (!wavPath.empty()) {
        gWav = std::fopen(wavPath.c_str(), "wb");
        if (gWav) WriteWavHeader();
        else kernel::Log("  audio: can't write %s\n", wavPath.c_str());
    }
    kernel::Log("  audio: playing through %s\n", SDL_GetCurrentAudioDriver());
    kernel::AddReportSection(Describe);
    return true;
}

bool OutputOpen() { return gStream != nullptr; }

void PlayFrame(const uint8_t* frame) {
    if (!gStream) return;
    std::array<float, kFrameSamples * 2> mix;
    std::array<double, kFrameChannels> energy{};
    std::array<float, kFrameChannels> peak{};
    for (uint32_t i = 0; i < kFrameSamples; ++i) {
        float in[kFrameChannels];
        for (uint32_t c = 0; c < kFrameChannels; ++c) {
            uint32_t bits = LoadBE32(frame, (c * kFrameSamples + i) * 4);
            std::memcpy(&in[c], &bits, 4);
            if (!std::isfinite(in[c])) in[c] = 0.0f;
            energy[c] += double(in[c]) * in[c];
            peak[c] = std::max(peak[c], std::fabs(in[c]));
        }
        mix[i * 2] = std::clamp(in[0] + kFold * (in[2] + in[4]), -1.0f, 1.0f);
        mix[i * 2 + 1] = std::clamp(in[1] + kFold * (in[2] + in[5]), -1.0f, 1.0f);
    }
    {
        std::lock_guard lock(gLevelsMutex);
        for (uint32_t c = 0; c < kFrameChannels; ++c) {
            gEnergy[c] += energy[c];
            gPeak[c] = std::max(gPeak[c], peak[c]);
        }
        if (gWav) {
            std::array<int16_t, kFrameSamples * 2> pcm;
            for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = int16_t(std::lrintf(mix[i] * 32767.0f));
            std::fwrite(pcm.data(), sizeof pcm, 1, gWav);
            gWavBytes += uint32_t(sizeof pcm);
            if (gPlayed % kWavUpdateFrames == 0) WriteWavHeader();
        }
    }
    if (gMute) mix.fill(0.0f);
    if (gPlayed > kWavUpdateFrames && SDL_GetAudioStreamQueued(gStream) == 0) ++gUnderruns;
    SDL_PutAudioStreamData(gStream, mix.data(), int(sizeof mix));
    ++gPlayed;
}

uint32_t QueuedFrames() {
    int queued = gStream ? SDL_GetAudioStreamQueued(gStream) : 0;
    return queued > 0 ? uint32_t(queued) / (kFrameSamples * 2 * sizeof(float)) : 0;
}

}  // namespace s3n::apu
