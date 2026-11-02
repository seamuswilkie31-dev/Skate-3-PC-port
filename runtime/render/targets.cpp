#include "render/targets.h"

#include "gpu/gpu.h"
#include "gpu/registers.h"
#include "kernel/kernel.h"
#include "memory.h"
#include "render/backend.h"
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
using namespace backend;

constexpr uint32_t kEdramTiles = 2048;
constexpr uint32_t kMaxTargetHeight = 2048;
constexpr uint32_t kMaxOperations = 4096;  // per recording, before it's submitted without a swap
constexpr VkDeviceSize kUploadRingSize = 64ull << 20;

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
    uint64_t serial = 0;  // new for each image made
};
uint64_t gTextureSerial = 0;

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

// Each command buffer's upload ring, reset when its recording starts.
struct UploadRing {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* mapped = nullptr;
    VkDeviceAddress address = 0;
    VkDeviceSize used = 0;
};
UploadRing gUploads[2];

// The open rendering scope, if any.
bool gRenderingOpen = false;
VkCommandBuffer gRenderingCommands = VK_NULL_HANDLE;
Image* gOpenColor = nullptr;
Image* gOpenDepth = nullptr;

uint32_t gDumpInterval = 0;
uint64_t gCaptureFrame = 0;
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

// Host-visible memory with a device address, mapped for good. Device-local memory the CPU
// can write is preferred where the GPU offers it.
bool CreateUploadRing(UploadRing& ring, std::string& error) {
    Vulkan& vk = Vk();
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = kUploadRingSize;
    info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (!Check(vkCreateBuffer(vk.device, &info, nullptr, &ring.buffer), "vkCreateBuffer", error)) return false;
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(vk.device, ring.buffer, &requirements);
    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.pNext = &flags;
    alloc.allocationSize = requirements.size;
    constexpr VkMemoryPropertyFlags kHost = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (VkMemoryPropertyFlags wanted : {kHost | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, kHost}) {
        for (uint32_t i = 0; i < vk.memory.memoryTypeCount && !ring.memory; ++i) {
            if (!(requirements.memoryTypeBits >> i & 1) || (vk.memory.memoryTypes[i].propertyFlags & wanted) != wanted)
                continue;
            alloc.memoryTypeIndex = i;
            if (vkAllocateMemory(vk.device, &alloc, nullptr, &ring.memory) != VK_SUCCESS) ring.memory = VK_NULL_HANDLE;
        }
    }
    if (!ring.memory) {
        error = "no host-visible memory for the upload ring";
        return false;
    }
    void* mapped = nullptr;
    if (!Check(vkBindBufferMemory(vk.device, ring.buffer, ring.memory, 0), "vkBindBufferMemory", error) ||
        !Check(vkMapMemory(vk.device, ring.memory, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory", error))
        return false;
    ring.mapped = static_cast<uint8_t*>(mapped);
    VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    addressInfo.buffer = ring.buffer;
    ring.address = vkGetBufferDeviceAddress(vk.device, &addressInfo);
    return true;
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
         Check(vkCreateSemaphore(vk.device, &timeline, nullptr, &gTimeline), "vkCreateSemaphore", error) &&
         CreateUploadRing(gUploads[0], error) && CreateUploadRing(gUploads[1], error);
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

}  // namespace

namespace backend {

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
        gUploads[gCurrent].used = 0;
    }
    return gCommands[gCurrent];
}

void Operation() { ++gOperations; }

void BeginRendering(VkCommandBuffer cb, Image* color, Image* depth) {
    if (gRenderingOpen && gRenderingCommands == cb && gOpenColor == color && gOpenDepth == depth) return;
    EndRendering();
    uint32_t width = UINT32_MAX, height = UINT32_MAX;
    for (Image* image : {color, depth}) {
        if (!image) continue;
        width = std::min(width, image->width);
        height = std::min(height, image->height);
    }
    if (width == UINT32_MAX) return;
    VkRenderingAttachmentInfo colorAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo depthAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
    info.renderArea = {{0, 0}, {width, height}};
    info.layerCount = 1;
    if (color) {
        colorAttachment.imageView = color->view;
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        info.colorAttachmentCount = 1;
        info.pColorAttachments = &colorAttachment;
    }
    if (depth) {
        depthAttachment.imageView = depth->view;
        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        info.pDepthAttachment = &depthAttachment;
        info.pStencilAttachment = &depthAttachment;
    }
    vkCmdBeginRendering(cb, &info);
    gRenderingOpen = true;
    gRenderingCommands = cb;
    gOpenColor = color;
    gOpenDepth = depth;
}

void EndRendering() {
    if (!gRenderingOpen) return;
    vkCmdEndRendering(gRenderingCommands);
    FullBarrier(gRenderingCommands);
    gRenderingOpen = false;
}

uint64_t FrameNumber() { return gFrames; }

bool Capturing() { return gCaptureFrame && gFrames + 1 == gCaptureFrame; }

Image* ResolvedImage(uint32_t address, uint64_t& serial) {
    for (Texture& t : gTextures) {
        if (t.address == address && t.image.image) {
            serial = t.serial;
            return &t.image;
        }
    }
    return nullptr;
}

VkCommandBuffer Allocate(VkDeviceSize size, Upload& upload) {
    VkCommandBuffer cb = Commands();
    if (!cb || size > kUploadRingSize) return VK_NULL_HANDLE;
    VkDeviceSize offset = (gUploads[gCurrent].used + 255) & ~VkDeviceSize(255);
    if (offset + size > kUploadRingSize) {  // full: this recording goes, the next starts empty
        Submit();
        cb = Commands();
        offset = 0;
    }
    UploadRing& ring = gUploads[gCurrent];
    ring.used = offset + size;
    upload = {ring.mapped + offset, ring.buffer, offset, ring.address + offset};
    return cb;
}

}  // namespace backend

namespace {

// Submits the recording. Returns the timeline value it signals when done.
uint64_t Submit() {
    EndRendering();
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

// An image still in use by recorded or submitted work, freed when that's done.
void Retire(Image& image) {
    gRetired.push_back({image, gTimelineValue + 1});
    image = Image{};
}

}  // namespace

namespace backend {

// The GPU has no render target height, so the image covers the EDRAM from its base on, as
// rows of the pitch (capped).
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
    EndRendering();  // the new image is set up outside rendering
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

}  // namespace backend

namespace {

// Bytes per pixel of a resolve's destination format (RB_COPY_DEST_INFO's).
uint32_t DestinationBytes(uint32_t format) {
    switch (format) {
    case 2: return 1;                                     // 8
    case 3: case 4: case 10: case 24: case 30: return 2;  // 1_5_5_5, 5_6_5, 8_8, 16, 16 float
    case 26: case 32: case 37: return 8;                  // 16_16_16_16, its float, 32_32 float
    case 38: return 16;                                   // 32_32_32_32 float
    default: return 4;
    }
}

// The texture a resolve writes at `address`, made (again) to fit, and the row `address` is
// at. A screen drawn in tiles is resolved a tile at a time, and Direct3D points each tile's
// resolve at the tile's first row, inside the texture the first tile made.
Image* DestinationTexture(uint32_t address, uint32_t width, uint32_t height, VkFormat format, uint32_t guestFormat,
                          VkCommandBuffer cb, int32_t& row) {
    row = 0;
    Texture* texture = nullptr;
    for (Texture& t : gTextures)
        if (t.address == address) texture = &t;
    if (!texture) {
        uint32_t rowBytes = ((width + 31) & ~31u) * DestinationBytes(guestFormat);
        for (Texture& t : gTextures) {
            if (!t.image.image || t.image.width != width || t.image.format != format || address <= t.address) continue;
            uint32_t offset = address - t.address;
            if (offset % rowBytes == 0 && offset / rowBytes < t.image.height) {
                row = int32_t(offset / rowBytes);
                return &t.image;
            }
        }
    }
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
    texture->serial = ++gTextureSerial;
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

    VkClearAttachment clears[2]{};
    uint32_t clearCount = 0;
    if (color) {
        clears[clearCount].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        clears[clearCount++].clearValue.color = colorValue;
    }
    if (depth && depthAspects) {
        clears[clearCount].aspectMask = depthAspects;
        clears[clearCount++].clearValue.depthStencil = depthValue;
    }
    VkClearRect rect{{{r.x0, r.y0}, {uint32_t(r.x1 - r.x0), uint32_t(r.y1 - r.y0)}}, 0, 1};
    BeginRendering(cb, color, depth && depthAspects ? depth : nullptr);
    vkCmdClearAttachments(cb, clearCount, clears, 1, &rect);
    Operation();
}

uint32_t TilesPerRow(uint32_t pitch, uint32_t msaa, uint32_t format, bool depth) {
    uint32_t samplesX = msaa == 2 ? 2 : 1, tileWidth = !depth && Is64Bit(format) ? 40 : 80;
    return (pitch * samplesX + tileWidth - 1) / tileWidth;
}

// Clears `r` (pixels) of the render target at this EDRAM base, and of every other render
// target laid out over the same tiles: Direct3D clears through an MSAA view of a surface
// (a 640x360 4x target holds a 1280x720 one's samples), so a clear has to reach every view
// of those samples. Each gets the rectangle in its own pixels.
void ClearEdram(VkCommandBuffer cb, bool depth, uint32_t base, uint32_t pitch, uint32_t msaa, uint32_t format, Rect r,
                const VkClearColorValue& colorValue, const VkClearDepthStencilValue& depthValue,
                VkImageAspectFlags aspects) {
    if (!FindTarget(base, pitch, msaa, format, depth, cb)) return;
    uint32_t sx = msaa == 2 ? 2 : 1, sy = msaa ? 2 : 1, row = TilesPerRow(pitch, msaa, format, depth);
    Rect samples{r.x0 * int32_t(sx), r.y0 * int32_t(sy), r.x1 * int32_t(sx), r.y1 * int32_t(sy)};
    for (Target& t : gTargets) {
        if (t.depth != depth || t.base != base || TilesPerRow(t.pitch, t.msaa, t.format, depth) != row) continue;
        if (!depth && Is64Bit(t.format) != Is64Bit(format)) continue;
        int32_t tx = t.msaa == 2 ? 2 : 1, ty = t.msaa ? 2 : 1;
        Rect p{samples.x0 / tx, samples.y0 / ty, (samples.x1 + tx - 1) / tx, (samples.y1 + ty - 1) / ty};
        if (depth)
            ClearTargets(cb, nullptr, colorValue, &t.image, aspects, depthValue, p);
        else
            ClearTargets(cb, &t.image, colorValue, nullptr, 0, depthValue, p);
    }
}

// Copies `edram` from the render target to the resolve's destination texture, at the same
// position from the destination's address.
void CopyOut(VkCommandBuffer cb, Image& source, Rect edram) {
    EndRendering();
    uint32_t address = Register(reg::kRbCopyDestBase) & 0x1FFFFFFF;
    uint32_t destPitch = Register(reg::kRbCopyDestPitch);
    uint32_t width = destPitch & 0x3FFF, height = destPitch >> 16 & 0x3FFF;
    uint32_t format = Register(reg::kRbCopyDestInfo) >> 7 & 0x3F;
    int32_t row = 0;
    Image* dest = width && height ? DestinationTexture(address, width, height, source.format, format, cb, row) : nullptr;
    if (!dest) {
        ++gSkippedResolves;
        return;
    }
    int32_t sx = edram.x0, sy = edram.y0, dx = edram.x0, dy = edram.y0 + row;
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

    bool color = (Register(reg::kRbModeControl) & 7) == reg::kEdramModeColorDepth && (Register(reg::kRbColorMask) & 0xF);
    uint32_t colorInfo = Register(reg::kRbColorInfo), depthInfo = Register(reg::kRbDepthInfo);
    uint32_t depthControl = Register(reg::kRbDepthControl);
    VkImageAspectFlags aspects = ((depthControl & 6) == 6 ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                                 (depthControl & 1 ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    VkClearColorValue colorValue{};
    for (int i = 0; i < 4; ++i) colorValue.float32[i] = LoadFloat(vertices, 3 + i);
    VkClearDepthStencilValue depthValue{std::clamp(LoadFloat(vertices, 2), 0.0f, 1.0f),
                                        Register(reg::kRbStencilRefMask) & 0xFF};
    if (Capturing())
        kernel::Log("  capture: clear (%d,%d)-(%d,%d) of surface %08X: colour %s (%.2f %.2f %.2f %.2f) depth %s %.3f\n",
                    r.x0, r.y0, r.x1, r.y1, surface, color ? "yes" : "no", colorValue.float32[0], colorValue.float32[1],
                    colorValue.float32[2], colorValue.float32[3], aspects ? "yes" : "no", depthValue.depth);
    if (color) ClearEdram(cb, false, colorInfo & 0xFFF, pitch, msaa, colorInfo >> 16 & 0xF, r, colorValue, depthValue, 0);
    if (aspects) ClearEdram(cb, true, depthInfo & 0xFFF, pitch, msaa, depthInfo >> 16 & 1, r, colorValue, depthValue, aspects);
    ++gClears;
}

void Resolve() {
    VkCommandBuffer cb = Commands();
    if (!cb) return;
    Rect screen = VertexRect(RectangleVertices(), 2), edram = InEdram(screen);
    uint32_t surface = Register(reg::kRbSurfaceInfo), pitch = surface & 0x3FFF, msaa = surface >> 16 & 3;
    uint32_t control = Register(reg::kRbCopyControl);
    uint32_t source = control & 7, command = control >> 20 & 3;
    if (Capturing())
        kernel::Log("  capture: resolve source %u (surface %08X, colour %08X) (%d,%d)-(%d,%d) to %08X pitch %08X info %08X, "
                    "control %08X\n", source, surface, Register(reg::kRbColorInfo), screen.x0, screen.y0, screen.x1, screen.y1,
                    Register(reg::kRbCopyDestBase), Register(reg::kRbCopyDestPitch), Register(reg::kRbCopyDestInfo), control);

    Image* color = nullptr;
    if (source < 4) {
        uint32_t info = Register(source == 0 ? reg::kRbColorInfo : reg::kRbColor1Info + source - 1);
        color = FindTarget(info & 0xFFF, pitch, msaa, info >> 16 & 0xF, false, cb);
    }
    if (command != 3) {  // 3: clear only
        if (color)
            CopyOut(cb, *color, edram);
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
        uint32_t info = Register(source == 0 ? reg::kRbColorInfo : reg::kRbColor1Info + source - 1);
        ClearEdram(cb, false, info & 0xFFF, pitch, msaa, info >> 16 & 0xF, edram, value, {}, 0);
        ++gResolveClears;
    }
    if (control >> 9 & 1) {
        uint32_t info = Register(reg::kRbDepthInfo), packed = Register(reg::kRbDepthClear);
        VkClearDepthStencilValue value{float(packed >> 8) / float(0xFFFFFF), packed & 0xFF};
        ClearEdram(cb, true, info & 0xFFF, pitch, msaa, info >> 16 & 1, edram, {}, value,
                   VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
        ++gResolveClears;
    }
}

void Swap(uint32_t frontBuffer, uint32_t width, uint32_t height) {
    VkCommandBuffer cb = Commands();
    if (!cb || !width || !height || width > 8192 || height > 8192) return;
    EndRendering();
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

void CaptureFrame(uint64_t frame) { gCaptureFrame = frame; }

void DumpFrames(uint32_t interval, const std::string& folder) {
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    gDumpInterval = interval;
    gDumpFolder = folder;
}

}  // namespace s3n::render
