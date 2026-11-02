# Skate 3's Direct3D: a map

Rendering, step 2. Skate 3 links the Xbox 360 Direct3D library statically
(XDK 2.0.9328). It has no symbols, so the functions below were found by what
they do, then confirmed by counting calls during a 90-second boot.

## How the functions were found

1. **Packets.** Direct3D builds GPU command packet headers from immediates
   in its code. A scan of the recompiled code for type-3 headers (draws,
   shader loads, constant loads, events, waits, indirect buffers) found the
   functions that write each kind of packet. They sit together at
   `0x82B4xxxx`–`0x82B6xxxx`, the library.
2. **Kernel calls.** Functions that call the video kernel functions anchor
   device set-up and presenting.
3. **Counting.** Each candidate is wrapped with a call counter that also
   records its callers. This uses `runtime/hooks`: XenonRecomp emits every
   game function as a weak symbol, so the runtime can stand in for any of
   them and still call the original. The counts below are from a 90-second
   boot to the title screen.

## The library

| Address | Role | Evidence | Calls (90 s) |
|---|---|---|---|
| `82B5DB18` | engine set-up (part of device creation) | calls `VdInitializeEngines`, `VdSetGraphicsInterruptCallback` | 1 |
| `82B4D390` | ring buffer set-up | calls `VdInitializeRingBuffer`, `VdEnableRingBufferRPtrWriteBack` | 1 |
| `82B5A648` | **swap / present** | calls `VdSwap`; called once per frame by the game (`82A52BF8`) | 3,176 (= frames) |
| `82B4C5A8` | kick-off: hands queued commands to the GPU | writes `CP_RB_WPTR` | 39,640 |
| `82B4C9B0` | fence | writes `EVENT_WRITE_SHD` | 15,081 |
| `82B4CEF8` | insert callback: a CPU callback when the GPU gets there | `INTERRUPT` + `WAIT_REG_MEM`; once per swap from `82B5A110` | 9,498 |
| `82B51800` | **draw** (the main one) | `DRAW_INDX`, `EVENT_WRITE_EXT`; sets shaders through `82B5C640` | 117,726 |
| `82B55668` | **draw** with a full state flush | many packet types; calls `82B54F48` → `82B54830` | 61,806 |
| `82B54830` | draw (inner) | `DRAW_INDX`, `IM_LOAD_IMMEDIATE` | 57,357 |
| `82B521B0` | **draw** | `DRAW_INDX`; sets shaders through `82B5C640` | 42,734 |
| `82B53D40` | draw (inner, from `82B54028`) | `DRAW_INDX`, `DRAW_INDX_2` | 41,877 |
| `82B525A8` | **draw** | `DRAW_INDX`; sets shaders through `82B5C640` | 1,914 |
| `82B5C640` | **set shaders** for a draw | `IM_LOAD` + two `LOAD_ALU_CONSTANT`s (vertex and pixel constants) | 162,374 |
| `82B5C3C0` | load shader constants | `LOAD_ALU_CONSTANT` | 324,748 |
| `82B4DF10` | set constants / state | `SET_CONSTANT`; mostly from `82B50738` | 350,508 |
| `82B5B0F0` | set constants | `SET_CONSTANT` | 82,753 |
| `82B56F08` | **run a recorded command buffer** | `INDIRECT_BUFFER`; called by the game's render passes | 8,240 |

The draw functions called by the game (`82B51800`, `82B521B0`, `82B525A8`,
`82B55668`) are the device's draw entry points. Their callers are the game's
renderer at `0x827Cxxxx`–`0x827Exxxx` and `0x82A4xxxx`. Other library
functions that write draw packets weren't called during boot: `82B53508`,
`82B533B0`, `82B51C98`, `82B629B8`, `82B62B10`, `82B633C8`, `82B68D18`.

## Outside the library

Not all of Skate 3's GPU work goes through the library's draw functions:

- **Recorded command buffers.** The game records command buffers and replays
  them through `82B56F08`, from `827D12A0`, `827DB4F4`, `827D47C8` and
  `827DB700`: about 8,200 replays in 90 seconds. A replay re-runs packets
  that were built earlier, so the draws inside it never pass through a draw
  function at replay time.
- **Packets written by the game.** `827BD490` (game code) writes
  `DRAW_INDX_2` packets itself, about 9,500 times. A second path at
  `0x82D8xxxx` (`82D8C1C0`, `82D89368`) runs its own indirect buffers and
  draws through `82BB9410`, about 2,000 times. Its timing fits the movie
  player or the UI.
- **State.** As in all Xbox 360 Direct3D, most state setters are inlined into
  game code. They write a shadow copy of the GPU's registers in the device
  and mark it dirty, and the draw functions turn that into packets.

In total, the command processor counted about 464,000 draw packets in those 90
seconds, more than the library's draw functions emitted.

## Shaders as the GPU gets them

- **Bound vertex shaders.** Before a vertex shader reaches the GPU, Direct3D
  binds it to the vertex declaration. It writes the fetch constant, prefetch
  count, format, stride and offset into each vertex fetch instruction; the
  container's vertex elements say which instructions those are. The runtime
  identifies shaders by a key that ignores those fields (PHASE3.md,
  rendering step 3).
- **Direct3D's own shaders.** Three vertex shaders have no container in
  `shaders_final.big`:
  - one for the rectangle lists drawn by `82B55668` (float2 positions);
  - one for the rectangle lists drawn by `82B53D40`;
  - one for the point lists inside recorded command buffers.
  The rectangle lists look like clears and copies.

## What this means for the renderer

There are two ways to drive Vulkan.

**A. At the packet level (recommended).** Translate what the command
processor sees: the register state at each draw packet, the shaders it
loads, render targets, resolves and textures, as `runtime/gpu` already
partly does.
- **Coverage:** it sees every draw, whether it comes from the library, a
  replayed command buffer or the game's own packets.
- **Shaders:** the 391 translated shaders still apply. A shader load names
  the microcode, which matches the code in its container once the fields
  Direct3D binds are ignored, so it leads to the SPIR-V.
- **Cost:** the GPU's render targets (EDRAM) and resolves have to be
  modelled, and resolution scaling has to be built into that model.

**B. At the Direct3D level** (the roadmap's original plan). Replace the draw
functions with Vulkan versions that read the device's state.
- **Gain:** render targets are native, which makes higher resolutions and
  frame rates simpler.
- **Cost:** recorded command buffers and the game's own packets need
  separate handling, by recording or decoding them. The device state the
  draws read is a register shadow anyway, so a lot of the packet-level work
  comes back.

Approach A is recommended: it covers everything Skate 3 does with one
mechanism. The library map above still matters for it, because the swap,
fence and callback functions are where frame pacing, the 60/120/240 fps work
and resolution changes hook in.

## Tools

- `runtime/hooks/hooks.h`: `S3N_COUNT_CALLS(address, label)` wraps a game
  function with a counter. `S3N_GUEST_ORIGINAL(address)` declares its
  original body, for replacing the function outright.
- `runtime/hooks/d3d_probes.cpp`: the counters behind the table above. They
  appear in every run report under "call counts".
