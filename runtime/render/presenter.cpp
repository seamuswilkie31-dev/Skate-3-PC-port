#include "render/presenter.h"

#include "kernel/kernel.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

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

SDL_Window* gWindow = nullptr;
VkInstance gInstance = VK_NULL_HANDLE;
VkSurfaceKHR gSurface = VK_NULL_HANDLE;
VkPhysicalDevice gGpu = VK_NULL_HANDLE;
VkDevice gDevice = VK_NULL_HANDLE;
uint32_t gQueueFamily = 0;
VkQueue gQueue = VK_NULL_HANDLE;
VkCommandPool gCommandPool = VK_NULL_HANDLE;
VkSwapchainKHR gSwapchain = VK_NULL_HANDLE;
VkExtent2D gExtent{};
std::vector<VkImage> gImages;
std::vector<VkSemaphore> gRenderFinished;  // one per swapchain image
std::array<Frame, kFramesInFlight> gFrames{};
uint32_t gFrameIndex = 0;
std::string gGpuName;

std::mutex gSwapMutex;
std::condition_variable gSwapWake;
uint64_t gSwapsPending = 0;
std::atomic<uint64_t> gSwaps{0};
std::atomic<uint64_t> gPresented{0};

bool Check(VkResult result, const char* what, std::string& error) {
    if (result == VK_SUCCESS) return true;
    error = std::string(what) + " failed (VkResult " + std::to_string(int(result)) + ")";
    return false;
}

bool PickGpu(std::string& error) {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(gInstance, &count, nullptr);
    std::vector<VkPhysicalDevice> gpus(count);
    vkEnumeratePhysicalDevices(gInstance, &count, gpus.data());
    int bestScore = -1;
    for (VkPhysicalDevice gpu : gpus) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(gpu, &properties);
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
                gGpu = gpu;
                gQueueFamily = f;
                gGpuName = properties.deviceName;
            }
            break;
        }
    }
    if (!gGpu) error = "no Vulkan GPU can present to the window";
    return gGpu != VK_NULL_HANDLE;
}

void DestroySwapchain() {
    for (VkSemaphore s : gRenderFinished) vkDestroySemaphore(gDevice, s, nullptr);
    gRenderFinished.clear();
    gImages.clear();
    if (gSwapchain) vkDestroySwapchainKHR(gDevice, gSwapchain, nullptr);
    gSwapchain = VK_NULL_HANDLE;
}

bool CreateSwapchain(std::string& error) {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gGpu, gSurface, &caps);
    if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0) return true;  // minimized

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gGpu, gSurface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gGpu, gSurface, &formatCount, formats.data());
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
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // vsync; the launcher's options come later
    info.clipped = VK_TRUE;
    if (!Check(vkCreateSwapchainKHR(gDevice, &info, nullptr, &gSwapchain), "vkCreateSwapchainKHR", error)) return false;

    uint32_t imageCount = 0;
    vkGetSwapchainImagesKHR(gDevice, gSwapchain, &imageCount, nullptr);
    gImages.resize(imageCount);
    vkGetSwapchainImagesKHR(gDevice, gSwapchain, &imageCount, gImages.data());
    gRenderFinished.resize(imageCount);
    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (VkSemaphore& s : gRenderFinished)
        if (!Check(vkCreateSemaphore(gDevice, &semaphoreInfo, nullptr, &s), "vkCreateSemaphore", error)) return false;
    return true;
}

