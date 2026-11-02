#include "render/shader_cache.h"

#include "memory.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
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
// constant and prefetch count (dword 0), the format, its modes and the destination swizzle
// (dword 1: components the format lacks become 0 or 1), and the stride and offset (dword 2).
constexpr uint32_t kBoundFetchFields[3] = {0x3FF00000, 0x7FFFFFFF, 0x7FFFFFFF};

std::vector<uint8_t> gData;
std::vector<Shader> gShaders;
std::filesystem::path gHarvestFolder;  // <cache folder>\source\harvested
std::unordered_map<uint64_t, Shader*> gByKey[2];  // vertex, pixel

}  // namespace

namespace {

// Walks the control flow. Instructions take 3 dwords; control flow instructions are 48
// bits, two to each 3-dword slot, from the start of the shader up to the first
// instruction a clause runs. Returns the slot after the last instruction any clause runs;
// `fetches` gets the slots holding fetch instructions, in order.
size_t WalkClauses(const uint8_t* microcode, size_t size, std::vector<size_t>& fetches) {
    auto word = [microcode](size_t index) { return LoadBE32(microcode, uint32_t(index * 4)); };
    size_t slots = size / 12, first = slots, end = 0;
    fetches.clear();
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
    std::sort(fetches.begin(), fetches.end());
    return end ? std::min(end, slots) : slots;
}

// The fetch constants the shader's texture fetches read.
uint32_t TextureFetchMask(const uint8_t* microcode, size_t size) {
    std::vector<size_t> fetches;
    size_t end = WalkClauses(microcode, size, fetches);
    uint32_t mask = 0;
    for (size_t slot : fetches) {
        uint32_t w0 = slot < end ? LoadBE32(microcode, uint32_t(slot * 12)) : 0;
        if ((w0 & 0x1F) == 1) mask |= 1u << (w0 >> 20 & 0x1F);  // texture fetch
    }
    return mask;
}

}  // namespace

