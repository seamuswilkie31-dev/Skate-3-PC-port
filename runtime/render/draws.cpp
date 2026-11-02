#include "render/draws.h"

#include "gpu/gpu.h"
#include "gpu/registers.h"
#include "kernel/kernel.h"
#include "memory.h"
#include "render/backend.h"
#include "render/shader_cache.h"
#include "render/vulkan.h"

#include <algorithm>
#include <array>
#include <atomic>
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
constexpr uint32_t kSharedBooleans = 256;
constexpr uint32_t kSharedHalfPixelOffset = 264;
constexpr uint32_t kSharedAlphaThreshold = 272;
constexpr uint32_t kSharedSize = 512;
constexpr uint32_t kSpecAlphaTest = 1 << 1;
constexpr uint32_t kStageConstantsSize = 256 * 16;  // one stage's float4 constants
constexpr uint32_t kTextureSlots[3] = {4096, 256, 256};  // 2D, 3D, cube
constexpr uint32_t kSamplerSlots = 256;
constexpr uint32_t kMaxVertices = 1u << 20;

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
    uint32_t address;  // the vertex fetch instruction's slot in the microcode
    uint32_t usage;
    uint32_t location;
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
};

struct PipelineKey {
    const Shader* vertexShader;
    const Shader* pixelShader;
    uint32_t topology, restart;
    uint32_t attributeCount;
    uint8_t locations[16];
    uint8_t uintInputs[16];
    VkFormat colorFormat;
    uint32_t samples;
    uint32_t depth;
    uint32_t cullMode, frontFace;
    uint32_t depthControl, blendControl, colorMask;
    uint32_t specConstants;
};
struct KeyHash {
    size_t operator()(const PipelineKey& key) const {
        uint64_t hash = 0xCBF29CE484222325ull;
        auto bytes = reinterpret_cast<const uint8_t*>(&key);
        for (size_t i = 0; i < sizeof key; ++i) hash = (hash ^ bytes[i]) * 0x100000001B3ull;
        return size_t(hash);
    }
};
struct KeyEqual {
    bool operator()(const PipelineKey& a, const PipelineKey& b) const { return std::memcmp(&a, &b, sizeof a) == 0; }
};

std::unordered_map<const Shader*, std::vector<Element>> gElements;
std::unordered_map<PipelineKey, VkPipeline, KeyHash, KeyEqual> gPipelines;

bool gSetUp = false;
bool gUnavailable = false;
VkDescriptorSetLayout gSetLayouts[4] = {};
VkDescriptorPool gDescriptorPool = VK_NULL_HANDLE;
VkDescriptorSet gSets[4] = {};
VkPipelineLayout gPipelineLayout = VK_NULL_HANDLE;
VkPipelineCache gPipelineCache = VK_NULL_HANDLE;
VkImage gDefaultImages[3] = {};
VkDeviceMemory gDefaultMemory[3] = {};
VkImageView gDefaultViews[3] = {};
VkSampler gDefaultSampler = VK_NULL_HANDLE;

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
    return out + (any ? "\n" : " none\n");
}

float RegisterFloat(uint32_t index) {
    uint32_t bits = Register(index);
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}

int32_t SignExtend(uint32_t value, uint32_t bits) { return int32_t(value << (32 - bits)) >> (32 - bits); }

// A 1x1 white image of each kind the shaders sample (2D, 3D, cube), standing in for
// every texture until textures are converted.
bool CreateDefaultImage(int kind, VkCommandBuffer cb, std::string& error) {
    Vulkan& vk = Vk();
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = kind == 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    info.flags = kind == 2 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {1, 1, 1};
    info.mipLevels = 1;
    info.arrayLayers = kind == 2 ? 6 : 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!Check(vkCreateImage(vk.device, &info, nullptr, &gDefaultImages[kind]), "vkCreateImage", error)) return false;
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(vk.device, gDefaultImages[kind], &requirements);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = requirements.size;
    for (uint32_t i = 0; i < vk.memory.memoryTypeCount; ++i) {
        if (requirements.memoryTypeBits >> i & 1) {
            alloc.memoryTypeIndex = i;
            break;
        }
    }
    if (!Check(vkAllocateMemory(vk.device, &alloc, nullptr, &gDefaultMemory[kind]), "vkAllocateMemory", error) ||
        !Check(vkBindImageMemory(vk.device, gDefaultImages[kind], gDefaultMemory[kind], 0), "vkBindImageMemory", error))
        return false;
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = gDefaultImages[kind];
    view.viewType = kind == 0 ? VK_IMAGE_VIEW_TYPE_2D : kind == 1 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_CUBE;
    view.format = info.format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, info.arrayLayers};
    if (!Check(vkCreateImageView(vk.device, &view, nullptr, &gDefaultViews[kind]), "vkCreateImageView", error))
        return false;
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = gDefaultImages[kind];
    barrier.subresourceRange = view.subresourceRange;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
    VkClearColorValue white{{1.0f, 1.0f, 1.0f, 1.0f}};
    vkCmdClearColorImage(cb, gDefaultImages[kind], VK_IMAGE_LAYOUT_GENERAL, &white, 1, &view.subresourceRange);
    return true;
}

