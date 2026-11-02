#pragma once

// Textures for the draws, in the bindless arrays the translated shaders index: sets 0-2
// hold 2D, 3D and cube textures and set 3 samplers; slot 0 of each is a white stand-in.
// A texture comes from its fetch constant: a render target a resolve copied out at its
// address, or guest memory (untiled, endian-swapped, as a Vulkan format), converted again
// when its memory changes. All on the command processor's thread.

#include <vulkan/vulkan.h>

#include <cstdint>

namespace s3n::render::textures {

// Makes the sets and the stand-ins (recorded into `cb`).
bool SetUp(VkCommandBuffer cb);
const VkDescriptorSetLayout* SetLayouts();  // 4
const VkDescriptorSet* Sets();              // 4

// The slots for fetch constant `index` (0-31): which array its texture is in (0: 2D,
// 1: 3D, 2: cube), its slot there, and its sampler's slot. May record uploads, so it's
// called before a draw allocates its upload space (backend.h).
struct Slots {
    uint32_t kind = 0, texture = 0, sampler = 0;
};
Slots Bind(uint32_t index);

}  // namespace s3n::render::textures