uint64_t ShaderKey(const uint8_t* microcode, size_t size) {
    auto word = [microcode](size_t index) { return LoadBE32(microcode, uint32_t(index * 4)); };
    std::vector<size_t> fetches;
    size_t end = WalkClauses(microcode, size, fetches);

    // Direct3D also writes the vertex fetches in the order of the vertex declaration, which
    // needn't be the shader's, so they're hashed in an order of their own: sorted, each
    // without its bound fields, one to each vertex fetch slot.
    std::vector<size_t> vertexSlots;
    std::vector<std::array<uint32_t, 3>> vertexFetches;
    for (size_t slot : fetches) {
        if (slot >= end || (word(3 * slot) & 0x1F) != 0) continue;
        vertexSlots.push_back(slot);
        std::array<uint32_t, 3>& f = vertexFetches.emplace_back();
        for (int i = 0; i < 3; ++i) f[i] = word(3 * slot + i) & ~kBoundFetchFields[i];
    }
    std::sort(vertexFetches.begin(), vertexFetches.end());

    uint64_t hash = 0xCBF29CE484222325ull;
    auto next = vertexSlots.begin();
    for (size_t s = 0; s < end; ++s) {
        std::array<uint32_t, 3> w = {word(3 * s), word(3 * s + 1), word(3 * s + 2)};
        if (next != vertexSlots.end() && *next == s) w = vertexFetches[next++ - vertexSlots.begin()];
        for (uint32_t value : w)
            for (int shift = 24; shift >= 0; shift -= 8) hash = (hash ^ (value >> shift & 0xFF)) * 0x100000001B3ull;
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
    gHarvestFolder = path.parent_path() / "source" / "harvested";
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
        s.textureFetchMask = TextureFetchMask(s.microcode, size);
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

size_t HarvestShaders(const uint8_t* memory, size_t size) {
    constexpr uint32_t kLargest = 256 << 10;  // no game shader container comes close
    size_t saved = 0;
    std::error_code ignored;
    for (size_t at = 0; at + 0x24 <= size; at += 4) {
        const uint8_t* c = memory + at;
        if (c[0] != 0x10 || c[1] != 0x2A || c[2] != 0x11) continue;  // container flags: 102A11xx
        uint32_t virtualSize = LoadBE32(c, 0x04), physicalSize = LoadBE32(c, 0x08), shaderOffset = LoadBE32(c, 0x18);
        uint64_t total = uint64_t(virtualSize) + physicalSize;
        if (LoadBE32(c, 0x1C) || LoadBE32(c, 0x20) || total > kLargest || at + total > size ||
            uint64_t(shaderOffset) + 8 > virtualSize)
            continue;
        uint32_t physicalOffset = LoadBE32(c, shaderOffset), codeSize = LoadBE32(c, shaderOffset + 4);
        if (!codeSize || codeSize % 12 || uint64_t(physicalOffset) + codeSize > physicalSize) continue;
        bool pixel = !(c[3] & 1);  // flags bit 0: a vertex shader
        uint64_t key = ShaderKey(c + virtualSize + physicalOffset, codeSize);
        if (FindShader(key, pixel)) continue;
        char name[40];
        std::snprintf(name, sizeof name, "%s_%016llX.bin", pixel ? "ps" : "vs", static_cast<unsigned long long>(key));
        std::filesystem::path path = gHarvestFolder / name;
        if (std::filesystem::exists(path, ignored)) continue;
        std::filesystem::create_directories(gHarvestFolder, ignored);
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(c), std::streamsize(total));
        if (out) ++saved;
    }
    return saved;
}

namespace {

void PutBE16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(uint8_t(value >> 8));
    out.push_back(uint8_t(value));
}

void PutBE32(std::vector<uint8_t>& out, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(uint8_t(value >> shift));
}

// A minimal container around bare pixel shader microcode, as XenosRecomp reads one: a constant
// table naming c0-c223 (g_C), c224-c255 one register each (XenosRecomp's arrays stop at c223
// for pixel shaders) and samplers s0-s15; no literals, so every constant comes from the
// registers; colour 0 out; and these interpolators (usage index, usage, register).
std::vector<uint8_t> BareContainer(const uint8_t* microcode, size_t size, const std::vector<uint32_t>& interpolators) {
    struct Entry {
        std::string name;
        uint16_t set, index, count;
        uint32_t type;
    };
    std::vector<Entry> entries{{"g_C", 2, 0, 224, 0}};
    for (uint16_t i = 0; i < 16; ++i) entries.push_back({"s" + std::to_string(i), 3, i, 1, 1});
    for (uint16_t i = 224; i < 256; ++i) entries.push_back({"g_C" + std::to_string(i), 2, i, 1, 2});

    // The constant table: its header, the constants, three types, then the names.
    std::vector<uint8_t> table;
    uint32_t infoAt = 7 * 4, typeAt = infoAt + uint32_t(entries.size()) * 20, nameAt = typeAt + 3 * 16;
    for (uint32_t value : {0u, 0u, 0u, uint32_t(entries.size()), infoAt, 0u, 0u}) PutBE32(table, value);
    std::string names;
    for (const Entry& e : entries) {
        PutBE32(table, nameAt + uint32_t(names.size()));
        PutBE16(table, e.set), PutBE16(table, e.index), PutBE16(table, e.count), PutBE16(table, 0);
        PutBE32(table, typeAt + e.type * 16), PutBE32(table, 0);
        names += e.name + '\0';
    }
    static constexpr uint16_t kTypes[3][6] = {{1, 3, 1, 4, 224, 0}, {4, 12, 1, 1, 1, 0}, {1, 3, 1, 4, 1, 0}};
    for (const auto& type : kTypes) {
        for (uint16_t value : type) PutBE16(table, value);
        PutBE32(table, 0);
    }
    table.insert(table.end(), names.begin(), names.end());
    while (table.size() % 4) table.push_back(0);

    std::vector<uint8_t> container;
    uint32_t tableAt = 0x24, shaderAt = tableAt + 4 + uint32_t(table.size());
    uint32_t virtualSize = shaderAt + 8 * 4 + uint32_t(interpolators.size()) * 4;
    for (uint32_t value : {0x102A1100u, virtualSize, uint32_t(size), 0u, tableAt, 0u, shaderAt, 0u, 0u})
        PutBE32(container, value);
    PutBE32(container, uint32_t(table.size()));
    container.insert(container.end(), table.begin(), table.end());
    // The shader: microcode at 0 in the physical part, no position input, its interpolators,
    // and colour 0.
    for (uint32_t value : {0u, uint32_t(size), 0u, 0xFF00u, 0u, uint32_t(interpolators.size()) << 5, 0u, 1u})
        PutBE32(container, value);
    for (uint32_t value : interpolators) PutBE32(container, value);
    container.insert(container.end(), microcode, microcode + size);
    return container;
}

}  // namespace

bool HarvestBarePixelShader(const uint8_t* microcode, size_t size, const Shader& vertexShader) {
    // The vertex shader's interpolators, in export order: the GPU hands export i to the pixel
    // shader's register i.
    const uint8_t* c = vertexShader.container;
    uint32_t shaderOffset = LoadBE32(c, 0x18);
    uint32_t count = LoadBE32(c, shaderOffset + 0x14) >> 5 & 0x1F;
    uint32_t skip = LoadBE32(c, shaderOffset + 0x18) + LoadBE32(c, shaderOffset + 0x1C);
    if (uint64_t(shaderOffset) + 0x24 + uint64_t(skip + count) * 4 > vertexShader.containerSize) return false;
    std::vector<uint32_t> interpolators;
    for (uint32_t i = 0; i < count; ++i)
        interpolators.push_back((LoadBE32(c, shaderOffset + 0x24 + (skip + i) * 4) & 0xFF) | i << 8);

    char name[40];
    std::snprintf(name, sizeof name, "ps_%016llX.bin", static_cast<unsigned long long>(ShaderKey(microcode, size)));
    std::error_code ignored;
    std::filesystem::path path = gHarvestFolder / name;
    if (std::filesystem::exists(path, ignored)) return false;
    std::filesystem::create_directories(gHarvestFolder, ignored);
    std::vector<uint8_t> container = BareContainer(microcode, size, interpolators);
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(container.data()), std::streamsize(container.size()));
    return bool(out);
}

Shader* Shaders(size_t& count) {
    count = gShaders.size();
    return gShaders.data();
}

}  // namespace s3n::render
