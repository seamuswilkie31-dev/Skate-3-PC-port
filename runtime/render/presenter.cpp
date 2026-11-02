#include "render/presenter.h"

#include "input/input.h"
#include "kernel/kernel.h"
#include "render/shader_cache.h"
#include "render/vulkan.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

namespace s3n::render {
namespace {

constexpr uint32_t kFramesInFlight = 2;

struct Frame {
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkFence done = VK_NULL_HANDLE;
};

// An image the backend fills and the window shows.
struct Output {
    Image image;
    uint64_t releasedAt = 0;  // gPresentTimeline value once the window is done with it; 0: never shown
};

SDL_Window* gWindow = nullptr;
VkSurfaceKHR gSurface = VK_NULL_HANDLE;
bool gVsync = true;  // the launcher's VSync
VkCommandPool gCommandPool = VK_NULL_HANDLE;
VkSwapchainKHR gSwapchain = VK_NULL_HANDLE;
VkExtent2D gExtent{};
std::vector<VkImage> gImages;
std::vector<VkSemaphore> gRenderFinished;  // one per swapchain image
std::array<Frame, kFramesInFlight> gFrames{};
uint32_t gFrameIndex = 0;
std::string gGpuName;
size_t gShaderModules = 0;

// The handoff. The window's timeline counts the frames it has shown.
std::array<Output, 2> gOutputs;
uint32_t gNextOutput = 0;  // command processor thread
VkSemaphore gPresentTimeline = VK_NULL_HANDLE;
uint64_t gPresentValue = 0;  // main thread
std::mutex gSwapMutex;
std::condition_variable gSwapWake;
Output* gLatest = nullptr;  // the newest finished frame, if the window hasn't taken it
VkSemaphore gLatestTimeline = VK_NULL_HANDLE;
uint64_t gLatestReady = 0;
std::atomic<uint64_t> gSwaps{0};
std::atomic<uint64_t> gPresented{0};

bool PickGpu(std::string& error) {
    Vulkan& vk = Vk();
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(vk.instance, &count, nullptr);
    std::vector<VkPhysicalDevice> gpus(count);
    vkEnumeratePhysicalDevices(vk.instance, &count, gpus.data());
    int bestScore = -1;
    for (VkPhysicalDevice gpu : gpus) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(gpu, &properties);
        if (properties.apiVersion < VK_API_VERSION_1_3) continue;
        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &families, nullptr);
        std::vector<VkQueueFamilyProperties> familyProperties(families);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &families, familyProperties.data());
        for (uint32_t f = 0; f < families; ++f) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(gpu, f, gSurface, &present);
            if (!(familyProperties[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
            int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (score > bestScore) {
                bestScore = score;
                vk.gpu = gpu;
                vk.queueFamily = f;
                gGpuName = properties.deviceName;
            }
            break;
        }
    }
    if (!vk.gpu) error = "no Vulkan 1.3 GPU can present to the window";
    return vk.gpu != VK_NULL_HANDLE;
}

// With VSync, frames wait for the display (FIFO). Without, the newest finished frame is shown
// at once: MAILBOX where there is one, else IMMEDIATE.
VkPresentModeKHR PresentMode() {
    if (gVsync) return VK_PRESENT_MODE_FIFO_KHR;
    Vulkan& vk = Vk();
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(vk.gpu, gSurface, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(vk.gpu, gSurface, &count, modes.data());
    for (VkPresentModeKHR wanted : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR})
        if (std::find(modes.begin(), modes.end(), wanted) != modes.end()) return wanted;
    return VK_PRESENT_MODE_FIFO_KHR;
}

void DestroySwapchain() {
    VkDevice device = Vk().device;
    for (VkSemaphore s : gRenderFinished) vkDestroySemaphore(device, s, nullptr);
    gRenderFinished.clear();
    gImages.clear();
    if (gSwapchain) vkDestroySwapchainKHR(device, gSwapchain, nullptr);
    gSwapchain = VK_NULL_HANDLE;
}

bool CreateSwapchain(std::string& error) {
    Vulkan& vk = Vk();
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.gpu, gSurface, &caps);
    if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0) return true;  // minimized

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk.gpu, gSurface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk.gpu, gSurface, &formatCount, formats.data());
    VkSurfaceFormatKHR format = formats[0];
    for (const VkSurfaceFormatKHR& f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) format = f;

    gExtent = caps.currentExtent;
    if (gExtent.width == 0xFFFFFFFF) {
        int w, h;
        SDL_GetWindowSizeInPixels(gWindow, &w, &h);
        gExtent = {uint32_t(w), uint32_t(h)};
    }
    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = gSurface;
    info.minImageCount = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount) info.minImageCount = std::min(info.minImageCount, caps.maxImageCount);
    info.imageFormat = format.format;
    info.imageColorSpace = format.colorSpace;
    info.imageExtent = gExtent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = PresentMode();
    info.clipped = VK_TRUE;
    if (!Check(vkCreateSwapchainKHR(vk.device, &info, nullptr, &gSwapchain), "vkCreateSwapchainKHR", error))
        return false;

    uint32_t imageCount = 0;
    vkGetSwapchainImagesKHR(vk.device, gSwapchain, &imageCount, nullptr);
    gImages.resize(imageCount);
    vkGetSwapchainImagesKHR(vk.device, gSwapchain, &imageCount, gImages.data());
    gRenderFinished.resize(imageCount);
    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (VkSemaphore& s : gRenderFinished)
        if (!Check(vkCreateSemaphore(vk.device, &semaphoreInfo, nullptr, &s), "vkCreateSemaphore", error)) return false;
    return true;
}

