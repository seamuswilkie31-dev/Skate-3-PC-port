#include "render/draws.h"

#include "gpu/gpu.h"
#include "gpu/registers.h"
#include "kernel/kernel.h"
#include "memory.h"
#include "render/backend.h"
#include "render/shader_cache.h"
#include "render/textures.h"
#include "render/vulkan.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace s3n::render {
namespace {

using gpu::Register;
namespace reg = gpu::reg;
using namespace backend;

// The translated shaders' interface (XenosRecomp: shader_common.h, shader_recompiler.cpp).
//
// Vertex inputs have fixed locations by usage (Position 0, BlendWeight 1, BlendIndices 2,
// Normal 3, TexCoord 5, Tangent 6, Binormal 7, Color 10).
struct UsageLocation {
    uint8_t usage, index, location;
};
constexpr UsageLocation kUsageLocations[] = {
    {0, 0, 0},  {3, 0, 1},  {6, 0, 2},   {7, 0, 3},  {5, 0, 4},  {5, 1, 5},  {5, 2, 6},  {5, 3, 7},  {10, 0, 8},
    {2, 0, 9},  {1, 0, 10}, {10, 1, 11}, {5, 4, 12}, {5, 5, 13}, {5, 6, 14}, {5, 7, 15}, {0, 1, 15},
};
constexpr uint32_t kUsageBlendIndices = 2;
// Blend indices are read as integers; normals, tangents and binormals as uint4 holding
// float bits. Everything else is float4.
bool IsUintUsage(uint32_t usage) { return usage == 2 || usage == 3 || usage == 6 || usage == 7; }

// The shared constants: texture slots per sampler register for 2D, 3D and cube textures,
// sampler slots, then the booleans and a few draw values.
constexpr uint32_t kSharedSamplers = 192;  // textures: 64 bytes per kind (2D, 3D, cube) from 0
constexpr uint32_t kSharedBooleans = 256;
constexpr uint32_t kSharedHalfPixelOffset = 264;
constexpr uint32_t kSharedAlphaThreshold = 272;
constexpr uint32_t kSharedSize = 512;
constexpr uint32_t kSpecAlphaTest = 1 << 1;
constexpr uint32_t kStageConstantsSize = 256 * 16;  // one stage's float4 constants
constexpr uint32_t kMaxVertices = 1u << 20;
constexpr uint32_t kMaxAttributes = 16;
constexpr uint32_t kNoRegister = ~0u;
constexpr uint32_t kIdentitySwizzle = 0x688;  // x, y, z, w: 3 bits each

enum Skip { kNoWindow, kEdramMode, kScreenSpace, kPrimitive, kIndices, kTooBig, kNoTarget, kViewport, kPipeline,
            kSkipCount };
const char* const kSkipNames[kSkipCount] = {"no window", "EDRAM mode", "screen-space positions", "primitive type",
                                            "immediate indices", "too many vertices", "no render target",
                                            "empty viewport", "no pipeline"};
std::array<std::atomic<uint64_t>, kSkipCount> gSkipped{};
std::atomic<uint64_t> gDrawn{0};
std::atomic<uint64_t> gPipelineCount{0};

// A vertex shader input and where it comes from.
struct Element {
    uint32_t address;  // the vertex fetch instruction's slot in the container's microcode
    uint32_t usage;
    uint32_t location;
    uint32_t reg = kNoRegister;           // the register that fetch writes
    uint32_t swizzle = kIdentitySwizzle;  // and its destination swizzle
};

// One vertex fetch, as Direct3D bound it.
struct Fetch {
    uint32_t location;
    uint32_t usage;
    bool valid;
    uint32_t base, end;  // the vertex buffer's physical byte range
    uint32_t endian;
    uint32_t format;
    bool isSigned, isInteger;
    int32_t exponent;
    uint32_t stride;  // dwords
    int32_t offset;   // dwords
    // Destination swizzles: the bound one, which says which fetched component (or 0 or 1)
    // each register component gets, and the container's, which the translated shader uses.
    uint32_t swizzle, shaderSwizzle;
};

struct PipelineKey {
    const Shader* vertexShader;
    const Shader* pixelShader;
    uint32_t topology, restart;
    uint32_t attributeCount;
    uint8_t locations[kMaxAttributes];
    uint8_t uintInputs[kMaxAttributes];
    VkFormat colorFormat;
    uint32_t samples;
    uint32_t depth;
    uint32_t cullMode, frontFace;
    uint32_t depthControl, blendControl, colorMask;
    uint32_t specConstants;
};
struct KeyHash {
    size_t operator()(const PipelineKey& key) const {
        return size_t(HashMemory(reinterpret_cast<const uint8_t*>(&key), sizeof key));
    }
};
struct KeyEqual {
    bool operator()(const PipelineKey& a, const PipelineKey& b) const { return std::memcmp(&a, &b, sizeof a) == 0; }
};

std::unordered_map<const Shader*, std::vector<Element>> gElements;
std::unordered_map<PipelineKey, VkPipeline, KeyHash, KeyEqual> gPipelines;

bool gSetUp = false;
bool gUnavailable = false;
VkPipelineLayout gPipelineLayout = VK_NULL_HANDLE;
VkPipelineCache gPipelineCache = VK_NULL_HANDLE;

// The constants last uploaded for each stage: reused while they're unchanged and still in
// the recording being made.
struct UploadedConstants {
    uint64_t version = ~0ull, recording = ~0ull;
    VkDeviceAddress address = 0;
};
UploadedConstants gUploadedConstants[2];

// What the recording being made already has bound and set, so draws record only changes.
// Vulkan keeps it across rendering scopes within a command buffer.
struct RecordedState {
    uint64_t recording = ~0ull;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkViewport viewport{};
    VkRect2D scissor{};
    uint32_t stencil[2] = {};  // RB_STENCILREFMASK, front and back
    uint32_t blend[4] = {};    // blend constants' bits
    bool viewportSet = false, scissorSet = false, stencilSet = false, blendSet = false;
};
RecordedState gRecorded;

// Where the command processor's time in draws goes, for the report.
enum Phase { kPhaseIndices, kPhaseTextures, kPhaseUpload, kPhaseVertices, kPhaseRecording, kPhaseCount };
constexpr const char* kPhaseNames[kPhaseCount] = {"indices", "textures", "upload space", "vertices", "recording"};
std::array<std::atomic<uint64_t>, kPhaseCount> gPhaseNanoseconds{};
std::atomic<uint64_t> gVerticesConverted{0};
std::atomic<uint64_t> gVertexHits{0}, gVertexMisses{0};  // the vertex cache's

// Adds the time since the last lap to a phase.
struct Laps {
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
    void operator()(Phase phase) {
        auto now = std::chrono::steady_clock::now();
        gPhaseNanoseconds[phase] += std::chrono::duration_cast<std::chrono::nanoseconds>(now - last).count();
        last = now;
    }
};

std::string Describe() {
    std::string out = "draws: " + std::to_string(gDrawn.load()) + " drawn with " + std::to_string(gPipelineCount.load()) +
                      " pipelines; skipped:";
    bool any = false;
    for (int i = 0; i < kSkipCount; ++i) {
        if (uint64_t n = gSkipped[i]) {
            out += std::string(any ? "," : "") + " " + std::to_string(n) + " " + kSkipNames[i];
            any = true;
        }
    }
    out += any ? "\n" : " none\n";
    char line[64];
    out += "  time in draws:";
    for (int i = 0; i < kPhaseCount; ++i) {
        std::snprintf(line, sizeof line, "%s %s %.1f s", i ? "," : "", kPhaseNames[i], gPhaseNanoseconds[i] / 1e9);
        out += line;
    }
    return out + " (" + std::to_string(gVerticesConverted.load()) + " vertices converted; vertex cache " +
           std::to_string(gVertexHits.load()) + " hits, " + std::to_string(gVertexMisses.load()) + " misses)\n";
}

float RegisterFloat(uint32_t index) {
    uint32_t bits = Register(index);
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}

int32_t SignExtend(uint32_t value, uint32_t bits) { return int32_t(value << (32 - bits)) >> (32 - bits); }

// The pipeline layout: the texture sets (textures.h) and the three constant addresses.
bool SetUp(VkCommandBuffer cb) {
    if (gSetUp) return true;
    if (gUnavailable) return false;
    gUnavailable = true;  // until it works
    Vulkan& vk = Vk();
    std::string error;
    if (!textures::SetUp(cb)) return false;
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 3 * sizeof(uint64_t)};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 4;
    layout.pSetLayouts = textures::SetLayouts();
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    VkPipelineCacheCreateInfo cache{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    if (!Check(vkCreatePipelineLayout(vk.device, &layout, nullptr, &gPipelineLayout), "vkCreatePipelineLayout",
               error) ||
        !Check(vkCreatePipelineCache(vk.device, &cache, nullptr, &gPipelineCache), "vkCreatePipelineCache", error)) {
        kernel::Log("  draws: can't set up: %s\n", error.c_str());
        return false;
    }
    kernel::AddReportSection(Describe);
    gUnavailable = false;
    gSetUp = true;
    return true;
}

// The vertex shader's inputs, from its container: each says which fetch instruction
// (by slot) feeds which usage.
const std::vector<Element>& VertexElements(const Shader* shader) {
    auto it = gElements.find(shader);
    if (it != gElements.end()) return it->second;
    std::vector<Element> elements;
    const uint8_t* c = shader->container;
    uint32_t shaderOffset = LoadBE32(c, 0x18);
    if (uint64_t(shaderOffset) + 0x24 <= shader->containerSize) {
        uint32_t first = LoadBE32(c, shaderOffset + 0x18), count = LoadBE32(c, shaderOffset + 0x1C);
        for (uint32_t i = 0; i < count; ++i) {
            uint64_t at = uint64_t(shaderOffset) + 0x24 + uint64_t(first + i) * 4;
            if (at + 4 > shader->containerSize) break;
            uint32_t value = LoadBE32(c, uint32_t(at));
            uint32_t usage = value >> 12 & 0xF, index = value >> 16 & 0xF;
            for (const UsageLocation& u : kUsageLocations) {
                if (u.usage == usage && u.index == index) {
                    Element e{value & 0xFFF, usage, u.location};
                    if (uint64_t(e.address) * 12 + 12 <= shader->microcodeSize) {
                        uint32_t w0 = LoadBE32(shader->microcode, e.address * 12);
                        if ((w0 & 0x1F) == 0) {
                            e.reg = w0 >> 12 & 0x3F;
                            e.swizzle = LoadBE32(shader->microcode, e.address * 12 + 4) & 0xFFF;
                        }
                    }
                    elements.push_back(e);
                    break;
                }
            }
        }
    }
    std::sort(elements.begin(), elements.end(), [](auto& a, auto& b) { return a.address < b.address; });
    return gElements.emplace(shader, std::move(elements)).first->second;
}

// The fetches as bound: each element's vertex fetch instruction in the microcode the GPU
// was given. Direct3D writes the fetches in the order of the vertex declaration, which
// needn't be the shader's, so an element's fetch is the one among the elements' slots that
// writes its register. A mini fetch reuses the buffer and stride of the full fetch before it.
void BoundFetches(const DrawCall& call, std::vector<Fetch>& fetches) {
    fetches.clear();
    auto word = [&call](uint32_t slot, uint32_t i) { return LoadBE32(call.vertexMicrocode, slot * 12 + i * 4); };
    auto inside = [&call](uint32_t slot) { return uint64_t(slot) * 12 + 12 <= call.vertexMicrocodeSize; };
    const std::vector<Element>& elements = VertexElements(call.vertexShader);
    std::pair<uint32_t, const Element*> bound[kMaxAttributes];
    uint32_t count = 0;
    for (const Element& e : elements) {
        if (count == kMaxAttributes) break;
        uint32_t slot = e.address;
        for (const Element& other : elements) {
            if (e.reg == kNoRegister || !inside(other.address)) continue;
            uint32_t w0 = word(other.address, 0);
            if ((w0 & 0x1F) == 0 && (w0 >> 12 & 0x3F) == e.reg) {
                slot = other.address;
                break;
            }
        }
        bound[count++] = {slot, &e};
    }
    std::sort(bound, bound + count, [](auto& a, auto& b) { return a.first < b.first; });

    uint32_t constant = 0, stride = 0;
    for (uint32_t i = 0; i < count; ++i) {
        auto [slot, element] = bound[i];
        const Element& e = *element;
        Fetch f{};
        f.location = e.location;
        f.usage = e.usage;
        f.swizzle = f.shaderSwizzle = e.swizzle;
        if (inside(slot)) {
            uint32_t w0 = word(slot, 0), w1 = word(slot, 1), w2 = word(slot, 2);
            if ((w0 & 0x1F) == 0) {  // a vertex fetch
                if (!(w1 >> 30 & 1)) {
                    constant = (w0 >> 20 & 0x1F) * 3 + (w0 >> 25 & 3);
                    stride = w2 & 0xFF;
                }
                uint32_t address = Register(reg::kFetchConstants + constant * 2);
                uint32_t info = Register(reg::kFetchConstants + constant * 2 + 1);
                f.valid = true;
                f.base = address & 0x1FFFFFFC;
                f.end = f.base + (info >> 2 & 0xFFFFFF) * 4;
                f.endian = info & 3;
                f.format = w1 >> 16 & 0x3F;
                f.isSigned = w1 >> 12 & 1;
                f.isInteger = w1 >> 13 & 1;
                f.exponent = SignExtend(w1 >> 24 & 0x3F, 6);
                f.stride = stride;
                f.offset = SignExtend(w2 >> 8 & 0x7FFFFF, 23);
                f.swizzle = w1 & 0xFFF;
            }
        }
        fetches.push_back(f);
    }
}

uint32_t Swap(uint32_t word, uint32_t endian) {
    switch (endian) {
    case 1: return (word & 0x00FF00FF) << 8 | (word >> 8 & 0x00FF00FF);  // 8in16
    case 2: return __builtin_bswap32(word);                               // 8in32
    case 3: return word << 16 | word >> 16;                               // 16in32
    default: return word;
    }
}

constexpr uint32_t FormatWords(uint32_t format) {
    switch (format) {
    case 6: case 7: case 16: case 17: case 25: case 31: case 33: case 36: return 1;
    case 26: case 32: case 34: case 37: return 2;
    case 57: return 3;
    case 35: case 38: return 4;
    default: return 0;
    }
}

// The GPU's half floats have no infinities: exponent 31 is an ordinary exponent.
float Half(uint32_t h) {
    uint32_t exponent = h >> 10 & 0x1F, mantissa = h & 0x3FF;
    float value = exponent ? std::ldexp(1.0f + mantissa / 1024.0f, int(exponent) - 15) : std::ldexp(float(mantissa), -24);
    return h & 0x8000 ? -value : value;
}

// All 65,536 half floats, worked out once.
const float* Halves() {
    static const std::vector<float> table = [] {
        std::vector<float> t(65536);
        for (uint32_t h = 0; h < 65536; ++h) t[h] = Half(h);
        return t;
    }();
    return table.data();
}

// Bits in each component of the packed integer formats, from the low end of each dword.
void ComponentBits(uint32_t format, uint32_t bits[4]) {
    static constexpr struct {
        uint32_t format;
        uint8_t bits[4];
    } kFormats[] = {
        {6, {8, 8, 8, 8}},     {7, {10, 10, 10, 2}},  {16, {11, 11, 10, 0}},  {17, {10, 11, 11, 0}}, {25, {16, 16, 0, 0}},
        {26, {16, 16, 16, 16}}, {33, {32, 0, 0, 0}}, {34, {32, 32, 0, 0}}, {35, {32, 32, 32, 32}},
    };
    for (const auto& f : kFormats)
        if (f.format == format)
            for (int i = 0; i < 4; ++i) bits[i] = f.bits[i];
}

// How one fetch's elements become shader inputs, worked out once for a draw's vertices.
// Integer components are scaled to normalised values (times 2^exponent); signed normalised
// ones stop at -1. A destination swizzle gives, for each register component, the fetched
// component (0-3), 0 (4) or 1 (5) it gets, or nothing (7). Direct3D sets the bound one for
// the declared type: 16-bit pairs in a big-endian buffer come out of the endian swap high
// half first, so a SHORT2 takes y, x. The translated shader reads its input through the
// container's swizzle, so each value goes where that one looks for it.
struct Decoder {
    const uint8_t* buffer = nullptr;  // the vertex buffer
    int64_t at = 0, step = 0;         // vertex 0's element in it, and bytes per vertex
    uint32_t begin = 0, end = 0;      // the vertices whose element lies inside the buffer
    float scale[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float low[4] = {-FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX};
    int8_t from[4] = {0, 1, 2, 3};  // each input component's fetched component, or 4 (0) or 5 (1)
};

Decoder MakeDecoder(const Fetch& f, uint32_t first, uint32_t count) {
    Decoder d;
    for (int i = 0; i < 4; ++i) {
        uint32_t from = f.swizzle >> (3 * i) & 7, to = f.shaderSwizzle >> (3 * i) & 7;
        if (to <= 3 && from <= 5) d.from[to] = int8_t(from);  // else a constant, or nothing written
    }
    uint32_t words = FormatWords(f.format);
    if (!f.valid || !words) return d;
    uint32_t bits[4] = {};
    ComponentBits(f.format, bits);
    float exponent = std::ldexp(1.0f, f.exponent);
    for (int i = 0; i < 4; ++i) {
        if (!bits[i]) continue;
        double range = f.isInteger ? 1.0 : double((1ull << (f.isSigned ? bits[i] - 1 : bits[i])) - 1);
        d.scale[i] = float(exponent / range);
        if (f.isSigned && !f.isInteger) d.low[i] = -exponent;
    }
    d.buffer = gpu::PhysicalMemory(f.base);
    d.at = (int64_t(first) * f.stride + f.offset) * 4;
    d.step = int64_t(f.stride) * 4;
    int64_t last = int64_t(f.end) - f.base - words * 4;  // the last place an element fits
    int64_t begin = 0, end = count;
    if (d.step) {
        if (d.at < 0) begin = (-d.at + d.step - 1) / d.step;
        end = d.at > last ? 0 : (last - d.at) / d.step + 1;
    } else if (d.at < 0 || d.at > last) {
        end = 0;
    }
    d.end = uint32_t(std::clamp<int64_t>(end, 0, count));
    d.begin = uint32_t(std::clamp<int64_t>(begin, 0, d.end));
    return d;
}

// One fetch's elements for vertices [0, count), as the shader's inputs, every `stride` bytes
// of `out`. Components the format lacks are 0, 0, 0, 1; so is all of a vertex outside the
// buffer.
template <uint32_t kFormat>
void Decode(const Fetch& f, const Decoder& d, uint32_t count, uint8_t* out, uint32_t stride) {
    constexpr uint32_t kWords = FormatWords(kFormat);
    constexpr bool kFloat = kFormat == 31 || kFormat == 32 || kFormat >= 36;
    const float* halves = Halves();
    bool isSigned = f.isSigned, indices = f.usage == kUsageBlendIndices;
    uint32_t endian = f.endian;
    auto field = [isSigned](uint32_t word, uint32_t shift, uint32_t bits) {
        uint32_t raw = uint32_t(word >> shift & ((1ull << bits) - 1));
        return isSigned ? float(SignExtend(raw, bits)) : float(raw);
    };
    for (uint32_t v = 0; v < count; ++v, out += stride) {
        float c[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        if (kWords && v >= d.begin && v < d.end) {
            const uint8_t* data = d.buffer + (d.at + int64_t(v) * d.step);
            uint32_t w[kWords ? kWords : 1];
            for (uint32_t i = 0; i < kWords; ++i) {
                std::memcpy(&w[i], data + i * 4, 4);
                w[i] = Swap(w[i], endian);
            }
            if constexpr (kFormat == 6) {
                for (int i = 0; i < 4; ++i) c[i] = field(w[0], 8 * i, 8);
            } else if constexpr (kFormat == 7) {
                c[0] = field(w[0], 0, 10), c[1] = field(w[0], 10, 10), c[2] = field(w[0], 20, 10), c[3] = field(w[0], 30, 2);
            } else if constexpr (kFormat == 16) {
                c[0] = field(w[0], 0, 11), c[1] = field(w[0], 11, 11), c[2] = field(w[0], 22, 10);
            } else if constexpr (kFormat == 17) {
                c[0] = field(w[0], 0, 10), c[1] = field(w[0], 10, 11), c[2] = field(w[0], 21, 11);
            } else if constexpr (kFormat == 25 || kFormat == 26) {
                for (uint32_t i = 0; i < kWords * 2; ++i) c[i] = field(w[i / 2], 16 * (i & 1), 16);
            } else if constexpr (kFormat >= 33 && kFormat <= 35) {
                for (uint32_t i = 0; i < kWords; ++i) c[i] = isSigned ? float(int32_t(w[i])) : float(w[i]);
            } else if constexpr (kFormat == 31 || kFormat == 32) {
                for (uint32_t i = 0; i < kWords * 2; ++i) c[i] = halves[w[i / 2] >> (16 * (i & 1)) & 0xFFFF];
            } else if constexpr (kFloat) {
                std::memcpy(c, w, kWords * 4);
            }
            if constexpr (!kFloat)
                for (int i = 0; i < 4; ++i) c[i] = std::max(c[i] * d.scale[i], d.low[i]);
        }
        float input[4];
        for (int i = 0; i < 4; ++i) input[i] = d.from[i] < 4 ? c[d.from[i]] : float(d.from[i] - 4);
        if (indices) {
            uint32_t values[4];
            for (int i = 0; i < 4; ++i) values[i] = uint32_t(std::max(input[i], 0.0f));
            std::memcpy(out, values, 16);
        } else {
            std::memcpy(out, input, 16);
        }
    }
}

// Vertices [first, first + count) of every fetch, 16 bytes each, interleaved.
void ConvertVertices(const std::vector<Fetch>& fetches, uint32_t first, uint32_t count, uint8_t* out) {
    uint32_t stride = uint32_t(fetches.size()) * 16;
    for (size_t e = 0; e < fetches.size(); ++e) {
        const Fetch& f = fetches[e];
        Decoder d = MakeDecoder(f, first, count);
        uint8_t* column = out + e * 16;
        switch (f.format) {
        case 6: Decode<6>(f, d, count, column, stride); break;
        case 7: Decode<7>(f, d, count, column, stride); break;
        case 16: Decode<16>(f, d, count, column, stride); break;
        case 17: Decode<17>(f, d, count, column, stride); break;
        case 25: Decode<25>(f, d, count, column, stride); break;
        case 26: Decode<26>(f, d, count, column, stride); break;
        case 31: Decode<31>(f, d, count, column, stride); break;
        case 32: Decode<32>(f, d, count, column, stride); break;
        case 33: Decode<33>(f, d, count, column, stride); break;
        case 34: Decode<34>(f, d, count, column, stride); break;
        case 35: Decode<35>(f, d, count, column, stride); break;
        case 36: Decode<36>(f, d, count, column, stride); break;
        case 37: Decode<37>(f, d, count, column, stride); break;
        case 38: Decode<38>(f, d, count, column, stride); break;
        case 57: Decode<57>(f, d, count, column, stride); break;
        default: Decode<0>(f, d, count, column, stride); break;  // unsupported: defaults
        }
    }
}

// Converted vertices, kept between draws. Most of the world is drawn every frame, and again
// in each pass (shadows, reflections), from the same vertex buffers, so each run of
// vertices is converted once and checked once a frame against a hash of the memory it came
// from. Small draws (the interface, effects) are converted every time.
constexpr uint32_t kCachedVertices = 64;  // the fewest vertices a draw needs to be cached
constexpr uint64_t kCacheFrames = 120;    // an entry unused for this many frames is dropped

struct VertexEntry {
    std::vector<Fetch> fetches;  // as bound: with first and count, the key
    uint32_t first = 0, count = 0;
    uint64_t sourceHash = 0;
    uint64_t checkedFrame = ~0ull, usedFrame = 0;
    std::vector<uint8_t> data;  // as ConvertVertices writes it
};
std::unordered_map<uint64_t, VertexEntry> gVertexCache;
uint64_t gVertexCacheSwept = 0;

bool SameFetch(const Fetch& a, const Fetch& b) {
    return a.location == b.location && a.usage == b.usage && a.valid == b.valid && a.base == b.base && a.end == b.end &&
           a.endian == b.endian && a.format == b.format && a.isSigned == b.isSigned && a.isInteger == b.isInteger &&
           a.exponent == b.exponent && a.stride == b.stride && a.offset == b.offset && a.swizzle == b.swizzle &&
           a.shaderSwizzle == b.shaderSwizzle;
}

uint64_t VertexKey(const std::vector<Fetch>& fetches, uint32_t first, uint32_t count) {
    uint64_t hash = 0xCBF29CE484222325ull;
    auto mix = [&hash](uint64_t value) {
        hash = (hash ^ value) * 0x100000001B3ull;
        hash ^= hash >> 31;
    };
    mix(uint64_t(first) << 32 | count);
    for (const Fetch& f : fetches) {
        mix(uint64_t(f.base) << 32 | f.end);
        mix(uint64_t(f.format) | uint64_t(f.endian) << 8 | uint64_t(f.isSigned) << 10 | uint64_t(f.isInteger) << 11 |
            uint64_t(f.valid) << 12 | uint64_t(f.location) << 16 | uint64_t(f.usage) << 24 |
            uint64_t(uint32_t(f.exponent)) << 32);
        mix(uint64_t(f.stride) << 32 | uint32_t(f.offset));
        mix(uint64_t(f.swizzle) << 32 | f.shaderSwizzle);
    }
    return hash;
}

// A hash of the memory the vertices come from: each vertex buffer's span of them.
uint64_t SourceHash(const std::vector<Fetch>& fetches, uint32_t first, uint32_t count) {
    uint64_t hash = 0;
    std::pair<uint32_t, uint32_t> done[kMaxAttributes];  // buffer and stride
    uint32_t doneCount = 0;
    for (const Fetch& f : fetches) {
        if (!f.valid || std::find(done, done + doneCount, std::pair{f.base, f.stride}) != done + doneCount) continue;
        done[doneCount++] = {f.base, f.stride};
        uint64_t start = f.stride ? uint64_t(f.base) + uint64_t(first) * f.stride * 4 : f.base + int64_t(f.offset) * 4;
        uint64_t end = std::min<uint64_t>(f.stride ? start + uint64_t(count) * f.stride * 4 : start + 16, f.end);
        if (start < f.base || start >= end) continue;
        hash = hash * 31 ^ HashMemory(gpu::PhysicalMemory(uint32_t(start)), size_t(end - start));
    }
    return hash;
}

// ConvertVertices, through the cache.
void CachedVertices(const std::vector<Fetch>& fetches, uint32_t first, uint32_t count, uint8_t* out) {
    size_t size = size_t(count) * fetches.size() * 16;
    uint64_t frame = FrameNumber();
    VertexEntry& entry = gVertexCache[VertexKey(fetches, first, count)];
    bool same = entry.first == first && entry.count == count && entry.fetches.size() == fetches.size() &&
                std::equal(fetches.begin(), fetches.end(), entry.fetches.begin(), SameFetch);
    if (same && entry.checkedFrame != frame) {
        entry.checkedFrame = frame;
        same = SourceHash(fetches, first, count) == entry.sourceHash;
    }
    if (same) {
        ++gVertexHits;
    } else {
        ++gVertexMisses;
        entry.fetches = fetches;
        entry.first = first;
        entry.count = count;
        entry.sourceHash = SourceHash(fetches, first, count);
        entry.checkedFrame = frame;
        entry.data.resize(size);
        ConvertVertices(fetches, first, count, entry.data.data());
        gVerticesConverted += count;
    }
    entry.usedFrame = frame;
    std::memcpy(out, entry.data.data(), size);
    if (frame >= gVertexCacheSwept + kCacheFrames / 2) {
        gVertexCacheSwept = frame;
        std::erase_if(gVertexCache, [frame](const auto& item) { return item.second.usedFrame + kCacheFrames < frame; });
    }
}

// An index buffer's indices: endian-swapped dwords, low half first for 16-bit ones.
template <uint32_t kEndian, bool kWide>
void ReadIndices(const uint8_t* data, uint32_t count, uint32_t* out) {
    uint32_t word;
    if constexpr (kWide) {
        for (uint32_t i = 0; i < count; ++i) {
            std::memcpy(&word, data + i * 4, 4);
            out[i] = Swap(word, kEndian);
        }
    } else {
        for (uint32_t i = 0; i < count; i += 2) {
            std::memcpy(&word, data + i * 2, 4);
            word = Swap(word, kEndian);
            out[i] = word & 0xFFFF;
            if (i + 1 < count) out[i + 1] = word >> 16;
        }
    }
}

void ReadIndices(const uint8_t* data, uint32_t count, bool wide, uint32_t endian, uint32_t* out) {
    switch (endian << 1 | uint32_t(wide)) {
    case 0: ReadIndices<0, false>(data, count, out); break;
    case 1: ReadIndices<0, true>(data, count, out); break;
    case 2: ReadIndices<1, false>(data, count, out); break;
    case 3: ReadIndices<1, true>(data, count, out); break;
    case 4: ReadIndices<2, false>(data, count, out); break;
    case 5: ReadIndices<2, true>(data, count, out); break;
    case 6: ReadIndices<3, false>(data, count, out); break;
    default: ReadIndices<3, true>(data, count, out); break;
    }
}

VkBlendFactor BlendFactor(uint32_t factor) {
    static constexpr VkBlendFactor kFactors[17] = {
        VK_BLEND_FACTOR_ZERO,           VK_BLEND_FACTOR_ONE,
        VK_BLEND_FACTOR_ONE,            VK_BLEND_FACTOR_ONE,  // 2, 3: unused
        VK_BLEND_FACTOR_SRC_COLOR,      VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        VK_BLEND_FACTOR_SRC_ALPHA,      VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_FACTOR_DST_COLOR,      VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
        VK_BLEND_FACTOR_DST_ALPHA,      VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
        VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
        VK_BLEND_FACTOR_CONSTANT_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
        VK_BLEND_FACTOR_SRC_ALPHA_SATURATE};
    return factor < 17 ? kFactors[factor] : VK_BLEND_FACTOR_ONE;
}

VkBlendOp BlendOp(uint32_t op) {
    static constexpr VkBlendOp kOps[5] = {VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX,
                                          VK_BLEND_OP_REVERSE_SUBTRACT};
    return op < 5 ? kOps[op] : VK_BLEND_OP_ADD;
}

// The GPU's compare functions and stencil operations are in Vulkan's order.
VkStencilOpState Stencil(uint32_t control, int shift) {
    VkStencilOpState state{};
    state.compareOp = VkCompareOp(control >> shift & 7);
    state.failOp = VkStencilOp(control >> (shift + 3) & 7);
    state.passOp = VkStencilOp(control >> (shift + 6) & 7);
    state.depthFailOp = VkStencilOp(control >> (shift + 9) & 7);
    return state;
}

VkPipeline CreatePipeline(const PipelineKey& key) {
    Vulkan& vk = Vk();
    VkSpecializationMapEntry specEntry{0, 0, sizeof(uint32_t)};
    uint32_t vertexSpec = 0;
    VkSpecializationInfo specs[2] = {{1, &specEntry, sizeof(uint32_t), &vertexSpec},
                                     {1, &specEntry, sizeof(uint32_t), &key.specConstants}};
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (int i = 0; i < 2; ++i) {
        stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
        stages[i].module = VkShaderModule((i ? key.pixelShader : key.vertexShader)->module);
        stages[i].pName = "main";
        stages[i].pSpecializationInfo = &specs[i];
    }

    VkVertexInputBindingDescription binding{0, key.attributeCount * 16, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[16];
    for (uint32_t i = 0; i < key.attributeCount; ++i)
        attributes[i] = {key.locations[i], 0,
                         key.uintInputs[i] ? VK_FORMAT_R32G32B32A32_UINT : VK_FORMAT_R32G32B32A32_SFLOAT, i * 16};
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = key.attributeCount ? 1 : 0;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = key.attributeCount;
    vertexInput.pVertexAttributeDescriptions = attributes;

    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VkPrimitiveTopology(key.topology);
    assembly.primitiveRestartEnable = key.restart;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = key.cullMode;
    raster.frontFace = VkFrontFace(key.frontFace);
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VkSampleCountFlagBits(key.samples);

    uint32_t dc = key.depthControl;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    if (key.depth) {
        depth.depthTestEnable = dc >> 1 & 1;
        depth.depthWriteEnable = (dc >> 1 & 1) && (dc >> 2 & 1);
        depth.depthCompareOp = VkCompareOp(dc >> 4 & 7);
        depth.stencilTestEnable = dc & 1;
        depth.front = Stencil(dc, 8);
        depth.back = dc >> 7 & 1 ? Stencil(dc, 20) : depth.front;
    }

    uint32_t bc = key.blendControl;
    VkPipelineColorBlendAttachmentState blend{};
    blend.srcColorBlendFactor = BlendFactor(bc & 0x1F);
    blend.colorBlendOp = BlendOp(bc >> 5 & 7);
    blend.dstColorBlendFactor = BlendFactor(bc >> 8 & 0x1F);
    blend.srcAlphaBlendFactor = BlendFactor(bc >> 16 & 0x1F);
    blend.alphaBlendOp = BlendOp(bc >> 21 & 7);
    blend.dstAlphaBlendFactor = BlendFactor(bc >> 24 & 0x1F);
    blend.blendEnable = (bc & 0x1FFF1FFF) != 0x00010001;  // not "one, zero, add" for both
    blend.colorWriteMask = key.colorMask;
    VkPipelineColorBlendStateCreateInfo colorBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlend.attachmentCount = key.colorFormat ? 1 : 0;
    colorBlend.pAttachments = &blend;

    VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT,           VK_DYNAMIC_STATE_SCISSOR,
                                 VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                                 VK_DYNAMIC_STATE_STENCIL_REFERENCE,  VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = uint32_t(std::size(dynamics));
    dynamic.pDynamicStates = dynamics;

    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = key.colorFormat ? 1 : 0;
    rendering.pColorAttachmentFormats = &key.colorFormat;
    rendering.depthAttachmentFormat = key.depth ? vk.depthFormat : VK_FORMAT_UNDEFINED;
    rendering.stencilAttachmentFormat = key.depth ? vk.depthFormat : VK_FORMAT_UNDEFINED;

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &colorBlend;
    info.pDynamicState = &dynamic;
    info.layout = gPipelineLayout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult result = vkCreateGraphicsPipelines(vk.device, gPipelineCache, 1, &info, nullptr, &pipeline);
    if (result != VK_SUCCESS) {
        static int logged = 0;
        if (logged++ < 8)
            kernel::Log("  draws: can't make a pipeline for shaders %016llX/%016llX (VkResult %d)\n",
                        key.vertexShader->containerHash, key.pixelShader->containerHash, int(result));
        pipeline = VK_NULL_HANDLE;
    }
    ++gPipelineCount;
    return pipeline;
}

VkPipeline FindPipeline(const PipelineKey& key) {
    auto it = gPipelines.find(key);
    if (it != gPipelines.end()) return it->second;
    return gPipelines.emplace(key, CreatePipeline(key)).first->second;
}

size_t Align(size_t value) { return (value + 255) & ~size_t(255); }

}  // namespace

void Draw(const DrawCall& call) {
    Laps lap;

    // Where it draws.
    uint32_t mode = Register(reg::kRbModeControl) & 7;
    if (mode != reg::kEdramModeColorDepth && mode != reg::kEdramModeDepth) {
        ++gSkipped[kEdramMode];
        return;
    }
    if ((Register(reg::kPaClVteCntl) & 0x3F) != 0x3F) {  // positions already on screen: not yet
        ++gSkipped[kScreenSpace];
        return;
    }

    // What it draws: the primitive, and the index range its vertices come from.
    uint32_t primitive = call.initiator & 0x3F, source = call.initiator >> 6 & 3, count = call.initiator >> 16;
    VkPrimitiveTopology topology;
    switch (primitive) {
    case 2: topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
    case 3: topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
    case 4: case 13: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;  // quads become triangle pairs
    case 5: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
    case 6: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
    default: ++gSkipped[kPrimitive]; return;
    }
    if (source == 1) {
        ++gSkipped[kIndices];
        return;
    }
    if (!count) return;
    uint32_t modeControl = Register(reg::kPaSuScModeCntl);
    bool strip = primitive == 3 || primitive == 5 || primitive == 6;
    bool restart = strip && (modeControl >> 21 & 1);
    uint32_t indexOffset = Register(reg::kVgtIndxOffset);

    // Indices from guest memory (endian-swapped dwords, low half first for 16-bit ones),
    // rebased to the first vertex used; auto-indexed draws need none unless they're quads.
    static thread_local std::vector<uint32_t> indices;
    indices.clear();
    uint32_t first = indexOffset, last = indexOffset + count - 1;
    bool indexed = source == 0 || primitive == 13;
    if (source == 0) {
        bool wide = call.initiator >> 11 & 1;
        uint32_t endian = call.indexSize >> 30, available = call.indexSize & 0xFFFFFF;
        count = std::min(count, available);
        uint32_t resetIndex = Register(reg::kVgtMultiPrimIbResetIndx) & (wide ? 0xFFFFFFFF : 0xFFFF);
        indices.resize(count);
        uint32_t* p = indices.data();
        ReadIndices(gpu::PhysicalMemory(call.indexBase & (wide ? ~3u : ~1u)), count, wide, endian, p);
        if (restart) {
            first = UINT32_MAX, last = 0;
            for (uint32_t i = 0; i < count; ++i) {
                if (p[i] == resetIndex) {
                    p[i] = UINT32_MAX;
                    continue;
                }
                p[i] += indexOffset;
                first = std::min(first, p[i]), last = std::max(last, p[i]);
            }
            if (first > last) return;  // nothing but restarts
            for (uint32_t i = 0; i < count; ++i)
                if (p[i] != UINT32_MAX) p[i] -= first;
        } else {
            uint32_t low = UINT32_MAX, high = 0;
            for (uint32_t i = 0; i < count; ++i) low = std::min(low, p[i]), high = std::max(high, p[i]);
            if (low > high) return;  // no indices
            for (uint32_t i = 0; i < count; ++i) p[i] -= low;
            first = low + indexOffset, last = high + indexOffset;
        }    } else if (primitive == 13) {
        for (uint32_t i = 0; i < count; ++i) indices.push_back(i);
    }
    if (primitive == 13) {  // quads (a, b, c, d) as triangles (a, b, c) and (a, c, d)
        static thread_local std::vector<uint32_t> quads;
        quads.swap(indices);
        indices.clear();
        for (size_t q = 0; q + 3 < quads.size(); q += 4)
            for (size_t i : {0, 1, 2, 0, 2, 3}) indices.push_back(quads[q + i]);
        if (indices.empty()) return;
    }
    lap(kPhaseIndices);
    uint32_t vertexCount = last - first + 1;
    if (vertexCount > kMaxVertices) {
        ++gSkipped[kTooBig];
        return;
    }

    static thread_local std::vector<Fetch> fetches;
    BoundFetches(call, fetches);
    uint32_t vertexStride = uint32_t(fetches.size()) * 16;

    // Textures first: converting one records an upload.
    VkCommandBuffer setup = Commands();
    if (!setup || !SetUp(setup)) {
        ++gSkipped[kNoWindow];
        return;
    }
    textures::Slots slots[16];
    uint32_t textureMask = (call.vertexShader->textureFetchMask | call.pixelShader->textureFetchMask) & 0xFFFF;
    for (uint32_t s = 0; s < 16; ++s)
        if (textureMask >> s & 1) slots[s] = textures::Bind(s);
    lap(kPhaseTextures);

    // Everything the draw reads, in one block of the upload ring: each stage's constants
    // unless they're unchanged in this recording, the shared constants, the vertices and
    // the indices.
    bool stale[2];
    size_t sharedAt, vertexAt, indexAt, total;
    auto layOut = [&] {
        for (uint32_t stage = 0; stage < 2; ++stage) {
            const UploadedConstants& u = gUploadedConstants[stage];
            stale[stage] = u.recording != RecordingNumber() || u.version != gpu::ConstantsVersion(stage);
        }
        sharedAt = (size_t(stale[0]) + stale[1]) * kStageConstantsSize;
        vertexAt = Align(sharedAt + kSharedSize);
        indexAt = Align(vertexAt + size_t(vertexCount) * vertexStride);
        total = indexAt + indices.size() * 4;
    };
    layOut();
    uint64_t recording = RecordingNumber();
    Upload upload;
    VkCommandBuffer cb = Allocate(total, upload);
    if (cb && RecordingNumber() != recording) {  // the ring was full: a new recording has none
        layOut();
        cb = Allocate(total, upload);
    }
    if (!cb) {
        ++gSkipped[kNoWindow];
        return;
    }
    lap(kPhaseUpload);
    const uint32_t* registers = gpu::ConstantRegisters();
    size_t constantsAt = 0;
    for (uint32_t stage = 0; stage < 2; ++stage) {
        if (!stale[stage]) continue;
        std::memcpy(upload.data + constantsAt, registers + (stage ? reg::kPixelAluConstants : reg::kAluConstants),
                    kStageConstantsSize);
        gUploadedConstants[stage] = {gpu::ConstantsVersion(stage), RecordingNumber(), upload.address + constantsAt};
        constantsAt += kStageConstantsSize;
    }
    uint8_t* shared = upload.data + sharedAt;
    std::memset(shared, 0, kSharedSize);  // slot 0: white, and the default sampler
    for (uint32_t s = 0; s < 16; ++s) {
        std::memcpy(shared + slots[s].kind * 64 + s * 4, &slots[s].texture, 4);
        std::memcpy(shared + kSharedSamplers + s * 4, &slots[s].sampler, 4);
    }
    uint32_t booleans = (registers[reg::kBoolConstants] & 0xFFFF) | registers[reg::kBoolConstants + 4] << 16;
    std::memcpy(shared + kSharedBooleans, &booleans, 4);
    float scale[6];
    for (int i = 0; i < 6; ++i) scale[i] = RegisterFloat(reg::kPaClVportXScale + i);
    if (scale[0] <= 0.0f || scale[2] == 0.0f) {
        ++gSkipped[kViewport];
        return;
    }
    if (!(Register(reg::kPaSuVtxCntl) & 1)) {  // Direct3D's pixel centres are at integers: move half a pixel
        float offset[2] = {0.5f / scale[0], 0.5f / scale[2]};
        std::memcpy(shared + kSharedHalfPixelOffset, offset, 8);
    }
    uint32_t colorControl = Register(reg::kRbColorControl);
    bool alphaTest = (colorControl >> 3 & 1) && (colorControl & 7) != 7 &&
                     (call.pixelShader->specConstantsMask & kSpecAlphaTest);
    float alphaReference = RegisterFloat(reg::kRbAlphaRef);
    std::memcpy(shared + kSharedAlphaThreshold, &alphaReference, 4);
    if (vertexCount >= kCachedVertices) {
        CachedVertices(fetches, first, vertexCount, upload.data + vertexAt);
    } else {
        ConvertVertices(fetches, first, vertexCount, upload.data + vertexAt);
        gVerticesConverted += vertexCount;
    }
    if (!indices.empty()) std::memcpy(upload.data + indexAt, indices.data(), indices.size() * 4);
    lap(kPhaseVertices);

    // The render targets.
    uint32_t surface = Register(reg::kRbSurfaceInfo), pitch = surface & 0x3FFF, msaa = surface >> 16 & 3;
    Image* color = nullptr;
    if (mode == reg::kEdramModeColorDepth) {
        uint32_t info = Register(reg::kRbColorInfo);
        color = FindTarget(info & 0xFFF, pitch, msaa, info >> 16 & 0xF, false, cb);
    }
    uint32_t depthInfo = Register(reg::kRbDepthInfo);
    Image* depth = FindTarget(depthInfo & 0xFFF, pitch, msaa, depthInfo >> 16 & 1, true, cb);
    if (!color && !depth) {
        ++gSkipped[kNoTarget];
        return;
    }
    int32_t targetWidth = int32_t(std::min(color ? color->width : UINT32_MAX, depth ? depth->width : UINT32_MAX));
    int32_t targetHeight = int32_t(std::min(color ? color->height : UINT32_MAX, depth ? depth->height : UINT32_MAX));

    PipelineKey key;
    std::memset(&key, 0, sizeof key);
    key.vertexShader = call.vertexShader;
    key.pixelShader = call.pixelShader;
    key.topology = topology;
    key.restart = restart;
    key.attributeCount = uint32_t(fetches.size());
    for (size_t i = 0; i < fetches.size(); ++i) {
        key.locations[i] = uint8_t(fetches[i].location);
        key.uintInputs[i] = IsUintUsage(fetches[i].usage);
    }
    key.colorFormat = color ? color->format : VK_FORMAT_UNDEFINED;
    key.samples = (color ? color : depth)->samples;
    key.depth = depth != nullptr;
    key.cullMode = (modeControl & 1 ? VK_CULL_MODE_FRONT_BIT : 0) | (modeControl & 2 ? VK_CULL_MODE_BACK_BIT : 0);
    key.frontFace = modeControl & 4 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    key.depthControl = Register(reg::kRbDepthControl);
    key.blendControl = Register(reg::kRbBlendControl0);
    key.colorMask = Register(reg::kRbColorMask) & 0xF;
    key.specConstants = alphaTest ? kSpecAlphaTest : 0;
    VkPipeline pipeline = FindPipeline(key);
    if (!pipeline) {
        ++gSkipped[kPipeline];
        return;
    }

    // Viewport and scissor, moved by the window offset (each tile of a tiled screen). The
    // GPU puts a position at offset + position * scale on screen. The translated vertex
    // shaders already negate y for Vulkan (DXC's -fvk-invert-y), so the viewport's height
    // is -2 * y scale (positive: Direct3D's y scale is negative) from offset + y scale.
    uint32_t windowOffset = Register(reg::kPaScWindowOffset);
    int32_t offsetX = SignExtend(windowOffset & 0x7FFF, 15), offsetY = SignExtend(windowOffset >> 16 & 0x7FFF, 15);
    float vx = modeControl >> 16 & 1 ? float(offsetX) : 0.0f, vy = modeControl >> 16 & 1 ? float(offsetY) : 0.0f;
    VkViewport viewport{scale[1] - scale[0] + vx,
                        scale[3] + scale[2] + vy,
                        2.0f * scale[0],
                        -2.0f * scale[2],
                        std::clamp(scale[5], 0.0f, 1.0f),
                        std::clamp(scale[5] + scale[4], 0.0f, 1.0f)};
    uint32_t tl = Register(reg::kPaScWindowScissorTl), br = Register(reg::kPaScWindowScissorBr);
    int32_t x0 = tl & 0x3FFF, y0 = tl >> 16 & 0x3FFF, x1 = br & 0x3FFF, y1 = br >> 16 & 0x3FFF;
    if (!(tl >> 31)) x0 += offsetX, y0 += offsetY, x1 += offsetX, y1 += offsetY;
    uint32_t screenTl = Register(reg::kPaScScreenScissorTl), screenBr = Register(reg::kPaScScreenScissorBr);
    x0 = std::max({x0, SignExtend(screenTl & 0x7FFF, 15), 0});
    y0 = std::max({y0, SignExtend(screenTl >> 16 & 0x7FFF, 15), 0});
    x1 = std::min({x1, SignExtend(screenBr & 0x7FFF, 15), targetWidth});
    y1 = std::min({y1, SignExtend(screenBr >> 16 & 0x7FFF, 15), targetHeight});
    if (x0 >= x1 || y0 >= y1) return;
    VkRect2D scissor{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};

    if (Capturing()) {
        std::string textureList;
        for (uint32_t s = 0; s < 16; ++s) {
            if (!(textureMask >> s & 1)) continue;
            uint32_t base = Register(reg::kFetchConstants + s * 6 + 1), size = Register(reg::kFetchConstants + s * 6 + 2);
            uint32_t dimension = Register(reg::kFetchConstants + s * 6 + 5) >> 9 & 3;  // 1D, 2D, 3D, cube
            char item[96];
            std::snprintf(item, sizeof item, " t%u=%08X/fmt%u/%ux%u/%uD/slot%u", s, base & 0x1FFFF000, base & 0x3F,
                          (size & 0x1FFF) + 1, (size >> 13 & 0x1FFF) + 1, dimension + 1, slots[s].texture);
            textureList += item;
        }
        std::string inputList;  // location: format (n normalised, i integer; s signed), offset/stride in dwords
        for (const Fetch& f : fetches) {
            char item[64];
            std::snprintf(item, sizeof item, " in%u=fmt%u%s%s%+d@%d/%u", f.location, f.format, f.isInteger ? "i" : "n",
                          f.isSigned ? "s" : "", f.exponent, f.offset, f.stride);
            inputList += item;
        }
        kernel::Log("  capture: draw %016llX/%016llX prim %u count %u target %08X/%08X/%08X mode %u viewport (%.0f,%.0f "
                    "%.0fx%.0f) scissor (%d,%d)-(%d,%d) blend %08X mask %X depth %08X%s%s\n",
                    call.vertexShader->containerHash, call.pixelShader->containerHash, primitive, count, surface,
                    Register(reg::kRbColorInfo), depthInfo, mode, viewport.x, viewport.y, viewport.width,
                    viewport.height, x0, y0, x1, y1, key.blendControl, key.colorMask, key.depthControl,
                    textureList.c_str(), inputList.c_str());
    }

    // Record: only what the recording doesn't have already.
    BeginRendering(cb, color, depth);
    RecordedState& state = gRecorded;
    if (state.recording != RecordingNumber()) {
        state = {};
        state.recording = RecordingNumber();
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gPipelineLayout, 0, 4, textures::Sets(), 0, nullptr);
    }
    if (state.pipeline != pipeline) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        state.pipeline = pipeline;
    }
    uint64_t addresses[3] = {gUploadedConstants[0].address, gUploadedConstants[1].address, upload.address + sharedAt};
    vkCmdPushConstants(cb, gPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof addresses, addresses);
    if (!state.viewportSet || std::memcmp(&state.viewport, &viewport, sizeof viewport)) {
        vkCmdSetViewport(cb, 0, 1, &viewport);
        state.viewport = viewport;
        state.viewportSet = true;
    }
    if (!state.scissorSet || std::memcmp(&state.scissor, &scissor, sizeof scissor)) {
        vkCmdSetScissor(cb, 0, 1, &scissor);
        state.scissor = scissor;
        state.scissorSet = true;
    }
    uint32_t front = Register(reg::kRbStencilRefMask);
    uint32_t back = key.depthControl >> 7 & 1 ? Register(reg::kRbStencilRefMaskBf) : front;
    if (!state.stencilSet || state.stencil[0] != front || state.stencil[1] != back) {
        vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, front & 0xFF);
        vkCmdSetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, back & 0xFF);
        vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_BIT, front >> 8 & 0xFF);
        vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_BACK_BIT, back >> 8 & 0xFF);
        vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_BIT, front >> 16 & 0xFF);
        vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_BACK_BIT, back >> 16 & 0xFF);
        state.stencil[0] = front, state.stencil[1] = back;
        state.stencilSet = true;
    }
    uint32_t blendConstants[4];
    for (int i = 0; i < 4; ++i) blendConstants[i] = Register(reg::kRbBlendRed + i);
    if (!state.blendSet || std::memcmp(state.blend, blendConstants, sizeof blendConstants)) {
        float values[4];
        std::memcpy(values, blendConstants, sizeof values);
        vkCmdSetBlendConstants(cb, values);
        std::memcpy(state.blend, blendConstants, sizeof blendConstants);
        state.blendSet = true;
    }
    if (!fetches.empty()) {
        VkDeviceSize offset = upload.offset + vertexAt;
        vkCmdBindVertexBuffers(cb, 0, 1, &upload.buffer, &offset);
    }
    if (indexed) {
        vkCmdBindIndexBuffer(cb, upload.buffer, upload.offset + indexAt, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cb, uint32_t(indices.size()), 1, 0, 0, 0);
    } else {
        vkCmdDraw(cb, count, 1, 0, 0);
    }
    Operation();
    ++gDrawn;
    lap(kPhaseRecording);
}

}  // namespace s3n::render
