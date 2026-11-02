#pragma once

// The game window and its Vulkan swapchain. The window lives on the main thread, which
// runs the loop: it pumps window events and shows each frame the GPU backend finishes,
// scaled to the window.

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>

namespace s3n::render {

struct Image;

// Opens the window and sets up Vulkan (render/vulkan.h). On failure, `error` says why.
bool Init(std::string& error);

// Runs on the main thread until the window is closed.
void Run();

// The handoff from the GPU backend, on the command processor's thread. There are two
// output images, so the backend fills one while the window may still be showing the other.
//
// At a swap: the image to put the finished frame in, `width` x `height`, R8G8B8A8, in the
// GENERAL layout (`cb` gets the transition when it's new), once the window has finished
// with it. nullptr when there's no window.
Image* NextOutput(uint32_t width, uint32_t height, VkCommandBuffer cb);

// Then, with the commands that fill it submitted: the window shows `output` once
// `timeline` reaches `ready`.
void ShowOutput(Image* output, VkSemaphore timeline, uint64_t ready);

}  // namespace s3n::render
