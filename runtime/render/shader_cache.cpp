#include "render/shader_cache.h"

#include "memory.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <vector>

namespace s3n::render {
namespace {

constexpr uint32_t kMagic = 0x534E3353;  // "S3NS"
constexpr uint32_t kVersion = 1;

// One entry as XenosRecomp's runtime cache writes it (recomp/xenosrecomp-patches/0002).
struct Entry {
    uint64_t containerHash;
    uint32_t flags, specConstantsMask, containerOffset, containerSize, spirvOffset, spirvSize;
};
static_assert(sizeof(Entry) == 32);

// Control flow opcodes that run a clause of instructions (exec, conditional and
// predicated exec, with or without ending the shader).
bool IsExec(uint32_t opcode) {
    return (opcode >= 1 && opcode <= 6) || opcode == 13 || opcode == 14;
}

// Vertex fetch fields Direct3D sets when it binds a vertex declaration: the fetch
// constant and prefetch count (dword 0), the format and its modes, above the destination
// swizzle (dword 1), and the stride and offset (dword 2).
constexpr uint32_t kBoundFetchFields[3] = {0x3FF00000, 0x7FFFF000, 0x7FFFFFFF};

std::vector<uint8_t> gData;
std::vector<Shader> gShaders;
std::unordered_map<uint64_t, Shader*> gByKey[2];  // vertex, pixel

}  // namespace

uint64_t ShaderKey(const uint8_t* microcode, size_t size) {
    auto word = [microcode](size_t index) { return LoadBE32(microcode, uint32_t(index * 4)); };

    // Instructions take 3 dwords. Control flow instructions are 48 bits, two to each
    // 3-dword slot, from the start of the shader up to the first instruction a clause runs.
    size_t slots = size / 12, first = slots, end = 0;
    std::vector<size_t> fetches;
    for (size_t s = 0; s < first; ++s) {
        uint32_t w0 = word(3 * s), w1 = word(3 * s + 1), w2 = word(3 * s + 2);
        for (uint64_t cf : {w0 | uint64_t(w1 & 0xFFFF) << 32, w1 >> 16 | uint64_t(w2) << 16}) {
            if (!IsExec(uint32_t(cf >> 44) & 0xF)) continue;
            size_t address = cf & 0xFFF, count = cf >> 12 & 7;
            uint32_t sequence = cf >> 16 & 0xFFF;  // two bits per instruction; the low one: fetch
            if (!count) continue;
            first = std::min(first, address);
            end = std::max(end, address + count);
            for (size_t i = 0; i < count; ++i)
                if (sequence >> (2 * i) & 1) fetches.push_back(address + i);
        }
    }
    end = end ? std::min(end, slots) : slots;
    std::sort(fetches.begin(), fetches.end());

    uint64_t hash = 0xCBF29CE484222325ull;
    auto next = fetches.begin();
    for (size_t s = 0; s < end; ++s) {
        uint32_t w[3] = {word(3 * s), word(3 * s + 1), word(3 * s + 2)};
        while (next != fetches.end() && *next < s) ++next;
        bool vertexFetch = next != fetches.end() && *next == s && (w[0] & 0x1F) == 0;
        for (int i = 0; i < 3; ++i) {
            uint32_t value = vertexFetch ? w[i] & ~kBoundFetchFields[i] : w[i];
            for (int shift = 24; shift >= 0; shift -= 8) hash = (hash ^ (value >> shift & 0xFF)) * 0x100000001B3ull;
        }
    }
    return hash;
}

size_t LoadShaderCache(const std::filesystem::path& path, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "no shader cache at " + path.string() + " (run scripts\\shaders.ps1)";
        return 0;
    }
    gData.assign(std::istreambuf_iterator<char>(file), {});
    uint32_t header[4];
    if (gData.size() < sizeof(header)) {
        error = "the shader cache is truncated";
        return 0;
    }
    std::memcpy(header, gData.data(), sizeof(header));
    if (header[0] != kMagic || header[1] != kVersion) {
        error = "the shader cache is from another version (rerun scripts\\shaders.ps1)";
        return 0;
    }
    uint32_t count = header[2];
    if (sizeof(header) + uint64_t(count) * sizeof(Entry) > gData.size()) {
        error = "the shader cache is truncated";
        return 0;
    }
    gShaders.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        Entry e;
        std::memcpy(&e, gData.data() + sizeof(header) + i * sizeof(Entry), sizeof(Entry));
        const uint8_t* container = gData.data() + e.containerOffset;
        bool valid = uint64_t(e.containerOffset) + e.containerSize <= gData.size() &&
                     uint64_t(e.spirvOffset) + e.spirvSize <= gData.size() && e.spirvOffset % 4 == 0 &&
                     e.containerSize >= 0x20;
        // The microcode: the shader record (at the container's shader offset) gives its
        // place in the physical part, which follows the virtual part.
        uint32_t virtualSize = 0, shaderOffset = 0, physicalOffset = 0, size = 0;
        if (valid) {
            virtualSize = LoadBE32(container, 0x04);
            shaderOffset = LoadBE32(container, 0x18);
            valid = uint64_t(shaderOffset) + 8 <= e.containerSize;
        }
        if (valid) {
            physicalOffset = LoadBE32(container, shaderOffset);
            size = LoadBE32(container, shaderOffset + 4);
            valid = uint64_t(virtualSize) + physicalOffset + size <= e.containerSize;
        }
        if (!valid) {
            error = "the shader cache is corrupt";
            gShaders.clear();
            gByKey[0].clear();
            gByKey[1].clear();
            return 0;
        }
        Shader& s = gShaders[i];
        s.containerHash = e.containerHash;
        s.pixel = e.flags & 1;
        s.specConstantsMask = e.specConstantsMask;
        s.container = container;
        s.containerSize = e.containerSize;
        s.microcode = container + virtualSize + physicalOffset;
        s.microcodeSize = size;
        s.key = ShaderKey(s.microcode, size);
        s.spirv = reinterpret_cast<const uint32_t*>(gData.data() + e.spirvOffset);
        s.spirvSize = e.spirvSize;
        // Containers with the same code translate the same; the first one stands for them.
        gByKey[s.pixel].emplace(s.key, &s);
    }
    return count;
}

Shader* FindShader(uint64_t key, bool pixel) {
    auto it = gByKey[pixel].find(key);
    return it == gByKey[pixel].end() ? nullptr : it->second;
}

Shader* Shaders(size_t& count) {
    count = gShaders.size();
    return gShaders.data();
}

}  // namespace s3n::render
