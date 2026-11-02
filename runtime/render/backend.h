#pragma once

// What the GPU backend's parts share (targets.cpp and draws.cpp), all on the command
// processor's thread: the command buffer being recorded, the render targets, the
// rendering scope the draws go into, and a ring of host-visible memory for each frame's
// vertices, indices and constants.

#include "render/vulkan.h"

#include <cstdint>

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

// Frames finished so far (swaps handled).
uint64_t FrameNumber();

// The texture a resolve last wrote at this physical address, or nullptr. serial changes
// whenever the image is made again.
Image* ResolvedImage(uint32_t address, uint64_t& serial);

// --capture-frame: true while the frame being built is the one to log in full.
bool Capturing();

}  // namespace s3n::render::backend
