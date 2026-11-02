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

// Primitives and their state.
constexpr uint32_t kPaScScreenScissorTl = 0x200E;  // signed x (0-14), y (16-30)
constexpr uint32_t kPaScScreenScissorBr = 0x200F;
constexpr uint32_t kPaScWindowScissorTl = 0x2081;  // x (0-13), y (16-29), window offset off (31)
constexpr uint32_t kPaScWindowScissorBr = 0x2082;
constexpr uint32_t kVgtIndxOffset = 0x2102;        // added to every index
constexpr uint32_t kVgtMultiPrimIbResetIndx = 0x2103;
constexpr uint32_t kRbBlendRed = 0x2105;           // blend constant (4 floats)
constexpr uint32_t kRbStencilRefMaskBf = 0x210C;   // back faces, as kRbStencilRefMask
constexpr uint32_t kRbAlphaRef = 0x210E;           // float
constexpr uint32_t kPaClVportXScale = 0x210F;      // then x offset, y scale, y offset, z scale, z offset
constexpr uint32_t kRbBlendControl0 = 0x2201;      // colour src (0-4), op (5-7), dst (8-12); alpha at 16
constexpr uint32_t kRbColorControl = 0x2202;       // alpha function (0-2), alpha test (3)
constexpr uint32_t kPaSuScModeCntl = 0x2205;       // cull front (0), back (1), clockwise front (2),
                                                   // window offset (16), primitive restart (21)
constexpr uint32_t kPaClVteCntl = 0x2206;          // viewport scale/offset enables (0-5)
constexpr uint32_t kPaSuVtxCntl = 0x2302;          // pixel centre (0: Direct3D's, at integers)

constexpr uint32_t kEdramModeColorDepth = 4;
constexpr uint32_t kEdramModeDepth = 5;
constexpr uint32_t kEdramModeCopy = 6;

// Shader constants: 512 float4s (vertex shaders c0-c255, pixel shaders c256-c511), then
// the fetch constants, then 256 booleans (vertex shaders 0-127, pixel shaders 128-255).
constexpr uint32_t kAluConstants = 0x4000;
constexpr uint32_t kPixelAluConstants = 0x4400;

// Fetch constants: 6 dwords per texture, or three 2-dword vertex fetch constants in the
// same space (vertex constant n at kFetchConstants + 2n): address | 3 (dword 0), endian
// (bits 0-1) and size in dwords (2-25) (dword 1).
constexpr uint32_t kFetchConstants = 0x4800;
constexpr uint32_t kBoolConstants = 0x4900;

}  // namespace s3n::gpu::reg
