#include "render/targets.h"

#include "gpu/gpu.h"
#include "gpu/registers.h"
#include "kernel/kernel.h"
#include "memory.h"
#include "render/presenter.h"
#include "render/vulkan.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

namespace s3n::render {
namespace {

using gpu::Register;
namespace reg = gpu::reg;

constexpr uint32_t kEdramTiles = 2048;
constexpr uint32_t kMaxTargetHeight = 2048;
constexpr uint32_t kMaxOperations = 4096;  // per recording, before it's submitted without a swap

// RB_COLOR_INFO's formats as the closest Vulkan formats. The GPU's 16-bit fixed-point
// formats (-32 to 32) and its 7e3 floats don't exist on PC; half floats stand in for now.
VkFormat ColorFormat(uint32_t format) {
    switch (format) {
    case 0:  // 8_8_8_8
    case 1:  // 8_8_8_8, gamma
        return VK_FORMAT_R8G8B8A8_UNORM;
    case 2:   // 2_10_10_10
    case 10:  // 2_10_10_10 as 10_10_10_10
        return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 4:  // 16_16
    case 6:  // 16_16 float
        return VK_FORMAT_R16G16_SFLOAT;
    case 14:  // 32 float
        return VK_FORMAT_R32_SFLOAT;
    case 15:  // 32_32 float
        return VK_FORMAT_R32G32_SFLOAT;
    default:  // 2_10_10_10 float (7e3) and the 16_16_16_16 formats
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    }
}

// 64-bit formats take twice the EDRAM per sample.
bool Is64Bit(uint32_t format) { return format == 5 || format == 7 || format == 12 || format == 15; }

struct Target {
    uint32_t base, pitch, msaa, format;
    bool depth;
    Image image;
};

// A resolve's destination, by its physical address.
struct Texture {
    uint32_t address;
    uint32_t format;  // RB_COPY_DEST_INFO's
    Image image;
};

struct Retired {
    Image image;
    uint64_t after;  // freed once the timeline passes this
};

struct Rect {
    int32_t x0, y0, x1, y1;
};

// Containers: the command processor's; the mutex is for the report.
std::mutex gMutex;
std::deque<Target> gTargets;
std::deque<Texture> gTextures;
std::vector<Retired> gRetired;

bool gSetUp = false;
bool gUnavailable = false;
VkCommandPool gPool = VK_NULL_HANDLE;
VkCommandBuffer gCommands[2] = {};
uint64_t gSubmittedAt[2] = {};  // timeline value of each command buffer's last submission
uint32_t gCurrent = 0;
bool gRecording = false;
uint32_t gOperations = 0;  // in the current recording
VkSemaphore gTimeline = VK_NULL_HANDLE;
uint64_t gTimelineValue = 0;

uint32_t gDumpInterval = 0;
std::string gDumpFolder;

std::atomic<uint64_t> gClears{0};
std::atomic<uint64_t> gResolves{0};
std::atomic<uint64_t> gResolveClears{0};
std::atomic<uint64_t> gSkippedResolves{0};
std::atomic<uint64_t> gFrames{0};
std::atomic<uint64_t> gFramesWithFront{0};

const char* const kMsaa[4] = {"1x", "2x", "4x", "?"};

std::string Describe() {
    char line[192];
    std::string out;
    std::unique_lock lock(gMutex, std::try_to_lock);
    std::snprintf(line, sizeof line,
                  "targets: %zu render targets, %zu resolve textures; %llu clears, %llu resolves (%llu skipped, "
                  "%llu clears after); %llu frames, %llu showing a front buffer\n",
                  lock.owns_lock() ? gTargets.size() : size_t(0), lock.owns_lock() ? gTextures.size() : size_t(0),
                  gClears.load(), gResolves.load(), gSkippedResolves.load(), gResolveClears.load(), gFrames.load(),
                  gFramesWithFront.load());
    out += line;
    if (lock.owns_lock()) {
        for (const Target& t : gTargets) {
            std::snprintf(line, sizeof line, "  %s target: base %u, pitch %u, %s, format %u -> %ux%u\n",
                          t.depth ? "depth" : "colour", t.base, t.pitch, kMsaa[t.msaa & 3], t.format, t.image.width,
                          t.image.height);
            out += line;
        }
    }
    return out;
}

bool SetUp() {
    if (gSetUp) return true;
    if (gUnavailable) return false;
    Vulkan& vk = Vk();
    gUnavailable = true;  // until it works
    if (!vk.device) return false;
    std::string error;
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = vk.queueFamily;
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 2;
    VkSemaphoreTypeCreateInfo timelineType{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    timelineType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo timeline{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    timeline.pNext = &timelineType;
    bool ok = Check(vkCreateCommandPool(vk.device, &pool, nullptr, &gPool), "vkCreateCommandPool", error);
    alloc.commandPool = gPool;
    ok = ok && Check(vkAllocateCommandBuffers(vk.device, &alloc, gCommands), "vkAllocateCommandBuffers", error) &&
         Check(vkCreateSemaphore(vk.device, &timeline, nullptr, &gTimeline), "vkCreateSemaphore", error);
    if (!ok) {
        kernel::Log("  targets: %s\n", error.c_str());
        return false;
    }
    kernel::AddReportSection(Describe);
    gUnavailable = false;
    gSetUp = true;
    return true;
}

void WaitFor(uint64_t value) {
    VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wait.semaphoreCount = 1;
    wait.pSemaphores = &gTimeline;
    wait.pValues = &value;
    vkWaitSemaphores(Vk().device, &wait, UINT64_MAX);
}

uint64_t Submit();

// The command buffer being recorded, started if need be. VK_NULL_HANDLE without a window.
// A long recording goes to the GPU here, between operations, without waiting for a swap.
VkCommandBuffer Commands() {
    if (!SetUp()) return VK_NULL_HANDLE;
    if (gRecording && gOperations >= kMaxOperations) Submit();
    if (!gRecording) {
        if (gSubmittedAt[gCurrent]) WaitFor(gSubmittedAt[gCurrent]);
        VkCommandBuffer cb = gCommands[gCurrent];
        vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &begin);
        gRecording = true;
        gOperations = 0;
    }
    return gCommands[gCurrent];
}

// Submits the recording. Returns the timeline value it signals when done.
uint64_t Submit() {
    VkCommandBuffer cb = gCommands[gCurrent];
    vkEndCommandBuffer(cb);
    uint64_t value = ++gTimelineValue;
    VkTimelineSemaphoreSubmitInfo values{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    values.signalSemaphoreValueCount = 1;
    values.pSignalSemaphoreValues = &value;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.pNext = &values;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &gTimeline;
    {
        std::lock_guard lock(Vk().queueMutex);
        vkQueueSubmit(Vk().queue, 1, &submit, VK_NULL_HANDLE);
    }
    gSubmittedAt[gCurrent] = value;
    gCurrent ^= 1;
    gRecording = false;

    // Free the images the GPU has finished with.
    uint64_t done = 0;
    vkGetSemaphoreCounterValue(Vk().device, gTimeline, &done);
    std::erase_if(gRetired, [done](Retired& r) {
        if (r.after > done) return false;
        DestroyImage(r.image);
        return true;
    });
    return value;
}

void Operation() { ++gOperations; }

// An image still in use by recorded or submitted work, freed when that's done.
void Retire(Image& image) {
    gRetired.push_back({image, gTimelineValue + 1});
    image = Image{};
}

// The render target at `base`. The GPU has no render target height, so the image covers
// the EDRAM from its base on, as rows of the pitch (capped).
Image* FindTarget(uint32_t base, uint32_t pitch, uint32_t msaa, uint32_t format, bool depth, VkCommandBuffer cb) {
    if (pitch == 0 || base >= kEdramTiles || msaa > 2) return nullptr;
    for (Target& t : gTargets)
        if (t.base == base && t.pitch == pitch && t.msaa == msaa && t.format == format && t.depth == depth)
            return &t.image;

    // MSAA samples are 1x2 (2x) or 2x2 (4x); a tile is 80x16 samples (40x16 at 64 bits).
    uint32_t samplesX = msaa == 2 ? 2 : 1, samplesY = msaa ? 2 : 1;
    uint32_t tileWidth = !depth && Is64Bit(format) ? 40 : 80;
    uint32_t tilesPerRow = (pitch * samplesX + tileWidth - 1) / tileWidth;
    uint32_t height = std::min((kEdramTiles - base) / tilesPerRow * 16 / samplesY, kMaxTargetHeight);
    if (!height) return nullptr;
    VkSampleCountFlagBits samples = msaa == 2   ? VK_SAMPLE_COUNT_4_BIT
                                    : msaa == 1 ? VK_SAMPLE_COUNT_2_BIT
                                                : VK_SAMPLE_COUNT_1_BIT;
    Target target{base, pitch, msaa, format, depth};
    VkImageUsageFlags usage = (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    std::string error;
    if (!CreateImage(target.image, depth ? Vk().depthFormat : ColorFormat(format), pitch, height, samples, usage, cb,
                     error)) {
        kernel::Log("  targets: can't make a %ux%u render target: %s\n", pitch, height, error.c_str());
        DestroyImage(target.image);
        return nullptr;
    }
    VkImageSubresourceRange range{target.image.aspect, 0, 1, 0, 1};
    if (depth) {
        VkClearDepthStencilValue zero{};
        vkCmdClearDepthStencilImage(cb, target.image.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    } else {
        VkClearColorValue zero{};
        vkCmdClearColorImage(cb, target.image.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    }
    FullBarrier(cb);
    std::lock_guard lock(gMutex);
    gTargets.push_back(target);
    return &gTargets.back().image;
}

// The texture a resolve writes at `address`, made (again) to fit.
Image* DestinationTexture(uint32_t address, uint32_t width, uint32_t height, VkFormat format, uint32_t guestFormat,
                          VkCommandBuffer cb) {
    Texture* texture = nullptr;
    for (Texture& t : gTextures)
        if (t.address == address) texture = &t;
    if (texture && texture->image.width == width && texture->image.height == height && texture->image.format == format) {
        texture->format = guestFormat;
        return &texture->image;
    }
    if (texture) {
        Retire(texture->image);
    } else {
        std::lock_guard lock(gMutex);
        gTextures.push_back({address, guestFormat, Image{}});
        texture = &gTextures.back();
    }
    texture->format = guestFormat;
    std::string error;
    if (!CreateImage(texture->image, format, width, height, VK_SAMPLE_COUNT_1_BIT,
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, cb,
                     error)) {
        kernel::Log("  targets: can't make a %ux%u resolve texture: %s\n", width, height, error.c_str());
        DestroyImage(texture->image);
        return nullptr;
    }
    return &texture->image;
}

float LoadFloat(const uint8_t* data, uint32_t index) {
    uint32_t word = LoadBE32(data, index * 4);
    float value;
    std::memcpy(&value, &word, 4);
    return value;
}

// The vertices of Direct3D's own rectangle lists, in fetch constant 0 (big-endian floats).
const uint8_t* RectangleVertices() { return gpu::PhysicalMemory(Register(reg::kFetchConstants) & ~3u); }

// The pixels three rectangle-list vertices cover (pixel centres are at .5).
Rect VertexRect(const uint8_t* data, uint32_t stride) {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (uint32_t i = 0; i < 3; ++i) {
        float x = LoadFloat(data, i * stride), y = LoadFloat(data, i * stride + 1);
        x0 = std::min(x0, x), x1 = std::max(x1, x);
        y0 = std::min(y0, y), y1 = std::max(y1, y);
    }
    return {int32_t(std::floor(x0 + 0.5f)), int32_t(std::floor(y0 + 0.5f)), int32_t(std::floor(x1 + 0.5f)),
            int32_t(std::floor(y1 + 0.5f))};
}

// Screen position to EDRAM position: the window offset (it moves each tile of a screen
// rendered in tiles to the top of EDRAM).
Rect InEdram(Rect r) {
    uint32_t offset = Register(reg::kPaScWindowOffset);
    int32_t x = int32_t(offset << 17) >> 17, y = int32_t(offset << 1) >> 17;
    return {r.x0 + x, r.y0 + y, r.x1 + x, r.y1 + y};
}

// Clears a rectangle of a colour target, a depth target or both (either may be null).
void ClearTargets(VkCommandBuffer cb, Image* color, const VkClearColorValue& colorValue, Image* depth,
                  VkImageAspectFlags depthAspects, const VkClearDepthStencilValue& depthValue, Rect r) {
    if (!color && !(depth && depthAspects)) return;
    int32_t width = INT32_MAX, height = INT32_MAX;
    for (Image* image : {color, depth}) {
        if (!image) continue;
        width = std::min(width, int32_t(image->width));
        height = std::min(height, int32_t(image->height));
    }
    r = {std::max(r.x0, 0), std::max(r.y0, 0), std::min(r.x1, width), std::min(r.y1, height)};
    if (r.x0 >= r.x1 || r.y0 >= r.y1) return;

    VkRenderingAttachmentInfo colorAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo depthAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
    info.renderArea = {{0, 0}, {uint32_t(width), uint32_t(height)}};
    info.layerCount = 1;
    VkClearAttachment clears[2]{};
    uint32_t clearCount = 0;
    if (color) {
        colorAttachment.imageView = color->view;
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        info.colorAttachmentCount = 1;
        info.pColorAttachments = &colorAttachment;
        clears[clearCount].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        clears[clearCount++].clearValue.color = colorValue;
    }
    if (depth && depthAspects) {
        depthAttachment.imageView = depth->view;
        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        info.pDepthAttachment = &depthAttachment;
        info.pStencilAttachment = &depthAttachment;
        clears[clearCount].aspectMask = depthAspects;
        clears[clearCount++].clearValue.depthStencil = depthValue;
    }
    VkClearRect rect{{{r.x0, r.y0}, {uint32_t(r.x1 - r.x0), uint32_t(r.y1 - r.y0)}}, 0, 1};
    vkCmdBeginRendering(cb, &info);
    vkCmdClearAttachments(cb, clearCount, clears, 1, &rect);
    vkCmdEndRendering(cb);
    FullBarrier(cb);
    Operation();
}

// Copies `edram` from the render target to the resolve's destination texture, where
// `screen` is (the window offset only moved it in EDRAM).
void CopyOut(VkCommandBuffer cb, Image& source, Rect edram, Rect screen) {
    uint32_t address = Register(reg::kRbCopyDestBase) & 0x1FFFFFFF;
    uint32_t destPitch = Register(reg::kRbCopyDestPitch);
    uint32_t width = destPitch & 0x3FFF, height = destPitch >> 16 & 0x3FFF;
    uint32_t format = Register(reg::kRbCopyDestInfo) >> 7 & 0x3F;
    Image* dest = width && height ? DestinationTexture(address, width, height, source.format, format, cb) : nullptr;
    if (!dest) {
        ++gSkippedResolves;
        return;
    }
    int32_t sx = edram.x0, sy = edram.y0, dx = screen.x0, dy = screen.y0;
    int32_t w = edram.x1 - edram.x0, h = edram.y1 - edram.y0;
    if (sx < 0) dx -= sx, w += sx, sx = 0;
    if (sy < 0) dy -= sy, h += sy, sy = 0;
    if (dx < 0) sx -= dx, w += dx, dx = 0;
    if (dy < 0) sy -= dy, h += dy, dy = 0;
    w = std::min({w, int32_t(source.width) - sx, int32_t(dest->width) - dx});
    h = std::min({h, int32_t(source.height) - sy, int32_t(dest->height) - dy});
    if (w <= 0 || h <= 0) return;
    if (source.samples != VK_SAMPLE_COUNT_1_BIT) {
        VkImageResolve region{};
        region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.srcOffset = {sx, sy, 0};
        region.dstOffset = {dx, dy, 0};
        region.extent = {uint32_t(w), uint32_t(h), 1};
        vkCmdResolveImage(cb, source.image, VK_IMAGE_LAYOUT_GENERAL, dest->image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    } else {
        VkImageCopy region{};
        region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.srcOffset = {sx, sy, 0};
        region.dstOffset = {dx, dy, 0};
        region.extent = {uint32_t(w), uint32_t(h), 1};
        vkCmdCopyImage(cb, source.image, VK_IMAGE_LAYOUT_GENERAL, dest->image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    }
    FullBarrier(cb);
    Operation();
    ++gResolves;
}

void WriteBmp(const std::filesystem::path& path, const uint8_t* rgba, uint32_t width, uint32_t height) {
    uint32_t imageSize = width * height * 4;
    uint8_t header[54] = {'B', 'M'};
    auto put32 = [&header](int at, uint32_t value) { std::memcpy(header + at, &value, 4); };
    put32(2, 54 + imageSize);
    put32(10, 54);
    put32(14, 40);
    put32(18, width);
    put32(22, uint32_t(-int32_t(height)));  // top-down
    header[26] = 1;                          // planes
    header[28] = 32;                         // bits per pixel
    put32(34, imageSize);
    std::vector<uint8_t> bgra(imageSize);
    for (uint32_t i = 0; i < width * height; ++i) {
        bgra[i * 4 + 0] = rgba[i * 4 + 2];
        bgra[i * 4 + 1] = rgba[i * 4 + 1];
        bgra[i * 4 + 2] = rgba[i * 4 + 0];
        bgra[i * 4 + 3] = rgba[i * 4 + 3];
    }
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(header), sizeof header);
    file.write(reinterpret_cast<const char*>(bgra.data()), bgra.size());
}

// A host-visible buffer the frame is copied into, for a dump.
struct Readback {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

bool CreateReadback(Readback& readback, VkDeviceSize size) {
    Vulkan& vk = Vk();
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(vk.device, &info, nullptr, &readback.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(vk.device, readback.buffer, &requirements);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = requirements.size;
    alloc.memoryTypeIndex = UINT32_MAX;
    constexpr VkMemoryPropertyFlags kWanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < vk.memory.memoryTypeCount; ++i) {
        if ((requirements.memoryTypeBits >> i & 1) && (vk.memory.memoryTypes[i].propertyFlags & kWanted) == kWanted) {
            alloc.memoryTypeIndex = i;
            break;
        }
    }
    return alloc.memoryTypeIndex != UINT32_MAX &&
           vkAllocateMemory(vk.device, &alloc, nullptr, &readback.memory) == VK_SUCCESS &&
           vkBindBufferMemory(vk.device, readback.buffer, readback.memory, 0) == VK_SUCCESS;
}

void DestroyReadback(Readback& readback) {
    if (readback.buffer) vkDestroyBuffer(Vk().device, readback.buffer, nullptr);
    if (readback.memory) vkFreeMemory(Vk().device, readback.memory, nullptr);
    readback = {};
}

}  // namespace

void ClearRectangle() {
    VkCommandBuffer cb = Commands();
    if (!cb) return;
    // Seven floats per vertex: x, y, z, then the colour (RGBA).
    const uint8_t* vertices = RectangleVertices();
    Rect r = InEdram(VertexRect(vertices, 7));
    uint32_t surface = Register(reg::kRbSurfaceInfo), pitch = surface & 0x3FFF, msaa = surface >> 16 & 3;

    Image* color = nullptr;
    if ((Register(reg::kRbModeControl) & 7) == reg::kEdramModeColorDepth && (Register(reg::kRbColorMask) & 0xF)) {
        uint32_t info = Register(reg::kRbColorInfo);
        color = FindTarget(info & 0xFFF, pitch, msaa, info >> 16 & 0xF, false, cb);
    }
    uint32_t depthControl = Register(reg::kRbDepthControl);
    VkImageAspectFlags aspects = ((depthControl & 6) == 6 ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                                 (depthControl & 1 ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    Image* depth = nullptr;
    if (aspects) {
        uint32_t info = Register(reg::kRbDepthInfo);
        depth = FindTarget(info & 0xFFF, pitch, msaa, info >> 16 & 1, true, cb);
    }
    VkClearColorValue colorValue{};
    for (int i = 0; i < 4; ++i) colorValue.float32[i] = LoadFloat(vertices, 3 + i);
    VkClearDepthStencilValue depthValue{std::clamp(LoadFloat(vertices, 2), 0.0f, 1.0f),
                                        Register(reg::kRbStencilRefMask) & 0xFF};
    ClearTargets(cb, color, colorValue, depth, aspects, depthValue, r);
    ++gClears;
}

void Resolve() {
    VkCommandBuffer cb = Commands();
    if (!cb) return;
    Rect screen = VertexRect(RectangleVertices(), 2), edram = InEdram(screen);
    uint32_t surface = Register(reg::kRbSurfaceInfo), pitch = surface & 0x3FFF, msaa = surface >> 16 & 3;
    uint32_t control = Register(reg::kRbCopyControl);
    uint32_t source = control & 7, command = control >> 20 & 3;

    Image* color = nullptr;
    if (source < 4) {
        uint32_t info = Register(source == 0 ? reg::kRbColorInfo : reg::kRbColor1Info + source - 1);
        color = FindTarget(info & 0xFFF, pitch, msaa, info >> 16 & 0xF, false, cb);
    }
    if (command != 3) {  // 3: clear only
        if (color)
            CopyOut(cb, *color, edram, screen);
        else
            ++gSkippedResolves;  // depth resolves come later
    }

    // Clears after the copy: colour packed as ARGB, depth as 24-bit depth and stencil.
    if (control >> 8 & 1 && color) {
        uint32_t packed = Register(reg::kRbColorClear);
        VkClearColorValue value{};
        value.float32[0] = float(packed >> 16 & 0xFF) / 255.0f;
        value.float32[1] = float(packed >> 8 & 0xFF) / 255.0f;
        value.float32[2] = float(packed & 0xFF) / 255.0f;
        value.float32[3] = float(packed >> 24) / 255.0f;
        ClearTargets(cb, color, value, nullptr, 0, {}, edram);
        ++gResolveClears;
    }
    if (control >> 9 & 1) {
        uint32_t info = Register(reg::kRbDepthInfo), packed = Register(reg::kRbDepthClear);
        Image* depth = FindTarget(info & 0xFFF, pitch, msaa, info >> 16 & 1, true, cb);
        VkClearDepthStencilValue value{float(packed >> 8) / float(0xFFFFFF), packed & 0xFF};
        ClearTargets(cb, nullptr, {}, depth, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, value, edram);
        ++gResolveClears;
    }
}

void Swap(uint32_t frontBuffer, uint32_t width, uint32_t height) {
    VkCommandBuffer cb = Commands();
    if (!cb || !width || !height || width > 8192 || height > 8192) return;
    Image* output = NextOutput(width, height, cb);
    if (!output) return;

    Image* front = nullptr;
    for (Texture& t : gTextures)
        if (t.address == (frontBuffer & 0x1FFFFFFF)) front = &t.image;
    if (front) {
        VkImageBlit blit{};
        blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        int32_t w = int32_t(std::min(front->width, width)), h = int32_t(std::min(front->height, height));
        blit.srcOffsets[1] = blit.dstOffsets[1] = {w, h, 1};
        vkCmdBlitImage(cb, front->image, VK_IMAGE_LAYOUT_GENERAL, output->image, VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
                       VK_FILTER_NEAREST);
        ++gFramesWithFront;
    } else {
        VkClearColorValue black{};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, output->image, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &range);
    }
    FullBarrier(cb);

    uint64_t frame = ++gFrames;
    Readback readback;
    bool dump = gDumpInterval && frame % gDumpInterval == 0 && CreateReadback(readback, uint64_t(width) * height * 4);
    if (dump) {
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(cb, output->image, VK_IMAGE_LAYOUT_GENERAL, readback.buffer, 1, &region);
    }
    uint64_t ready = Submit();
    ShowOutput(output, gTimeline, ready);
    if (dump) {
        WaitFor(ready);
        void* pixels = nullptr;
        vkMapMemory(Vk().device, readback.memory, 0, VK_WHOLE_SIZE, 0, &pixels);
        WriteBmp(std::filesystem::path(gDumpFolder) / ("frame_" + std::to_string(frame) + ".bmp"),
                 static_cast<const uint8_t*>(pixels), width, height);
        vkUnmapMemory(Vk().device, readback.memory);
        kernel::Log("  targets: frame %llu dumped (%s)\n", frame, front ? "front buffer" : "no front buffer yet");
    }
    DestroyReadback(readback);
}

void DumpFrames(uint32_t interval, const std::string& folder) {
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    gDumpInterval = interval;
    gDumpFolder = folder;
}

}  // namespace s3n::render
