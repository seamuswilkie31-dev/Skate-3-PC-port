#pragma once

// The GPU's output side in Vulkan: its render targets (the 10 MB of EDRAM it draws into),
// the clears and resolves that fill them and copy them out to textures, and the frame
// handed to the window at each swap. Everything here runs on the command processor's
// thread, in the order the GPU would do it, and does nothing without a window.

#include <cstdint>
#include <string>

namespace s3n::render {

// Direct3D's clear: a rectangle list drawn with its own clear shader. The vertices
// (fetch constant 0: x, y, z, then RGBA) give the rectangle, the depth and the colour.
void ClearRectangle();

// A draw in the copy EDRAM mode: copies (resolves) a render target to a texture in
// memory, clearing the render target afterwards if asked.
void Resolve();

// VdSwap's marker: the frame is finished, and the front buffer (a resolved texture) goes
// to the window.
void Swap(uint32_t frontBuffer, uint32_t width, uint32_t height);

// --dump-frames: writes every `interval`th frame to `folder` as a BMP, for checking runs
// without watching them.
void DumpFrames(uint32_t interval, const std::string& folder);

}  // namespace s3n::render
