#include "render/vulkan.h"

namespace s3n::render {

Vulkan& Vk() {
    static Vulkan vulkan;
    return vulkan;
}

bool Check(VkResult result, const char* what, std::string& error) {
    if (result == VK_SUCCESS) return true;
    error = std::string(what) + " failed (VkResult " + std::to_string(int(result)) + ")";
    return false;
}

bool CreateImage(Image& image, VkFormat format, uint32_t width, uint32_t height, VkSampleCountFlagBits samples,
                 VkImageUsageFlags usage, VkCommandBuffer cb, std::string& error, uint32_t layers) {
    Vulkan& vk = Vk();
    image.format = format;
    image.width = width;
    image.height = height;
    image.layers = layers;
    image.samples = samples;
    bool depth = format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
    image.aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;

    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = layers;
    if (layers == 6) info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    info.samples = samples;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!Check(vkCreateImage(vk.device, &info, nullptr, &image.image), "vkCreateImage", error)) return false;

    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(vk.device, image.image, &requirements);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = requirements.size;
    alloc.memoryTypeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < vk.memory.memoryTypeCount; ++i) {
        if ((requirements.memoryTypeBits >> i & 1) &&
            (vk.memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            alloc.memoryTypeIndex = i;
            break;
        }
    }
    if (alloc.memoryTypeIndex == UINT32_MAX) {
        error = "no device memory for an image";
        return false;
    }
    if (!Check(vkAllocateMemory(vk.device, &alloc, nullptr, &image.memory), "vkAllocateMemory", error) ||
        !Check(vkBindImageMemory(vk.device, image.image, image.memory, 0), "vkBindImageMemory", error))
        return false;

    if (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_STORAGE_BIT)) {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = image.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format;
        view.subresourceRange = {image.aspect, 0, 1, 0, 1};
        if (!Check(vkCreateImageView(vk.device, &view, nullptr, &image.view), "vkCreateImageView", error)) return false;
    }
    if (cb) Transition(cb, image.image, image.aspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    return true;
}

void DestroyImage(Image& image) {
    VkDevice device = Vk().device;
    if (image.view) vkDestroyImageView(device, image.view, nullptr);
    if (image.image) vkDestroyImage(device, image.image, nullptr);
    if (image.memory) vkFreeMemory(device, image.memory, nullptr);
    image = Image{};
}

void FullBarrier(VkCommandBuffer cb) {
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0,
                         nullptr, 0, nullptr);
}

void Transition(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {aspect, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
}

}  // namespace s3n::render
