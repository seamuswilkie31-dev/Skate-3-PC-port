#pragma once

// What the GPU backend's parts share (targets.cpp and draws.cpp), all on the command
// processor's thread: the command buffer being recorded, the render targets, the
// rendering scope the draws go into, and a ring of host-visible memory for each frame's
// vertices, indices and constants.

#include "render/vulkan.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace s3n::render::backend {

// The command buffer being recorded, started if need be. VK_NULL_HANDLE without a window.
// A long recording is submitted here, between operations.
VkCommandBuffer Commands();

// Counts an operation in the recording (clear, resolve, draw).
void Operation();

// The render target at an EDRAM base with this pitch, MSAA mode (0-2) and format, made
// when first used. nullptr if it can't be.
Image* FindTarget(uint32_t base, uint32_t pitch, uint32_t msaa, uint32_t format, bool depth, VkCommandBuffer cb);

// Draws go into one rendering scope for as long as their targets stay the same. Anything
// else that touches images ends it first.
void BeginRendering(VkCommandBuffer cb, Image* color, Image* depth);
void EndRendering();

// Space in this recording's upload ring, mapped and with a device address. Allocate first,
// then record with the command buffer it returns: if the ring is full, the recording is
// submitted and a new one started.
struct Upload {
    uint8_t* data = nullptr;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceAddress address = 0;
};
VkCommandBuffer Allocate(VkDeviceSize size, Upload& upload);

// The recording being made: a number that changes when one is submitted and the next begins.
// Upload space stays valid for as long as its recording is the one being made.
uint64_t RecordingNumber();

// Frames finished so far (swaps handled).
uint64_t FrameNumber();

// A hash of guest memory, fast enough to check what textures and vertices were made from
// once a frame: four independent multiply-xorshift lanes over 8-byte words.
inline uint64_t HashMemory(const uint8_t* data, size_t size) {
    uint64_t lanes[4] = {0x9E3779B97F4A7C15ull, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull, size};
    size_t i = 0;
    for (; i + 32 <= size; i += 32) {
        for (int l = 0; l < 4; ++l) {
            uint64_t v;
            std::memcpy(&v, data + i + l * 8, 8);
            lanes[l] = (lanes[l] ^ v) * 0xFF51AFD7ED558CCDull;
            lanes[l] ^= lanes[l] >> 29;
        }
    }
    for (; i < size; ++i) lanes[0] = (lanes[0] ^ data[i]) * 0x100000001B3ull;
    return lanes[0] ^ lanes[1] * 31 ^ lanes[2] * 961 ^ lanes[3] * 29791;
}

// The texture a resolve last wrote at this physical address, or nullptr. serial changes
// whenever the image is made again.
Image* ResolvedImage(uint32_t address, uint64_t& serial);

// Changes whenever a resolve texture's image is made (again).
uint64_t ResolveSerial();

// --capture-frame: true while the frame being built is the one to log in full.
bool Capturing();

}  // namespace s3n::render::backend