// The bindless descriptor sets the shaders index (sets 0-2: 2D, 3D and cube textures; set
// 3: samplers), the pipeline layout, and the stand-in white images in slot 0.
bool SetUp(VkCommandBuffer cb) {
    if (gSetUp) return true;
    if (gUnavailable) return false;
    gUnavailable = true;  // until it works
    Vulkan& vk = Vk();
    std::string error;
    EndRendering();
    VkDescriptorBindingFlags bindingFlags =
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    for (int i = 0; i < 4; ++i) {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
        binding.descriptorCount = i < 3 ? kTextureSlots[i] : kSamplerSlots;
        binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo flags{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        flags.bindingCount = 1;
        flags.pBindingFlags = &bindingFlags;
        VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layout.pNext = &flags;
        layout.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        layout.bindingCount = 1;
        layout.pBindings = &binding;
        if (!Check(vkCreateDescriptorSetLayout(vk.device, &layout, nullptr, &gSetLayouts[i]),
                   "vkCreateDescriptorSetLayout", error))
            break;
    }
    VkDescriptorPoolSize sizes[2] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kTextureSlots[0] + kTextureSlots[1] + kTextureSlots[2]},
        {VK_DESCRIPTOR_TYPE_SAMPLER, kSamplerSlots}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pool.maxSets = 4;
    pool.poolSizeCount = 2;
    pool.pPoolSizes = sizes;
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorSetCount = 4;
    alloc.pSetLayouts = gSetLayouts;
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 3 * sizeof(uint64_t)};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 4;
    layout.pSetLayouts = gSetLayouts;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    VkPipelineCacheCreateInfo cache{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.maxLod = VK_LOD_CLAMP_NONE;
    bool ok = gSetLayouts[3] &&
              Check(vkCreateDescriptorPool(vk.device, &pool, nullptr, &gDescriptorPool), "vkCreateDescriptorPool",
                    error);
    alloc.descriptorPool = gDescriptorPool;
    ok = ok && Check(vkAllocateDescriptorSets(vk.device, &alloc, gSets), "vkAllocateDescriptorSets", error) &&
              Check(vkCreatePipelineLayout(vk.device, &layout, nullptr, &gPipelineLayout), "vkCreatePipelineLayout",
                    error) &&
              Check(vkCreatePipelineCache(vk.device, &cache, nullptr, &gPipelineCache), "vkCreatePipelineCache",
                    error) &&
              Check(vkCreateSampler(vk.device, &sampler, nullptr, &gDefaultSampler), "vkCreateSampler", error);
    for (int kind = 0; ok && kind < 3; ++kind) ok = CreateDefaultImage(kind, cb, error);
    if (!ok) {
        kernel::Log("  draws: can't set up: %s\n", error.c_str());
        return false;
    }
    FullBarrier(cb);

    VkDescriptorImageInfo images[3], samplerInfo{gDefaultSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    VkWriteDescriptorSet writes[4]{};
    for (int i = 0; i < 4; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = gSets[i];
        writes[i].descriptorCount = 1;
        if (i < 3) {
            images[i] = {VK_NULL_HANDLE, gDefaultViews[i], VK_IMAGE_LAYOUT_GENERAL};
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            writes[i].pImageInfo = &images[i];
        } else {
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            writes[i].pImageInfo = &samplerInfo;
        }
    }
    vkUpdateDescriptorSets(vk.device, 4, writes, 0, nullptr);
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
                    elements.push_back({value & 0xFFF, usage, u.location});
                    break;
                }
            }
        }
    }
    std::sort(elements.begin(), elements.end(), [](auto& a, auto& b) { return a.address < b.address; });
    return gElements.emplace(shader, std::move(elements)).first->second;
}

