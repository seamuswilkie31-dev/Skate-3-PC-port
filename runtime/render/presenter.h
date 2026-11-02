#pragma once

// The game window and its Vulkan swapchain. The window lives on the main thread, which
// runs the loop: it pumps window events and presents a frame each time the title swaps.
// Until draws are translated, every frame is cleared to black; this is the path rendered
// frames will take.

#include <string>

namespace s3n::render {

// Opens the window and sets up Vulkan. On failure, `error` says why.
bool Init(std::string& error);

// Runs on the main thread until the window is closed.
void Run();

// Called by VdSwap (any thread): the title has finished a frame.
void NotifySwap();

}  // namespace s3n::render
