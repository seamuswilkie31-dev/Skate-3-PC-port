#pragma once

// The Vulkan device: the window (presenter.cpp) creates it on the main thread, and the
// GPU backend (targets.cpp) records and submits on the command processor's thread.

#include <vulkan/vulkan.h>

#include <cstdint>
#include <mutex>
#include <string>

namespace s3n::render {

struct Vulkan {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;
    std::mutex queueMutex;  // the queue is shared by both threads
    VkPhysicalDeviceMemoryProperties memory{};
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;  // for the GPU's 24-bit depth + 8-bit stencil
};
Vulkan& Vk();

bool Check(VkResult result, const char* what, std::string& error);

// An image with its own memory, and a view of it when it's a render target.
struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = 0;
    uint32_t width = 0, height = 0;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
};

// Creates the image in VK_IMAGE_LAYOUT_UNDEFINED; `cb` (if given) moves it to GENERAL,
// the layout every image here stays in.
bool CreateImage(Image& image, VkFormat format, uint32_t width, uint32_t height, VkSampleCountFlagBits samples,
                 VkImageUsageFlags usage, VkCommandBuffer cb, std::string& error);
void DestroyImage(Image& image);

// Makes all earlier work in the command buffer visible to all later work. Simple rather
// than fast; finer barriers come when there's real rendering to measure.
void FullBarrier(VkCommandBuffer cb);

void Transition(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to);

}  // namespace s3n::render