// The fetches as bound: each element's vertex fetch instruction in the microcode the GPU
// was given. A mini fetch reuses the buffer and stride of the full fetch before it.
void BoundFetches(const DrawCall& call, std::vector<Fetch>& fetches) {
    fetches.clear();
    uint32_t constant = 0, stride = 0;
    for (const Element& e : VertexElements(call.vertexShader)) {
        Fetch f{};
        f.location = e.location;
        f.usage = e.usage;
        if (uint64_t(e.address) * 12 + 12 <= call.vertexMicrocodeSize) {
            uint32_t w0 = LoadBE32(call.vertexMicrocode, e.address * 12);
            uint32_t w1 = LoadBE32(call.vertexMicrocode, e.address * 12 + 4);
            uint32_t w2 = LoadBE32(call.vertexMicrocode, e.address * 12 + 8);
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

uint32_t FormatWords(uint32_t format) {
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

float Component(uint32_t raw, uint32_t bits, bool isSigned, bool isInteger) {
    if (isSigned) {
        int32_t value = SignExtend(raw, bits);
        return isInteger ? float(value) : std::max(float(value) / float((1u << (bits - 1)) - 1), -1.0f);
    }
    if (isInteger) return float(raw);
    return float(raw) / float(bits == 32 ? 4294967295.0 : double((1ull << bits) - 1));
}

float Float(uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}

// One fetched element as float4 (missing components 0, 0, 0, 1). Components are packed
// from the low bits of each (endian-swapped) dword.
void Unpack(const Fetch& f, const uint32_t* w, float v[4]) {
    auto c = [&](uint32_t raw, uint32_t bits) { return Component(raw, bits, f.isSigned, f.isInteger); };
    switch (f.format) {
    case 6:  // 8_8_8_8
        for (int i = 0; i < 4; ++i) v[i] = c(w[0] >> (8 * i) & 0xFF, 8);
        break;
    case 7:  // 2_10_10_10
        v[0] = c(w[0] & 0x3FF, 10), v[1] = c(w[0] >> 10 & 0x3FF, 10), v[2] = c(w[0] >> 20 & 0x3FF, 10);
        v[3] = c(w[0] >> 30, 2);
        break;
    case 16:  // 10_11_11
        v[0] = c(w[0] & 0x7FF, 11), v[1] = c(w[0] >> 11 & 0x7FF, 11), v[2] = c(w[0] >> 22, 10);
        break;
    case 17:  // 11_11_10
        v[0] = c(w[0] & 0x3FF, 10), v[1] = c(w[0] >> 10 & 0x7FF, 11), v[2] = c(w[0] >> 21, 11);
        break;
    case 25:  // 16_16
        v[0] = c(w[0] & 0xFFFF, 16), v[1] = c(w[0] >> 16, 16);
        break;
    case 26:  // 16_16_16_16
        v[0] = c(w[0] & 0xFFFF, 16), v[1] = c(w[0] >> 16, 16), v[2] = c(w[1] & 0xFFFF, 16), v[3] = c(w[1] >> 16, 16);
        break;
    case 31:  // 16_16 float
        v[0] = Half(w[0] & 0xFFFF), v[1] = Half(w[0] >> 16);
        break;
    case 32:  // 16_16_16_16 float
        v[0] = Half(w[0] & 0xFFFF), v[1] = Half(w[0] >> 16), v[2] = Half(w[1] & 0xFFFF), v[3] = Half(w[1] >> 16);
        break;
    case 33: case 34: case 35:  // 32, 32_32, 32_32_32_32
        for (uint32_t i = 0; i < FormatWords(f.format); ++i) v[i] = c(w[i], 32);
        break;
    case 36: case 37: case 38: case 57:  // floats
        for (uint32_t i = 0; i < FormatWords(f.format); ++i) v[i] = Float(w[i]);
        break;
    }
    if (f.exponent && f.format != 31 && f.format != 32 && f.format < 36)
        for (int i = 0; i < 4; ++i) v[i] = std::ldexp(v[i], f.exponent);
}

// Vertices [first, first + count) of every fetch, 16 bytes each, interleaved.
void ConvertVertices(const std::vector<Fetch>& fetches, uint32_t first, uint32_t count, uint8_t* out) {
    uint32_t stride = uint32_t(fetches.size()) * 16;
    for (size_t e = 0; e < fetches.size(); ++e) {
        const Fetch& f = fetches[e];
        uint32_t words = FormatWords(f.format);
        for (uint32_t v = 0; v < count; ++v) {
            float value[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            int64_t address = int64_t(f.base) + (int64_t(first + v) * f.stride + f.offset) * 4;
            if (f.valid && words && address >= f.base && address + words * 4 <= f.end) {
                const uint8_t* data = gpu::PhysicalMemory(uint32_t(address));
                uint32_t w[4];
                for (uint32_t i = 0; i < words; ++i) {
                    std::memcpy(&w[i], data + i * 4, 4);
                    w[i] = Swap(w[i], f.endian);
                }
                Unpack(f, w, value);
            }
            uint8_t* destination = out + size_t(v) * stride + e * 16;
            if (f.usage == kUsageBlendIndices) {
                uint32_t indices[4];
                for (int i = 0; i < 4; ++i) indices[i] = uint32_t(std::max(value[i], 0.0f));
                std::memcpy(destination, indices, 16);
            } else {
                std::memcpy(destination, value, 16);
            }
        }
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
        const uint8_t* data = gpu::PhysicalMemory(call.indexBase & (wide ? ~3u : ~1u));
        first = UINT32_MAX, last = 0;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t word;
            std::memcpy(&word, data + (wide ? i * 4 : i / 2 * 4), 4);
            word = Swap(word, endian);
            uint32_t index = wide ? word : (i & 1 ? word >> 16 : word & 0xFFFF);
            if (restart && index == resetIndex) {
                indices.push_back(UINT32_MAX);
                continue;
            }
            index += indexOffset;
            first = std::min(first, index), last = std::max(last, index);
            indices.push_back(index);
        }
        if (first > last) return;  // nothing but restarts
        for (uint32_t& index : indices)
            if (index != UINT32_MAX) index -= first;
    } else if (primitive == 13) {
        for (uint32_t i = 0; i < count; ++i) indices.push_back(i);
    }
    if (primitive == 13) {  // quads (a, b, c, d) as triangles (a, b, c) and (a, c, d)
        std::vector<uint32_t> quads;
        quads.swap(indices);
        for (size_t q = 0; q + 3 < quads.size(); q += 4)
            for (size_t i : {0, 1, 2, 0, 2, 3}) indices.push_back(quads[q + i]);
        if (indices.empty()) return;
    }
    uint32_t vertexCount = last - first + 1;
    if (vertexCount > kMaxVertices) {
        ++gSkipped[kTooBig];
        return;
    }

    static thread_local std::vector<Fetch> fetches;
    BoundFetches(call, fetches);
    if (fetches.size() > 16) fetches.resize(16);
    uint32_t vertexStride = uint32_t(fetches.size()) * 16;

    // Everything the draw reads, in one block of the upload ring: both stages' constants,
    // the shared constants, the vertices and the indices.
    size_t vertexAt = Align(2 * kStageConstantsSize + kSharedSize);
    size_t indexAt = Align(vertexAt + size_t(vertexCount) * vertexStride);
    size_t total = indexAt + indices.size() * 4;
    Upload upload;
    VkCommandBuffer cb = Allocate(total, upload);
    if (!cb || !SetUp(cb)) {
        ++gSkipped[kNoWindow];
        return;
    }
    const uint32_t* registers = gpu::ConstantRegisters();
    std::memcpy(upload.data, registers + reg::kAluConstants, kStageConstantsSize);
    std::memcpy(upload.data + kStageConstantsSize, registers + reg::kPixelAluConstants, kStageConstantsSize);
    uint8_t* shared = upload.data + 2 * kStageConstantsSize;
    std::memset(shared, 0, kSharedSize);  // every texture and sampler: slot 0
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
    ConvertVertices(fetches, first, vertexCount, upload.data + vertexAt);
    if (!indices.empty()) std::memcpy(upload.data + indexAt, indices.data(), indices.size() * 4);

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

    // Viewport and scissor, moved by the window offset (each tile of a tiled screen).
    uint32_t windowOffset = Register(reg::kPaScWindowOffset);
    int32_t offsetX = SignExtend(windowOffset & 0x7FFF, 15), offsetY = SignExtend(windowOffset >> 16 & 0x7FFF, 15);
    float vx = modeControl >> 16 & 1 ? float(offsetX) : 0.0f, vy = modeControl >> 16 & 1 ? float(offsetY) : 0.0f;
    VkViewport viewport{scale[1] - scale[0] + vx,
                        scale[3] - scale[2] + vy,
                        2.0f * scale[0],
                        2.0f * scale[2],
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

    // Record.
    BeginRendering(cb, color, depth);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gPipelineLayout, 0, 4, gSets, 0, nullptr);
    uint64_t addresses[3] = {upload.address, upload.address + kStageConstantsSize,
                             upload.address + 2 * kStageConstantsSize};
    vkCmdPushConstants(cb, gPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof addresses, addresses);
    vkCmdSetViewport(cb, 0, 1, &viewport);
    vkCmdSetScissor(cb, 0, 1, &scissor);
    uint32_t front = Register(reg::kRbStencilRefMask);
    uint32_t back = key.depthControl >> 7 & 1 ? Register(reg::kRbStencilRefMaskBf) : front;
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, front & 0xFF);
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, back & 0xFF);
    vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_BIT, front >> 8 & 0xFF);
    vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_BACK_BIT, back >> 8 & 0xFF);
    vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_BIT, front >> 16 & 0xFF);
    vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_BACK_BIT, back >> 16 & 0xFF);
    float blendConstants[4];
    for (int i = 0; i < 4; ++i) blendConstants[i] = RegisterFloat(reg::kRbBlendRed + i);
    vkCmdSetBlendConstants(cb, blendConstants);
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
}

}  // namespace s3n::render