bool RecreateSwapchain() {
    {
        std::lock_guard lock(Vk().queueMutex);
        vkDeviceWaitIdle(Vk().device);
    }
    DestroySwapchain();
    std::string error;
    if (!CreateSwapchain(error)) {
        kernel::Log("  render: %s\n", error.c_str());
        return false;
    }
    return true;
}

void Barrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags src,
             VkPipelineStageFlags dst, VkAccessFlags srcAccess, VkAccessFlags dstAccess) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, src, dst, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

// One frame: acquire, then the newest finished frame scaled into the window with its
// aspect ratio kept, and present.
void PresentFrame() {
    if (!gSwapchain && !RecreateSwapchain()) return;
    if (!gSwapchain) return;  // minimized
    VkDevice device = Vk().device;
    Frame& frame = gFrames[gFrameIndex];
    vkWaitForFences(device, 1, &frame.done, VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    VkResult result =
        vkAcquireNextImageKHR(device, gSwapchain, UINT64_MAX, frame.imageAvailable, VK_NULL_HANDLE, &imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        RecreateSwapchain();
        return;
    }
    // Taken only now: once the frame is taken, this submission must signal its release.
    Output* output;
    VkSemaphore timeline;
    uint64_t ready, release;
    {
        std::lock_guard lock(gSwapMutex);
        output = gLatest;
        timeline = gLatestTimeline;
        ready = gLatestReady;
        gLatest = nullptr;
        release = ++gPresentValue;
        if (output) output->releasedAt = release;
    }
    vkResetFences(device, 1, &frame.done);

    VkCommandBuffer cb = frame.commands;
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &begin);
    VkImage image = gImages[imageIndex];
    Barrier(cb, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkClearColorValue black{};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    if (output) {
        const Image& src = output->image;
        float scale = std::min(float(gExtent.width) / src.width, float(gExtent.height) / src.height);
        int32_t w = int32_t(src.width * scale), h = int32_t(src.height * scale);
        int32_t x = (int32_t(gExtent.width) - w) / 2, y = (int32_t(gExtent.height) - h) / 2;
        VkImageBlit blit{};
        blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {int32_t(src.width), int32_t(src.height), 1};
        blit.dstOffsets[0] = {x, y, 0};
        blit.dstOffsets[1] = {x + w, y + h, 1};
        Barrier(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdBlitImage(cb, src.image, VK_IMAGE_LAYOUT_GENERAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_LINEAR);
    }
    Barrier(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    vkEndCommandBuffer(cb);

    // Waits for the swapchain image and (with a frame) the backend's commands; signals the
    // present and the window's timeline.
    VkSemaphore waits[2] = {frame.imageAvailable, timeline};
    uint64_t waitValues[2] = {0, ready};
    VkPipelineStageFlags waitStages[2] = {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT};
    VkSemaphore signals[2] = {gRenderFinished[imageIndex], gPresentTimeline};
    uint64_t signalValues[2] = {0, release};
    VkTimelineSemaphoreSubmitInfo values{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    values.waitSemaphoreValueCount = output ? 2 : 1;
    values.pWaitSemaphoreValues = waitValues;
    values.signalSemaphoreValueCount = 2;
    values.pSignalSemaphoreValues = signalValues;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.pNext = &values;
    submit.waitSemaphoreCount = output ? 2 : 1;
    submit.pWaitSemaphores = waits;
    submit.pWaitDstStageMask = waitStages;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    submit.signalSemaphoreCount = 2;
    submit.pSignalSemaphores = signals;

    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &gRenderFinished[imageIndex];
    present.swapchainCount = 1;
    present.pSwapchains = &gSwapchain;
    present.pImageIndices = &imageIndex;
    {
        std::lock_guard lock(Vk().queueMutex);
        vkQueueSubmit(Vk().queue, 1, &submit, frame.done);
        result = vkQueuePresentKHR(Vk().queue, &present);
    }
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) RecreateSwapchain();
    gFrameIndex = (gFrameIndex + 1) % kFramesInFlight;
    ++gPresented;
}

std::string Describe() {
    char line[192];
    std::snprintf(line, sizeof line, "render: %s, %ux%u; %llu frames presented for %llu frames finished\n",
                  gGpuName.c_str(), gExtent.width, gExtent.height, gPresented.load(), gSwaps.load());
    return line;
}

}  // namespace

void SetVsync(bool vsync) { gVsync = vsync; }

bool Init(std::string& error) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        error = std::string("SDL_Init: ") + SDL_GetError();
        return false;
    }
    gWindow = SDL_CreateWindow("Skate 3 Native", 1280, 720, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!gWindow) {
        error = std::string("SDL_CreateWindow: ") + SDL_GetError();
        return false;
    }

    Vulkan& vk = Vk();
    Uint32 extensionCount = 0;
    const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&extensionCount);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Skate 3 Native";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &app;
    instanceInfo.enabledExtensionCount = extensionCount;
    instanceInfo.ppEnabledExtensionNames = extensions;
    if (!Check(vkCreateInstance(&instanceInfo, nullptr, &vk.instance), "vkCreateInstance", error)) return false;
    if (!SDL_Vulkan_CreateSurface(gWindow, vk.instance, nullptr, &gSurface)) {
        error = std::string("SDL_Vulkan_CreateSurface: ") + SDL_GetError();
        return false;
    }
    if (!PickGpu(error)) return false;
    vkGetPhysicalDeviceMemoryProperties(vk.gpu, &vk.memory);

    // The translated shaders read constants through buffer device addresses and index
    // textures and samplers in bindless arrays (XenosRecomp's interface). The backend
    // renders with dynamic rendering and syncs with the window through timeline semaphores.
    VkPhysicalDeviceVulkan13Features supported13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features supported12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    supported12.pNext = &supported13;
    VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    supported.pNext = &supported12;
    vkGetPhysicalDeviceFeatures2(vk.gpu, &supported);
    if (!supported12.bufferDeviceAddress || !supported12.descriptorIndexing || !supported12.runtimeDescriptorArray ||
        !supported12.descriptorBindingPartiallyBound || !supported12.descriptorBindingSampledImageUpdateAfterBind ||
        !supported12.descriptorBindingUpdateUnusedWhilePending || !supported12.timelineSemaphore ||
        !supported13.dynamicRendering || !supported.features.shaderInt64) {
        error = gGpuName +
                " lacks Vulkan features the renderer needs (buffer device address, descriptor indexing, timeline "
                "semaphores, dynamic rendering, 64-bit ints)";
        return false;
    }
    VkPhysicalDeviceVulkan13Features enable13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    enable13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceVulkan12Features enable12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    enable12.pNext = &enable13;
    enable12.bufferDeviceAddress = VK_TRUE;
    enable12.descriptorIndexing = VK_TRUE;
    enable12.runtimeDescriptorArray = VK_TRUE;
    enable12.descriptorBindingPartiallyBound = VK_TRUE;
    enable12.descriptorBindingVariableDescriptorCount = supported12.descriptorBindingVariableDescriptorCount;
    enable12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    enable12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    enable12.shaderSampledImageArrayNonUniformIndexing = supported12.shaderSampledImageArrayNonUniformIndexing;
    enable12.timelineSemaphore = VK_TRUE;
    VkPhysicalDeviceFeatures2 enable{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    enable.pNext = &enable12;
    enable.features.shaderInt64 = VK_TRUE;
    enable.features.shaderInt16 = supported.features.shaderInt16;

    // The GPU's depth buffers are 24-bit depth with 8-bit stencil.
    for (VkFormat format : {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT}) {
        VkFormatProperties properties;
        vkGetPhysicalDeviceFormatProperties(vk.gpu, format, &properties);
        if (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            vk.depthFormat = format;
            break;
        }
    }
    if (!vk.depthFormat) {
        error = gGpuName + " has no depth-stencil format for the GPU's depth buffers";
        return false;
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = vk.queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.pNext = &enable;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = 1;
    deviceInfo.ppEnabledExtensionNames = deviceExtensions;
    if (!Check(vkCreateDevice(vk.gpu, &deviceInfo, nullptr, &vk.device), "vkCreateDevice", error)) return false;
    vkGetDeviceQueue(vk.device, vk.queueFamily, 0, &vk.queue);

    // A module for every translated shader, so the driver vets them all up front.
    size_t shaderCount = 0;
    Shader* shaders = Shaders(shaderCount);
    for (size_t i = 0; i < shaderCount; ++i) {
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize = shaders[i].spirvSize;
        moduleInfo.pCode = shaders[i].spirv;
        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(vk.device, &moduleInfo, nullptr, &module) == VK_SUCCESS) {
            shaders[i].module = module;
            ++gShaderModules;
        }
    }

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = vk.queueFamily;
    if (!Check(vkCreateCommandPool(vk.device, &poolInfo, nullptr, &gCommandPool), "vkCreateCommandPool", error))
        return false;
    for (Frame& frame : gFrames) {
        VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocInfo.commandPool = gCommandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (!Check(vkAllocateCommandBuffers(vk.device, &allocInfo, &frame.commands), "vkAllocateCommandBuffers",
                   error) ||
            !Check(vkCreateSemaphore(vk.device, &semaphoreInfo, nullptr, &frame.imageAvailable), "vkCreateSemaphore",
                   error) ||
            !Check(vkCreateFence(vk.device, &fenceInfo, nullptr, &frame.done), "vkCreateFence", error))
            return false;
    }
    VkSemaphoreTypeCreateInfo timelineType{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    timelineType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo timelineInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    timelineInfo.pNext = &timelineType;
    if (!Check(vkCreateSemaphore(vk.device, &timelineInfo, nullptr, &gPresentTimeline), "vkCreateSemaphore", error))
        return false;
    if (!CreateSwapchain(error)) return false;
    kernel::AddReportSection(Describe);
    kernel::Log("  render: Vulkan on %s, %ux%u window; %zu of %zu shaders loaded as modules\n", gGpuName.c_str(),
                gExtent.width, gExtent.height, gShaderModules, shaderCount);
    return true;
}

void Run() {
    using Clock = std::chrono::steady_clock;
    Clock::time_point second = Clock::now();
    uint64_t swapsAtSecond = 0;
    for (;;) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) return;
            if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) RecreateSwapchain();
            input::HandleEvent(event);
        }
        input::Poll();
        bool finished;
        {
            std::unique_lock lock(gSwapMutex);
            gSwapWake.wait_for(lock, std::chrono::milliseconds(16), [] { return gLatest != nullptr; });
            finished = gLatest != nullptr;
        }
        if (finished) PresentFrame();  // the newest frame only; any between were faster than the display

        if (Clock::now() - second >= std::chrono::seconds(1)) {
            uint64_t swaps = gSwaps.load();
            char title[96];
            std::snprintf(title, sizeof title, "Skate 3 Native - %llu fps", swaps - swapsAtSecond);
            SDL_SetWindowTitle(gWindow, title);
            swapsAtSecond = swaps;
            second = Clock::now();
        }
    }
}

