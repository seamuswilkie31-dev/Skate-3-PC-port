#pragma once

// The game's shaders, translated ahead of time (scripts/shaders.ps1) and loaded at
// start-up. The GPU is only ever given a shader's microcode, so that's what they're
// found by: a key computed from the microcode (ShaderKey), the same for the container's
// copy and the copy Direct3D binds and hands to the GPU.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace s3n::render {

struct Shader {
    uint64_t containerHash = 0;  // XenosRecomp's name for it
    uint64_t key = 0;            // ShaderKey of its microcode
    bool pixel = false;
    uint32_t specConstantsMask = 0;
    const uint8_t* container = nullptr;  // the Xbox 360 shader container (big-endian)
    uint32_t containerSize = 0;
    const uint8_t* microcode = nullptr;  // inside the container
    uint32_t microcodeSize = 0;          // bytes
    const uint32_t* spirv = nullptr;
    uint32_t spirvSize = 0;  // bytes
    void* module = nullptr;  // VkShaderModule, once the renderer has made it
};

// The identity of a shader's code: FNV-1a 64 over the instructions its control flow
// executes, with the vertex fetch fields Direct3D fills in when it binds a vertex
// declaration cleared. Leaves out anything after the last instruction (some shaders
// carry a hash there, which bound copies don't always keep).
uint64_t ShaderKey(const uint8_t* microcode, size_t size);

// Loads shader_cache.bin. Returns the number of shaders, or 0 with `error` set.
size_t LoadShaderCache(const std::filesystem::path& path, std::string& error);

// The translated vertex or pixel shader with this key, or nullptr.
Shader* FindShader(uint64_t key, bool pixel);

// Every loaded shader, for the renderer to make modules from.
Shader* Shaders(size_t& count);

}  // namespace s3n::render