bool RecreateSwapchain() {
    vkDeviceWaitIdle(gDevice);
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

// One frame: acquire, clear, present.
void PresentFrame() {
    if (!gSwapchain && !RecreateSwapchain()) return;
    if (!gSwapchain) return;  // minimized
    Frame& frame = gFrames[gFrameIndex];
    vkWaitForFences(gDevice, 1, &frame.done, VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    VkResult result =
        vkAcquireNextImageKHR(gDevice, gSwapchain, UINT64_MAX, frame.imageAvailable, VK_NULL_HANDLE, &imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        RecreateSwapchain();
        return;
    }
    vkResetFences(gDevice, 1, &frame.done);

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
    Barrier(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    vkEndCommandBuffer(cb);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &frame.imageAvailable;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &gRenderFinished[imageIndex];
    vkQueueSubmit(gQueue, 1, &submit, frame.done);

    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &gRenderFinished[imageIndex];
    present.swapchainCount = 1;
    present.pSwapchains = &gSwapchain;
    present.pImageIndices = &imageIndex;
    result = vkQueuePresentKHR(gQueue, &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) RecreateSwapchain();
    gFrameIndex = (gFrameIndex + 1) % kFramesInFlight;
    ++gPresented;
}

std::string Describe() {
    char line[192];
    std::snprintf(line, sizeof line, "render: %s, %ux%u; %llu frames presented for %llu game swaps\n",
                  gGpuName.c_str(), gExtent.width, gExtent.height, gPresented.load(), gSwaps.load());
    return line;
}

}  // namespace

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

    Uint32 extensionCount = 0;
    const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&extensionCount);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Skate 3 Native";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &app;
    instanceInfo.enabledExtensionCount = extensionCount;
    instanceInfo.ppEnabledExtensionNames = extensions;
    if (!Check(vkCreateInstance(&instanceInfo, nullptr, &gInstance), "vkCreateInstance", error)) return false;
    if (!SDL_Vulkan_CreateSurface(gWindow, gInstance, nullptr, &gSurface)) {
        error = std::string("SDL_Vulkan_CreateSurface: ") + SDL_GetError();
        return false;
    }
    if (!PickGpu(error)) return false;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = gQueueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = 1;
    deviceInfo.ppEnabledExtensionNames = deviceExtensions;
    if (!Check(vkCreateDevice(gGpu, &deviceInfo, nullptr, &gDevice), "vkCreateDevice", error)) return false;
    vkGetDeviceQueue(gDevice, gQueueFamily, 0, &gQueue);

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = gQueueFamily;
    if (!Check(vkCreateCommandPool(gDevice, &poolInfo, nullptr, &gCommandPool), "vkCreateCommandPool", error))
        return false;
    for (Frame& frame : gFrames) {
        VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocInfo.commandPool = gCommandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (!Check(vkAllocateCommandBuffers(gDevice, &allocInfo, &frame.commands), "vkAllocateCommandBuffers", error) ||
            !Check(vkCreateSemaphore(gDevice, &semaphoreInfo, nullptr, &frame.imageAvailable), "vkCreateSemaphore", error) ||
            !Check(vkCreateFence(gDevice, &fenceInfo, nullptr, &frame.done), "vkCreateFence", error))
            return false;
    }
    if (!CreateSwapchain(error)) return false;
    kernel::AddReportSection(Describe);
    kernel::Log("  render: Vulkan on %s, %ux%u window\n", gGpuName.c_str(), gExtent.width, gExtent.height);
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
        }
        bool swapped;
        {
            std::unique_lock lock(gSwapMutex);
            gSwapWake.wait_for(lock, std::chrono::milliseconds(16), [] { return gSwapsPending > 0; });
            swapped = gSwapsPending > 0;
            gSwapsPending = 0;  // present at most once per loop; extra swaps were faster than the display
        }
        if (swapped) PresentFrame();

        if (Clock::now() - second >= std::chrono::seconds(1)) {
            uint64_t swaps = gSwaps.load();
            char title[96];
            std::snprintf(title, sizeof title, "Skate 3 Native - %llu fps (nothing drawn yet)", swaps - swapsAtSecond);
            SDL_SetWindowTitle(gWindow, title);
            swapsAtSecond = swaps;
            second = Clock::now();
        }
    }
}

void NotifySwap() {
    ++gSwaps;
    {
        std::lock_guard lock(gSwapMutex);
        ++gSwapsPending;
    }
    gSwapWake.notify_one();
}

}  // namespace s3n::render