Image* NextOutput(uint32_t width, uint32_t height, VkCommandBuffer cb) {
    VkDevice device = Vk().device;
    if (!device) return nullptr;
    Output& output = gOutputs[gNextOutput];
    uint64_t released;
    {
        std::lock_guard lock(gSwapMutex);
        released = output.releasedAt;
    }
    if (released) {
        VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wait.semaphoreCount = 1;
        wait.pSemaphores = &gPresentTimeline;
        wait.pValues = &released;
        vkWaitSemaphores(device, &wait, UINT64_MAX);
    }
    if (output.image.width != width || output.image.height != height) {
        DestroyImage(output.image);
        std::string error;
        if (!CreateImage(output.image, VK_FORMAT_R8G8B8A8_UNORM, width, height, VK_SAMPLE_COUNT_1_BIT,
                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, cb, error)) {
            kernel::Log("  render: can't make an output image: %s\n", error.c_str());
            return nullptr;
        }
    }
    return &output.image;
}

void ShowOutput(Image* image, VkSemaphore timeline, uint64_t ready) {
    ++gSwaps;
    Output* output = image == &gOutputs[0].image ? &gOutputs[0] : &gOutputs[1];
    gNextOutput = output == &gOutputs[0] ? 1 : 0;
    {
        std::lock_guard lock(gSwapMutex);
        gLatest = output;
        gLatestTimeline = timeline;
        gLatestReady = ready;
    }
    gSwapWake.notify_one();
}

}  // namespace s3n::render
