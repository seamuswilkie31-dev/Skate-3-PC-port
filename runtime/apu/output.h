#pragma once

// The PC's speakers. The title's audio driver client submits a frame every 256 samples at
// 48 kHz (XAudioSubmitRenderDriverFrame): 6 channels (5.1: front left and right, centre,
// LFE, rear left and right) of big-endian floats, one channel's 256 samples after another.
// They're mixed down to stereo and played through SDL3.

#include <cstdint>
#include <string>

namespace s3n::apu {

inline constexpr uint32_t kOutputRate = 48000;
inline constexpr uint32_t kFrameSamples = 256;
inline constexpr uint32_t kFrameChannels = 6;

// Opens the default playback device; false when there's none (the reason is logged).
// `mute` plays silence instead, keeping the timing. `wavPath`, when given, also records the
// stereo mix to a 16-bit WAV file.
bool OpenOutput(bool mute, const std::string& wavPath);
bool OutputOpen();

// Queues one frame (kFrameSamples * kFrameChannels floats in guest memory).
void PlayFrame(const uint8_t* frame);

// Frames queued and not played yet: the driver clock keeps this a few frames ahead.
uint32_t QueuedFrames();

}  // namespace s3n::apu
