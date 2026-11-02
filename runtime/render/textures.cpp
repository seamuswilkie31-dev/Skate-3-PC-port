#include "render/textures.h"

#include "gpu/gpu.h"
#include "gpu/registers.h"
#include "kernel/kernel.h"
#include "render/backend.h"
#include "render/vulkan.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>

namespace s3n::render::textures {
namespace {

using gpu::Register;
namespace reg = gpu::reg;
using namespace backend;

constexpr uint32_t kSlots[4] = {4096, 256, 256, 256};  // 2D, 3D and cube textures; samplers

// A texture format as Vulkan stores it: its block size (4 for the compressed formats),
// bytes per block, and which Vulkan component holds each of the GPU's x, y, z and w.
struct FormatInfo {
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t block = 1, bytes = 4;
    uint8_t channel[4] = {0, 1, 2, 3};
};

FormatInfo Format(uint32_t format) {
    switch (format) {
    case 2: case 8: case 9: return {VK_FORMAT_R8_UNORM, 1, 1};                          // 8, 8_A, 8_B
    case 3: return {VK_FORMAT_A1R5G5B5_UNORM_PACK16, 1, 2, {2, 1, 0, 3}};                // 1_5_5_5
    case 4: return {VK_FORMAT_B5G6R5_UNORM_PACK16, 1, 2};                                // 5_6_5
    case 6: case 50: return {VK_FORMAT_R8G8B8A8_UNORM, 1, 4};                            // 8_8_8_8
    case 7: case 54: return {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 1, 4};                  // 2_10_10_10
    case 10: return {VK_FORMAT_R8G8_UNORM, 1, 2};                                        // 8_8
    case 18: case 51: return {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 4, 8};                     // DXT1
    case 19: case 52: return {VK_FORMAT_BC2_UNORM_BLOCK, 4, 16};                         // DXT2_3
    case 20: case 53: return {VK_FORMAT_BC3_UNORM_BLOCK, 4, 16};                         // DXT4_5
    case 24: return {VK_FORMAT_R16_UNORM, 1, 2};                                         // 16
    case 25: return {VK_FORMAT_R16G16_UNORM, 1, 4};                                      // 16_16
    case 26: return {VK_FORMAT_R16G16B16A16_UNORM, 1, 8};                                // 16_16_16_16
    case 30: return {VK_FORMAT_R16_SFLOAT, 1, 2};                                        // 16 float
    case 31: return {VK_FORMAT_R16G16_SFLOAT, 1, 4};                                     // 16_16 float
    case 32: return {VK_FORMAT_R16G16B16A16_SFLOAT, 1, 8};                               // 16_16_16_16 float
    case 36: return {VK_FORMAT_R32_SFLOAT, 1, 4};                                        // 32 float
    case 37: return {VK_FORMAT_R32G32_SFLOAT, 1, 8};                                     // 32_32 float
    case 38: return {VK_FORMAT_R32G32B32A32_SFLOAT, 1, 16};                              // 32_32_32_32 float
    case 49: return {VK_FORMAT_BC5_UNORM_BLOCK, 4, 16};                                  // DXN
    case 59: return {VK_FORMAT_BC4_UNORM_BLOCK, 4, 8};                                   // DXT5A
    default: return {};
    }
}

// A texture from guest memory, as its fetch constant describes it.
struct Texture {
    Image image;
    uint64_t hash = 0;          // of its memory when last converted
    uint64_t checkedFrame = ~0ull;
};

// A view of an image with a fetch constant's swizzle, in a slot of the 2D array.
struct View {
    VkImageView view = VK_NULL_HANDLE;
    uint32_t slot = 0;
};

bool gSetUp = false;
bool gUnavailable = false;
VkDescriptorSetLayout gSetLayouts[4] = {};
VkDescriptorPool gPool = VK_NULL_HANDLE;
VkDescriptorSet gSets[4] = {};
VkImage gDefaultImages[3] = {};
VkDeviceMemory gDefaultMemory[3] = {};
VkImageView gDefaultViews[3] = {};
VkSampler gDefaultSampler = VK_NULL_HANDLE;
uint32_t gNextSlot[4] = {1, 1, 1, 1};

std::unordered_map<uint64_t, Texture> gTextures;  // by fetch constant fields
std::map<std::tuple<VkImage, uint64_t, uint32_t>, View> gViews;  // (image, serial, swizzle)
std::unordered_map<uint32_t, uint32_t> gSamplers;  // sampler state -> slot

std::atomic<uint64_t> gConverted{0}, gReconverted{0}, gResolvedUses{0}, gUnsupported{0}, gOutOfSlots{0};
uint32_t gUnsupportedFormats = 0;  // bit per format seen unsupported (first 32)

std::string Describe() {
    char line[256];
    std::snprintf(line, sizeof line,
                  "textures: %zu from memory (%llu conversions, %llu again after changes), %llu uses of resolved "
                  "render targets, %u sampler states; %llu fetches of unsupported formats, %llu out of slots\n",
                  gTextures.size(), gConverted.load(), gReconverted.load(), gResolvedUses.load(),
                  uint32_t(gSamplers.size()), gUnsupported.load(), gOutOfSlots.load());
    std::string out = line;
    if (gUnsupportedFormats) {
        out += "  unsupported formats:";
        for (uint32_t f = 0; f < 32; ++f)
            if (gUnsupportedFormats >> f & 1) out += " " + std::to_string(f);
        out += "\n";
    }
    return out;
}

void WriteDescriptor(uint32_t set, uint32_t slot, VkImageView view, VkSampler sampler) {
    VkDescriptorImageInfo info{sampler, view, view ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = gSets[set];
    write.dstArrayElement = slot;
    write.descriptorCount = 1;
    write.descriptorType = set < 3 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(Vk().device, 1, &write, 0, nullptr);
}

// A 1x1 white image of each kind (2D, 3D, cube) for slot 0.
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

// The GPU's 2D tiling: 32x32-block macro tiles, each split between two banks and four
// pipes, as the Xbox 360's documented address function lays them out. Returns the byte
// offset of block (x, y) for a pitch (in blocks) already aligned to 32.
uint32_t Tiled2D(uint32_t x, uint32_t y, uint32_t pitch, uint32_t log2Bytes) {
    uint32_t outer = ((y >> 5) * (pitch >> 5) + (x >> 5)) << 6;
    uint32_t inner = ((y >> 1) & 7) << 3 | (x & 7);
    uint32_t bytes = (outer | inner) << log2Bytes;
    uint32_t bank = y >> 4 & 1, pipe = (x >> 3 & 3) ^ ((y >> 3 & 1) << 1);
    return (y & 1) << 4 | pipe << 6 | bank << 11 | (bytes & 15) | (bytes >> 4 & 1) << 5 | (bytes >> 5 & 7) << 8 |
           (bytes >> 8) << 12;
}

uint32_t Log2(uint32_t value) {
    uint32_t log = 0;
    while ((1u << log) < value) ++log;
    return log;
}

// Converts the base level into `staging`: blocks untiled (or copied row by row) and the
// fetch constant's endian swap undone. The swaps reverse bytes within 16 bits (8in16), 32
// bits (8in32), or swap 16-bit halves (16in32): as an address, an exclusive-or of 1, 3, 2.
void Convert(const uint8_t* source, uint8_t* staging, uint32_t blocksWide, uint32_t blocksHigh, uint32_t pitchBlocks,
             bool tiled, uint32_t bytes, uint32_t endian) {
    static constexpr uint32_t kSwap[4] = {0, 1, 3, 2};
    uint32_t swap = kSwap[endian & 3], log2Bytes = Log2(bytes);
    uint32_t pitchAligned = (pitchBlocks + 31) & ~31u;
    for (uint32_t y = 0; y < blocksHigh; ++y) {
        uint8_t* out = staging + size_t(y) * blocksWide * bytes;
        for (uint32_t x = 0; x < blocksWide; ++x, out += bytes) {
            uint32_t at = tiled ? Tiled2D(x, y, pitchAligned, log2Bytes) : (y * pitchBlocks + x) * bytes;
            if (swap == 3 && bytes % 4 == 0) {
                for (uint32_t i = 0; i < bytes; i += 4) {
                    uint32_t word;
                    std::memcpy(&word, source + at + i, 4);
                    word = __builtin_bswap32(word);
                    std::memcpy(out + i, &word, 4);
                }
            } else {
                for (uint32_t i = 0; i < bytes; ++i) out[i] = source[(at + i) ^ swap];
            }
        }
    }
}

VkComponentSwizzle Component(uint32_t select, const uint8_t channel[4]) {
    static constexpr VkComponentSwizzle kComponents[4] = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G,
                                                          VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
    if (select < 4) return kComponents[channel[select]];
    return select == 4 ? VK_COMPONENT_SWIZZLE_ZERO : VK_COMPONENT_SWIZZLE_ONE;
}

// The 2D slot of `image` seen through a fetch constant's swizzle (3 bits per output
// component: x, y, z, w, then 0 and 1).
uint32_t ViewSlot(const Image& image, uint64_t serial, uint32_t swizzle, const uint8_t channel[4]) {
    auto key = std::make_tuple(image.image, serial, swizzle);
    auto it = gViews.find(key);
    if (it != gViews.end()) return it->second.slot;
    if (gNextSlot[0] >= kSlots[0]) {
        ++gOutOfSlots;
        return 0;
    }
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = image.image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = image.format;
    info.components = {Component(swizzle & 7, channel), Component(swizzle >> 3 & 7, channel),
                       Component(swizzle >> 6 & 7, channel), Component(swizzle >> 9 & 7, channel)};
    info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    View view;
    if (vkCreateImageView(Vk().device, &info, nullptr, &view.view) != VK_SUCCESS) return 0;
    view.slot = gNextSlot[0]++;
    WriteDescriptor(0, view.slot, view.view, VK_NULL_HANDLE);
    gViews.emplace(key, view);
    return view.slot;
}

// The sampler for a fetch constant's clamps, filters and border colour.
uint32_t SamplerSlot(const uint32_t fetch[6]) {
    uint32_t key = (fetch[0] >> 10 & 0x1FF) | (fetch[3] >> 19 & 0x3F) << 9 | (fetch[5] & 3) << 15;
    auto it = gSamplers.find(key);
    if (it != gSamplers.end()) return it->second;
    if (gNextSlot[3] >= kSlots[3]) {
        ++gOutOfSlots;
        return 0;
    }
    static constexpr VkSamplerAddressMode kClamp[8] = {
        VK_SAMPLER_ADDRESS_MODE_REPEAT,          VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,   VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,   VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER};
    static constexpr VkBorderColor kBorder[4] = {VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
                                                 VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
                                                 VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
                                                 VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK};
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = (fetch[3] >> 19 & 3) == 0 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    info.minFilter = (fetch[3] >> 21 & 3) == 0 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    info.mipmapMode = (fetch[3] >> 23 & 3) == 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = kClamp[fetch[0] >> 10 & 7];
    info.addressModeV = kClamp[fetch[0] >> 13 & 7];
    info.addressModeW = kClamp[fetch[0] >> 16 & 7];
    info.borderColor = kBorder[fetch[5] & 3];
    info.maxLod = 0.0f;  // only the base level is converted so far
    VkSampler sampler = VK_NULL_HANDLE;
    if (vkCreateSampler(Vk().device, &info, nullptr, &sampler) != VK_SUCCESS) return 0;
    uint32_t slot = gNextSlot[3]++;
    WriteDescriptor(3, slot, VK_NULL_HANDLE, sampler);
    gSamplers.emplace(key, slot);
    return slot;
}

}  // namespace

bool SetUp(VkCommandBuffer cb) {
    if (gSetUp) return true;
    if (gUnavailable) return false;
    gUnavailable = true;  // until it works
    Vulkan& vk = Vk();
    std::string error;
    EndRendering();
    VkDescriptorBindingFlags bindingFlags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                            VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    bool ok = true;
    for (int i = 0; ok && i < 4; ++i) {
        VkDescriptorSetLayoutBinding binding{};
        binding.descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
        binding.descriptorCount = kSlots[i];
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
        ok = Check(vkCreateDescriptorSetLayout(vk.device, &layout, nullptr, &gSetLayouts[i]),
                   "vkCreateDescriptorSetLayout", error);
    }
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSlots[0] + kSlots[1] + kSlots[2]},
                                     {VK_DESCRIPTOR_TYPE_SAMPLER, kSlots[3]}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pool.maxSets = 4;
    pool.poolSizeCount = 2;
    pool.pPoolSizes = sizes;
    ok = ok && Check(vkCreateDescriptorPool(vk.device, &pool, nullptr, &gPool), "vkCreateDescriptorPool", error);
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = gPool;
    alloc.descriptorSetCount = 4;
    alloc.pSetLayouts = gSetLayouts;
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    ok = ok && Check(vkAllocateDescriptorSets(vk.device, &alloc, gSets), "vkAllocateDescriptorSets", error) &&
         Check(vkCreateSampler(vk.device, &sampler, nullptr, &gDefaultSampler), "vkCreateSampler", error);
    for (int kind = 0; ok && kind < 3; ++kind) ok = CreateDefaultImage(kind, cb, error);
    if (!ok) {
        kernel::Log("  textures: can't set up: %s\n", error.c_str());
        return false;
    }
    FullBarrier(cb);
    for (uint32_t kind = 0; kind < 3; ++kind) WriteDescriptor(kind, 0, gDefaultViews[kind], VK_NULL_HANDLE);
    WriteDescriptor(3, 0, VK_NULL_HANDLE, gDefaultSampler);
    kernel::AddReportSection(Describe);
    gUnavailable = false;
    gSetUp = true;
    return true;
}

const VkDescriptorSetLayout* SetLayouts() { return gSetLayouts; }
const VkDescriptorSet* Sets() { return gSets; }

Slots Bind(uint32_t index) {
    Slots slots;
    if (!gSetUp || index >= 32) return slots;
    uint32_t fetch[6];
    for (uint32_t i = 0; i < 6; ++i) fetch[i] = Register(reg::kFetchConstants + index * 6 + i);
    if ((fetch[0] & 3) != 2) return slots;  // not a texture
    slots.sampler = SamplerSlot(fetch);
    uint32_t dimension = fetch[5] >> 9 & 3;  // 1D, 2D (or stacked), 3D, cube
    if (dimension != 1 || (fetch[1] >> 10 & 1)) {
        slots.kind = dimension == 2 ? 1 : dimension == 3 ? 2 : 0;  // white until those are converted
        return slots;
    }
    uint32_t base = fetch[1] & 0x1FFFF000, format = fetch[1] & 0x3F, endian = fetch[1] >> 6 & 3;
    uint32_t width = (fetch[2] & 0x1FFF) + 1, height = (fetch[2] >> 13 & 0x1FFF) + 1;
    uint32_t pitch = (fetch[0] >> 22 & 0x1FF) * 32;
    bool tiled = fetch[0] >> 31;
    uint32_t swizzle = fetch[3] >> 1 & 0xFFF;

    // A render target a resolve copied out here: sampled directly.
    uint64_t serial = 0;
    if (Image* resolved = ResolvedImage(base, serial)) {
        static constexpr uint8_t kIdentity[4] = {0, 1, 2, 3};
        slots.texture = ViewSlot(*resolved, serial, swizzle, kIdentity);
        ++gResolvedUses;
        return slots;
    }

    FormatInfo info = Format(format);
    if (!info.format) {
        ++gUnsupported;
        if (format < 32) gUnsupportedFormats |= 1u << format;
        return slots;
    }
    uint32_t blocksWide = (width + info.block - 1) / info.block, blocksHigh = (height + info.block - 1) / info.block;
    uint32_t pitchBlocks = std::max(pitch / info.block, blocksWide);
    size_t span = tiled ? size_t((pitchBlocks + 31) & ~31u) * ((blocksHigh + 31) & ~31u) * info.bytes
                        : size_t(pitchBlocks) * blocksHigh * info.bytes;
    if (uint64_t(base) + span > 0x20000000) return slots;

    uint64_t key = uint64_t(base) << 32 ^ uint64_t(fetch[1] & 0xFF) << 24 ^ uint64_t(fetch[2]) * 0x9E3779B1ull ^
                   uint64_t(pitch) << 8 ^ uint64_t(tiled);
    auto [it, created] = gTextures.try_emplace(key);
    Texture& texture = it->second;
    const uint8_t* source = gpu::PhysicalMemory(base);
    uint64_t frame = FrameNumber();
    if (created || texture.checkedFrame != frame) {
        texture.checkedFrame = frame;
        uint64_t hash = HashMemory(source, span);
        if (created || hash != texture.hash) {
            size_t size = size_t(blocksWide) * blocksHigh * info.bytes;
            Upload upload;
            VkCommandBuffer cb = Allocate(size, upload);
            if (!cb) return slots;
            EndRendering();
            Convert(source, upload.data, blocksWide, blocksHigh, pitchBlocks, tiled, info.bytes, endian);
            std::string error;
            if (created && !CreateImage(texture.image, info.format, width, height, VK_SAMPLE_COUNT_1_BIT,
                                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, cb, error)) {
                kernel::Log("  textures: can't make a %ux%u texture: %s\n", width, height, error.c_str());
                DestroyImage(texture.image);
                gTextures.erase(it);
                return slots;
            }
            VkBufferImageCopy region{};
            region.bufferOffset = upload.offset;
            region.bufferRowLength = blocksWide * info.block;
            region.bufferImageHeight = blocksHigh * info.block;
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {width, height, 1};
            vkCmdCopyBufferToImage(cb, upload.buffer, texture.image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
            FullBarrier(cb);
            ++(created ? gConverted : gReconverted);
            texture.hash = hash;
        }
    }
    slots.texture = ViewSlot(texture.image, 0, swizzle, info.channel);
    return slots;
}

}  // namespace s3n::render::textures
