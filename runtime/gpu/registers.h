#pragma once

// GPU register numbers the renderer reads, with the fields it uses.

#include <cstdint>

namespace s3n::gpu::reg {

// Render targets. Bases and sizes are in EDRAM tiles: 80x16 samples of 32 bits (40x16
// of 64 bits), 5120 bytes each, 2048 in all.
constexpr uint32_t kRbSurfaceInfo = 0x2000;  // pitch in pixels (0-13), MSAA (16-17: 1x, 2x, 4x)
constexpr uint32_t kRbColorInfo = 0x2001;    // base (0-11), format (16-19); RT 1-3 at 0x2003-0x2005
constexpr uint32_t kRbDepthInfo = 0x2002;    // base (0-11), format (16: 24-bit float depth)
constexpr uint32_t kRbColor1Info = 0x2003;
constexpr uint32_t kPaScWindowOffset = 0x2080;  // signed x (0-14) and y (16-30) added to positions
constexpr uint32_t kRbColorMask = 0x2104;       // 4 bits (RGBA) per render target
constexpr uint32_t kRbStencilRefMask = 0x210D;  // reference (0-7), mask (8-15), write mask (16-23)
constexpr uint32_t kRbDepthControl = 0x2200;    // stencil (0), depth test (1) and write (2) enables
constexpr uint32_t kRbModeControl = 0x2208;     // EDRAM mode (0-2)
constexpr uint32_t kRbCopyControl = 0x2318;     // source (0-2: RT 0-3, 4 depth), clears (8 colour, 9 depth),
                                                // command (20-21: raw, convert, constant, none)
constexpr uint32_t kRbCopyDestBase = 0x2319;    // physical address
constexpr uint32_t kRbCopyDestPitch = 0x231A;   // pitch (0-13) and height (16-29) in pixels
constexpr uint32_t kRbCopyDestInfo = 0x231B;    // endian (0-2), format (7-12), red/blue swap (24)
constexpr uint32_t kRbDepthClear = 0x231D;      // 24-bit depth (8-31), stencil (0-7)
constexpr uint32_t kRbColorClear = 0x231E;      // packed in the render target's format (ARGB for 8888)

constexpr uint32_t kEdramModeColorDepth = 4;
constexpr uint32_t kEdramModeDepth = 5;
constexpr uint32_t kEdramModeCopy = 6;

// Fetch constants: 6 dwords per texture, or three 2-dword vertex fetch constants in the
// same space (vertex constant n at kFetchConstants + 2n): address | 3 (dword 0), endian
// (bits 0-1) and size in dwords (2-25) (dword 1).
constexpr uint32_t kFetchConstants = 0x4800;

}  // namespace s3n::gpu::reg
