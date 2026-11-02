#pragma once

// The game's draws in Vulkan, on the command processor's thread: a pipeline from the
// register state and the two translated shaders, vertices and indices converted from
// guest memory, and the shader constants copied from the GPU's registers, drawn into the
// render targets (targets.cpp). Textures aren't converted yet: every texture fetch reads
// white.

#include <cstdint>

namespace s3n::render {

struct Shader;

struct DrawCall {
    uint32_t initiator = 0;  // VGT_DRAW_INITIATOR
    uint32_t indexBase = 0;  // VGT_DMA_BASE and VGT_DMA_SIZE, for draws from an index buffer
    uint32_t indexSize = 0;
    const Shader* vertexShader = nullptr;
    const Shader* pixelShader = nullptr;
    const uint8_t* vertexMicrocode = nullptr;  // the bound copy the GPU was given
    uint32_t vertexMicrocodeSize = 0;          // bytes
};

void Draw(const DrawCall& call);

}  // namespace s3n::render
