# Phase 3 — progress

Goal: implement the kernel in the order the game needs it, until the first
frame renders. Steps (from [PHASE2.md](PHASE2.md)): threads, real
synchronization objects, files from the ISO, then whatever the log asks for
next, up to D3D device creation. All of those are done, and so is the GPU's
front end: the game now runs its boot sequence, presenting frames to a GPU
that doesn't draw yet (see the end of this page).

## Done: threads and synchronization

Skate 3's start-up now runs on **13 guest threads**, each named by the game:

| Thread | Name | What it does at start-up |
|---|---|---|
| t1 | (main) | start-up, then loads the first data files |
| t2 | `load_thread` | waits for load requests |
| t3 | `timer_thread` | wakes on a periodic kernel timer and sets an event each tick |
| t4, t5 | `rwfilesys` | file-system workers, wait for requests |
| t6 | `presence_thread` | Xbox Live presence |
| t7 | (unnamed) | polls with `KeDelayExecutionThread` |
| t8 | (unnamed) | the network thread, waits on WSA events |
| t9–t11 | `Job Manager - Job Thread` | job system workers |
| t12 | `dlc_enumerator` | downloadable content |
| t13 | `decompress_thread` | decompression worker |

How it works (`runtime/kernel/`):

- **Threads** (`threads.cpp`). `ExCreateThread` runs each guest thread on a
  host thread with a 256 MB stack reservation, so deep recursion in recompiled
  code has room. The guest side lives in one block in the stack range
  (`0x70000000`–`0x7F000000`): the stack, a processor block in r13 and the TLS
  block. The KTHREAD is a kernel object with the fields title code reads:
  stack bounds, TLS, thread ID, start address, processor, create/exit time and
  exit status. With an XAPI startup routine the thread runs
  `startup(start, context)`, otherwise `start(context)`. Suspended creation,
  `NtResumeThread`, `ExTerminateThread`, priorities (mapped to host priorities
  within ±2), affinity (recorded only) and `KeDelayExecutionThread` (a real
  sleep) all work.
- **Kernel objects and waits** (`dispatcher.cpp`). Every waitable object is a
  dispatcher header in guest memory, and the signal state there is the real
  state, so title code that sets up or reads objects inline agrees with the
  kernel functions. One dispatcher lock guards state changes and one condition
  variable wakes waiters. Events (notification and synchronization),
  semaphores with limits, recursive mutants with owners, named objects
  (`OBJECT_ATTRIBUTES` names, opening the existing object), handles with
  reference counting, the current-thread pseudo-handle, `NtDuplicateObject`,
  `Ob*Reference*`, wait-any and wait-all with NT timeouts, and user APCs
  delivered in alertable waits.
- **Critical sections and spinlocks** (`locks.cpp`). The console's 28-byte
  `RTL_CRITICAL_SECTION` layout. Taking a free lock is one atomic operation on
  guest memory, so it's compatible with title code using the same structure;
  contention waits on the embedded event. Spin counts are honoured.
- **Time** (`time.cpp`). A clock thread refreshes `KeTimeStampBundle` every
  millisecond and fires kernel timers: it signals them, queues their APC
  routine and re-arms periodic ones. The game's `timer_thread` runs off one.
  `KeQueryPerformanceFrequency` reports the host TSC rate, because XenonRecomp
  compiles `mftb` as `rdtsc`.
- **Smaller pieces**: `RtlInitAnsiString`, WSA events (`NetDll_WSA*Event*`;
  the network thread now blocks instead of spinning), `XamNotifyCreateListener`.
  `RtlRaiseException` recognises the SetThreadName exception the game raises
  for every new thread. Any other exception ends the run with a report,
  because structured exception handling isn't implemented.

### Memory layout change

The runtime's own objects (kernel objects, kernel variables) moved from
`0x7F000000` to just above the first 64 KB, where the console keeps them.
The first 64 KB stay unmapped, so real null pointers still fault.

This was needed for start-up to get past one function. Its init code
(`0x826B57D0`) reads a 64-bit field at offset `0x12160` through a global
pointer that is only set later in the same function. On the console the read
lands in mapped memory and is harmless; here it faulted. A hardware write
watch (below) confirmed nothing sets the pointer before the read. 4 KB-page
`NtAllocateVirtualMemory` allocations now come from the low range too, as on
the console; 64 KB pages stay at `0x40000000`.

### Diagnostics added

- **Per-thread reports.** Each run ends with every thread's name, entry
  point and state (running, waiting in which call on which object and for how
  long, suspended, exited), plus its `lr`, `r1` and `r3`.
- **Fault locations.** A fault names the recompiled function it happened in,
  matched exactly through the x64 unwind tables. It also gives the faulting
  thread's guest registers.
- **`--watch <hex address>`** logs every write to one guest word, with the
  writing thread, the function and its registers. It uses an x64 hardware
  breakpoint in every guest thread, so it catches inline stores and atomics as
  well as kernel writes.
- **Lock-free tracing.** Every guest thread passes through the tracer on every
  kernel call, so it no longer takes a lock. Log lines carry the thread
  number, plus `r7`.

### How far the game gets

| Run | What changed | Result |
|---|---|---|
| 7 | real threads, waits, critical sections, timers | 8 threads start; 48 imports; the main thread opens its first file through a stub and crashes on a null pointer |
| 8 | thread names, WSA events, notification listener | 13 threads; the main thread faults reading `null + 0x12160` |
| 9 | runtime objects moved to low memory | past that read; the timer thread ticks; the main thread opens 6 files, gets nothing, and dereferences `null + 4` in `0x82B43318` |
| 10 | file opens fail honestly and log their path | the same stop, now with the list of files it needs |

The files the game asks for first, all on the disc (`d:`):

```
d:\fileserver.ini
d:\data\big\miscboot.big
d:\data\big\db.big
d:\data\big\fedynamic.big
d:\data\db\skaterschema.vlt
d:\data\db\skatercollections.vlt
```

## Done: files from the disc

The game's data now comes from your copy: an ISO image or an extracted
folder, chosen in the launcher (`config.ini`) or passed as `--game <path>`.

- **The disc** (`runtime/disc.cpp`). Indexed once when mounted: 103 files,
  5.97 GiB for the retail disc. Reads are positional host reads, safe from any
  number of guest threads. ISO images use XDVDFS through `common/xdvdfs`.
  Extracted folders read the files directly.
- **Paths** (`kernel/file_imports.cpp`). `d:\`, `game:\` and `\Device\Cdrom0`
  are the disc. Symbolic links from `ObCreateSymbolicLink` are followed, and
  names relative to an open directory handle work. Other devices (the hard
  disk, cache partitions) aren't served yet; opens there fail with "path not
  found" and are logged.
- **Opens** honour the console's dispositions and directory/non-directory
  options, and answer like a read-only disc: creating or overwriting fails
  with "media write protected". Every open is logged with its outcome.
- **Reads.** `NtReadFile` and `NtReadFileScatter` (a list of 4 KB pages)
  support explicit byte offsets or the file pointer, and report end of file.
  They complete before returning, then signal the caller's event and queue
  its APC, so asynchronous reads also work.
- **Queries.** `NtQueryInformationFile` (basic, standard, internal, position,
  end of file, network open), `NtSetInformationFile` (position),
  `NtQueryFullAttributesFile`, `NtQueryVolumeInformationFile` (volume, size,
  device, attributes: a read-only `GDFX` CD-ROM volume) and
  `NtQueryDirectoryFile` (one entry per call, `*`/`?` masks, restart).

### The console's 50 MHz timebase

With files loading, the game's timer thread started spinning. It sets a
60 Hz timer (16.7 ms), then re-arms it by subtracting the time its work took,
measured with `mftb`. XenonRecomp compiled `mftb` as `rdtsc`, so the work
looked about 70 times longer than it was (3.4 GHz instead of 50 MHz), and
every new due time was already in the past.

The fix is our XenonRecomp patch `0003-console-timebase.patch`: `mftb` now
calls the runtime's `PPCQueryTimebase()`, which scales the host's TSC to the
console's 50 MHz, and `KeQueryPerformanceFrequency` reports 50 MHz to match.
The timer now ticks at a steady 60 Hz. The same fix matters later for frame
timing (phase 6).

### How far the game gets

| Run | What changed | Result |
|---|---|---|
| 11 | files served from the ISO | reads `miscboot.big`, `db.big` and `fedynamic.big`; `d:\fileserver.ini` (a development file) is correctly missing; the timer thread spins |
| 12 | 50 MHz timebase | 14 threads, 65 imports. The game parses its archives (130,000 critical-section lock pairs in 0.3 s: its allocator), sets up Direct3D, then writes to the GPU's registers at `0x7FC80714` |
| 13 | extracted folder instead of the ISO | the same files and the same stop |

The last run's Direct3D set-up, in order: `VdInitializeEngines`,
`VdSetGraphicsInterruptCallback` (callback `0x82B4D118`),
`VdSetSystemCommandBufferGpuIdentifierAddress`, `VdInitializeRingBuffer` (at
physical `0x1DC02000`), `VdEnableRingBufferRPtrWriteBack` (at `0x1DC0B03C`),
`VdQueryVideoMode`, `VdQueryVideoFlags`, `VdCallGraphicsNotificationRoutines`
and `VdGetCurrentDisplayGamma`, all still stubs. Then it writes the command
processor's ring-buffer write pointer (register `0x1C5`, `CP_RB_WPTR`) through
the GPU's memory-mapped registers at `0x7FC80000`, which aren't mapped yet.

## Done: the GPU front end

The game now runs its boot sequence for about 32 seconds. It presents about
1,425 frames and queues about 222,000 draw calls, which are counted but not
drawn yet. It reads most of its data and starts decoding the EA intro movie.

- **Physical memory is aliased** (`runtime/memory.cpp`). The console sees its
  512 MB through three windows (`0xA0000000`, `0xC0000000`, `0xE0000000`).
  The game writes GPU data through one and the GPU reads it by physical
  address. The guest space is now one placeholder reservation (Windows 10
  1803+), and each window is a view of the same 512 MB section.
- **The 4 KB window is one page off.** The game's Direct3D converts CPU to
  GPU addresses inline, and for the `0xE0000000` window it adds 4 KB: on the
  console, `0xE0000000 + x` is physical `x + 0x1000`. Windows can't map a
  view at a 4 KB offset (views are placed on 64 KB boundaries), so the shift
  is modelled instead:
  - `MmGetPhysicalAddress` adds the shift.
  - 4 KB-window allocations keep a spare page after them, so shifted ranges
    never collide with other allocations.
  - The GPU shifts addresses back when they fall inside such an allocation.

  Without this, the game's interrupt record and the GPU's scratch area,
  which are the same memory on the console, were different pages, and the
  first GPU interrupt was never acknowledged.
- **The register window** (`0x7FC80000`, 64 KB) is ordinary memory that the
  command processor shares with the CPU:
  - The CPU's write to `CP_RB_WPTR` is what the command processor polls for.
  - Scratch registers enabled in `SCRATCH_UMSK` are mirrored to `SCRATCH_ADDR`
    on every write, the GPU's or the CPU's.
  - `COHER_STATUS_HOST` flushes complete as soon as they're waited on.
  - The vertical-blank bit in the display interrupt status is set before each
    vertical-blank interrupt; the game's handler checks it.
- **The command processor** (`runtime/gpu/gpu.cpp`) is a host thread that
  consumes the ring buffer and indirect buffers and reports its read pointer.
  It carries out every packet that synchronizes with the CPU:
  - register writes and `REG_RMW`;
  - waits: `WAIT_REG_MEM`;
  - memory writes: `MEM_WRITE`, `COND_WRITE`, `REG_TO_MEM`;
  - fences: `EVENT_WRITE_SHD`;
  - screen extents: `EVENT_WRITE_EXT`;
  - constant loads into a full register file;
  - predication through bin masks;
  - `INTERRUPT`.

  Draws and shader loads are counted and skipped.
- **Interrupts** run on a runtime-owned guest thread: one per `INTERRUPT`
  packet and a vertical blank every 1/60 s. Each calls the game's callback
  with the right hardware-thread number in its processor block.
- **Video functions** (`kernel/video_imports.cpp`):
  - The display reports as 1280×720 at 60 Hz, widescreen.
  - `VdSwap` fills its 64 reserved ring dwords with one no-op packet.
  - The scaler command buffer is type-2 fillers.
  - Engine set-up, EDRAM training and display persistence all succeed.

  Also implemented: `RtlFillMemoryUlong`, and `VdHSIOCalibrationLock`, a
  kernel variable that must be a ready critical section.
- **The audio decoder's registers** (`0x7FEA0000`) are mapped as plain memory
  until audio (phase 4), so writes to them don't fault.
- **Diagnostics.** A `gpu:` section in every run report gives the ring state,
  frame and interrupt counts, packet counts by opcode, and any wait in
  progress. `S3N_GPU_TRACE=<n>` logs the first n packets in full.
  `--watch-reads <address>` reports every read of a word as well as every
  write.

### How far the game gets

| Run | What changed | Result |
|---|---|---|
| 14 | registers, ring buffer, interrupts, video | runs the full 60 s: the ring buffer is consumed and 3,594 vertical blanks are delivered; the main thread waits forever on `VdHSIOCalibrationLock` |
| 15 | that lock set up | loads `shaders_final.big`, starts `render_thread`, presents 2 frames; the GPU waits for an interrupt acknowledgement |
| 16 | scaler buffer, `RtlFillMemoryUlong`, scratch mirroring | the same wait: the handler clears its bit one page away from where the GPU looks |
| 17 | the 4 KB window shift | interrupts acknowledged; 12 frames, 56 draws, audio and memory-card threads; then a write to the audio decoder's registers |
| 18–19 | audio registers mapped | about 32 s, about 1,425 frames, about 222,000 draws, 30 archives read, the intro movie's decoder running; then `render_thread` writes through a bad pointer in `0x8247EFC0` |

The last stop: a per-frame buffer object's base pointer (`+0x30`) holds an
address no allocation covers (`0x3FE04178` in one run, different in the
next), so something leaves it uninitialized or overwrites it. Also seen,
both expected for now: the game probes `\Device\Harddisk0\partition0`
(there's no hard drive yet), and one video-memory set-up list has a stated
length that cuts its last packet short, which is harmless and skipped.

## Done: the render-thread crash, fixed with an audio clock

The crash wasn't a memory bug in the runtime. Here is how it was traced:

1. **The object.** A write watch on the buffer object's `+0x30` field showed
   the base pointer being overwritten by the render thread itself, with a
   float.
2. **The overflow.** The object sits right after its own buffer, about
   250 KB of 16-byte records. The 16,000th append ran off the end and over
   the object.
3. **The resets.** A watch on the write offset (`+0xCC`) showed it never
   reset in 32 seconds.
4. **The reads.** A read watch on the first record (new: `--watch-reads`)
   showed nothing ever read it.
5. **What the records are.** Each one is a "set parameter" command: a
   function, an object, a slot and a float. The game builds this list every
   frame and normally executes and resets it.

The list only grew because the game was stuck early in its boot sequence. Its
audio engine had registered with the console's audio render driver
(`XAudioRegisterRenderDriverClient`), which was a placeholder, so the
engine's callback never ran. Everything timed by audio stood still,
including the EA intro movie, which never finished.

`kernel/audio_imports.cpp` now provides that clock. A runtime-owned guest
thread calls each registered client's callback once per 256-sample frame
(48 kHz, about 5.3 ms), passing a pointer to the client's argument word as
the console does. Submitted frames are counted and dropped until audio output
(phase 4). Speaker configuration and category volumes report stereo and full
volume.

With the clock running, the intro movie plays to the end, its decoder
thread exits, and the run reaches the time limit with no crash. Two runs gave
the same result.

### The new stop: the XMA audio decoder

About 26 seconds in, the game's audio core thread (`RwAudioCore Dac`)
starts streaming through the XMA hardware decoder. The `XMA*` functions are
still placeholders, so input buffers never get consumed. The thread then
spins through them, about 900 million calls in 90 seconds, while holding a
lock. The render thread waits on that lock, and frames stop.

## Done: a silent XMA decoder, and boot reaches the title screen

`apu/xma.cpp` and `kernel/xma_imports.cpp` give the game the XMA decoder's
interface:

- **Contexts.** `XMACreateContext` hands out real contexts from a pool of
  256 in physical memory. Its placeholder had returned none, so the game had
  been driving context 0. Each context is 16 big-endian words of bit fields
  in the hardware's layout: the two input buffers with their packet counts
  and valid bits, the input read position in bits, and the output ring of
  256-byte blocks with its read and write offsets.
- **Functions.** The 17 `XMA*` functions the game imports read and set those
  fields. Buffer addresses are stored as physical addresses, as the hardware
  reads them.
- **The decoder.** A thread visits the enabled contexts every millisecond.
  When there's room in the output ring, it writes one 512-sample frame of
  silence (8 blocks in stereo) and moves through the current input buffer at
  4 frames per 2 KB packet. A used-up buffer has its valid bit cleared, and
  the decoder switches to the other one, which is the handshake the game's
  audio core waits for. Output only advances as fast as the game reads it,
  so the game's own audio timing sets the pace.

With it, the audio core streams instead of spinning, the render thread keeps
its lock, and the boot sequence runs through. In two runs of 90 and 120
seconds:
- the game ran without stalling, presenting about 34 frames a second
  (about 4,100 frames and 544,000 draws in two minutes);
- it played the EA intro, finished loading, and then opened
  `Attract_english_ntsc.vp6`, the attract-mode video the game plays when its
  title screen sits idle.

So boot reaches the "press Start" screen and waits for a button no one can
press yet. It also probes the cache partition (`CACHE:\httpcache`), which
isn't served, and carries on without it.

## Rendering, step 1: the window, Vulkan and the shaders

**The window** (`runtime/render/presenter.cpp`) opens on the process's main
thread with SDL3 and drives a Vulkan swapchain (vsync). Every `VdSwap`
presents a frame; until draws are translated it's cleared to black. The
title bar shows the game's frame rate, closing the window ends the run, and
`--no-window` runs without one. On this machine it picked the RTX 5060 Ti and
presented one frame for every game swap (1,792 in 45 seconds).

**The shaders.** `data\big\shaders_final.big` holds all of the game's
shaders: 392 standard Xbox 360 shader containers (261 pixel, 131 vertex).
These are exactly what XenosRecomp reads:
- XenosRecomp turns each container into HLSL, and DXC compiles that to
  SPIR-V.
- **391 of 392 compile.** The one failure samples a 1D texture, which
  XenosRecomp doesn't support yet. It's skipped for now.
- `setup_tools.ps1` now builds XenosRecomp too (pinned to `990d03b`, with
  `recomp/xenosrecomp-patches/0001` making it skip a shader that won't
  compile instead of crashing).
- `scripts/shaders.ps1` runs the whole pipeline from your ISO into
  `<work>\shaders\shader_cache.cpp` (930 KB of compressed SPIR-V, keyed by
  each container's hash).

**The approach.** These results confirm the roadmap's plan: render at the
Direct3D level rather than emulating the GPU's packets. XenonRecomp emits
every game function as a weak symbol, so the runtime can replace any of them
with its own code, and the game's statically linked Direct3D functions can
be swapped for Vulkan implementations one by one. Shaders arrive as the same
containers the cache was built from, so a shader is a hash lookup. The
packet-level GPU front end stays underneath for everything not yet replaced.

## Rendering, step 2: the Direct3D map

[D3D.md](D3D.md) maps Skate 3's statically linked Direct3D library:
- the device set-up, swap, kick-off, fence and callback functions;
- the four draw entry points the game calls;
- the shader and constant uploads behind each draw.

It also covers what bypasses the library: recorded command buffers the game
replays (about 8,200 times in 90 seconds) and two places where game code
writes GPU packets itself. The functions were found by scanning for packet
headers and confirmed with call counters, using a new hook layer
(`runtime/hooks`) that can wrap or replace any recompiled function.

The findings change the recommendation: drive Vulkan from the command
processor (packet level), which sees every draw whichever way it was issued,
rather than replacing Direct3D's draw functions. D3D.md gives both options.

## Rendering, step 3 (first part): shaders at runtime

**The cache.** `recomp/xenosrecomp-patches/0002` makes XenosRecomp also
write `shader_cache.bin`: each translated shader's container, untouched,
with its SPIR-V. `shaders.ps1` puts it in `<work>\shaders`.
- The runtime loads it at start-up (`runtime/render/shader_cache.cpp`).
- The window makes a Vulkan shader module from each one: all 391.
- The device is now created with what XenosRecomp's shaders need: buffer
  device addresses, descriptor indexing and 64-bit integers.

**Which shader is loaded.** The command processor reads the microcode of
every `IM_LOAD` and `IM_LOAD_IMMEDIATE`. A plain hash of it found only 37%
of loads, and no draw had both its shaders found. Comparing loaded microcode
with the containers showed two reasons:
- **Direct3D binds vertex shaders.** When it binds a vertex shader to a vertex
  declaration, it fills in the vertex fetch instructions: fetch constant,
  prefetch count, format, stride and offset. The container leaves those
  fields zero, and the container's vertex elements say which instructions
  get filled.
- **Some shaders carry a trailer.** It's 3 dwords after the last instruction
  (`4E4A0000` and 8 bytes of hash), and the GPU sometimes gets it cleared.

`ShaderKey` deals with both. It walks the shader's control flow, hashes only
the instructions the shader runs, and clears the fields Direct3D binds:
- **391 containers give 249 keys.** Containers that share a key compile to
  identical SPIR-V, so the key is safe.
- **The bound fields aren't wasted.** XenosRecomp reads vertex inputs by
  usage and leaves their formats to the pipeline. So the format, stride and
  offset of each input come from the bound instructions when pipelines are
  built.

**Result** (60-second boot):

| | Before (plain hash) | After (`ShaderKey`) |
|---|---|---|
| Shader loads found | 148,637 of 403,473 | 321,165 of 403,750 (80%) |
| Draws with both shaders found | 0 | 147,407 (18 vertex/pixel pairs) |

Every pixel shader is found. Every other draw uses one of four vertex shaders
that aren't in `shaders_final.big`. The call counters tie each one to the
function that draws with it:

| Key | Size | Draws | Primitives | Drawn by |
|---|---|---|---|---|
| `B0114762…` | 24 dwords | 154,080 | point lists | recorded command buffers: one load per replay (`82B56F08`, 6,420), about 24 draws each; no vertex fetch |
| `B097E2D0…` | 15 dwords | 47,234 | rectangle lists | `82B55668` (one draw per call); fetches float2 positions only |
| `053B88DD…` | 27 dwords | 32,770 | rectangle lists | `82B53D40` (one draw per call); fetches a float3 and a float4 |
| `E8EDE87E…` | 33 dwords | 1,290 | quad lists | the main draw `82B51800`, from game code at `827EE6A8` |

The first three are Direct3D's own shaders. They're bare microcode with no
container, so XenosRecomp has nothing to translate. The rectangle lists look
like clears and copies. The run report now lists shader loads found, draws
by whether their shaders are known, and the unknown shaders with their draw
counts and primitive types.

## Rendering, step 3 (second part): render targets, resolves and the front buffer

**What the GPU does with a frame.** Logging the render-target registers at
every draw of a 60-second boot showed the shape of a frame:
- **The frame you see** is drawn into a 1280×720 target (no MSAA). It's then
  resolved to physical `0x01E68000`, the address in VdSwap's fetch constant:
  a tiled 1280×720 8888 texture, the front buffer.
- **The 3D scene** is drawn at 1152×640 with 2× MSAA, in screen tiles. Each
  tile is resolved out (colour and depth), clearing EDRAM as it goes.
- **Smaller targets** are each resolved to their own texture: 640×360 with
  4× MSAA, 320×180, 80 pixels wide, and 16-bit targets 1600 and 560 wide
  (shadows or effects).
- **Direct3D's own draws:**
  - `82B55668` is Resolve: it draws in the copy EDRAM mode, and three float2
    vertices give the rectangle.
  - `82B53D40` is Clear: a rectangle list whose vertices carry the depth and
    colour. The very first clear is 20% grey.
  - The point lists inside recorded command buffers have no render target
    bound, so they're left alone.

**The backend** (`runtime/render/targets.cpp`) runs on the command
processor's thread:
- **Render targets:** a Vulkan image for each EDRAM base, pitch, MSAA mode
  and format (17 during boot). The GPU has no render-target height, so each
  image covers the EDRAM from its base on, up to 2048 rows.
- **Clears:** Direct3D's clear rectangles and the clears a resolve asks for,
  done as Vulkan attachment clears. These need dynamic rendering, so the
  runtime now requires Vulkan 1.3.
- **Resolves:** each copies (or MSAA-resolves) its rectangle into a texture
  keyed by the destination address, at its screen position. The window
  offset only moves a tile within EDRAM. Depth resolves are skipped for now.
- **Swaps:** VdSwap writes a marker packet with the front buffer's address
  and size. When the command processor reaches it, the backend copies that
  texture into an output image, submits the frame and hands it to the
  window, which scales it to fit. Two output images and timeline semaphores
  keep either thread at most one frame ahead of the other.
- **`--dump-frames n`** writes every nth frame to `<work>\frames` as a BMP,
  so runs can be checked without watching them.

**Result** (60-second boot):

| | Count |
|---|---|
| Resolves | 34,129 (plus 8,633 depth resolves skipped) |
| Direct3D clears | 29,594 |
| Frames | 2,062, every one showing the front buffer |

The frames are still black, and that's expected. Nothing but ordinary draws
writes the 1280×720 target, so it stays black until draws are translated.
To check the chain, that target was temporarily filled with a test colour,
and the colour came out unchanged in the dumped frames. The Khronos
validation layer reports no errors or warnings.

## Rendering, step 3 (third part): draws and textures, and the boot sequence on screen

Every frame of the boot now renders, as it does on the console:
- the language select;
- the Skate 3 logo and copyright screen;
- the EA and Black Box intros;
- the title screen, with its 3D scene and "Press Start to continue";
- the attract-mode video.

**What the game draws.** A census of 100,000 draws showed what the draws use:
- **Viewport:** the viewport transform is always on.
- **Pixel centres:** Direct3D's convention (centres at integers) throughout.
- **Primitives:** almost all triangle lists, triangle strips and quad lists,
  nearly all auto-indexed.

**Draws** (`runtime/render/draws.cpp`), for every draw whose shaders are
both translated:
- **Pipelines:** one Vulkan pipeline per shader pair and register state:
  vertex layout, topology, culling, depth and stencil, blending, colour
  mask, alpha test.
- **Vertices:** converted from guest memory through each vertex fetch as
  Direct3D bound it: format, endian swap, stride and offset. They go in at
  the input locations XenosRecomp gives each usage. Normals, tangents and
  blend indices go in as `uint4`, the rest as `float4`.
- **Indices:** from the index buffer, rebased. Quad lists become pairs of
  triangles.
- **Constants:** both stages' constants (vertex c0–255, pixel c256–511) are
  copied from the registers. The shared constants the shaders read through
  buffer device addresses come with them: texture and sampler slots,
  booleans, the half-pixel offset and the alpha-test threshold.
- **Viewport and scissor:** from the registers, moved by the window offset.
  XenosRecomp compiles vertex shaders with DXC's `-fvk-invert-y`, so the
  viewport is built to undo that inversion. Without that, every frame came
  out upside down.
- **Batching:** draws, clears and resolves share one rendering scope while
  their targets stay the same, and one upload ring per recording
  (`render/backend.h`).

**Textures** (`runtime/render/textures.cpp`) come from the fetch constants,
in bindless arrays:
- **Resolved render targets:** a render target a resolve copied out is
  sampled directly; there were about 32,000 such uses a minute.
- **From memory:** other textures are converted from guest memory: untiled
  (the Xbox 360's 2D tiling) or copied row by row, and the endian swap is
  undone.
- **Formats:** 8888, 565, 1555, 2_10_10_10, 8, 8_8, 16-bit and float
  formats, and DXT1/3/5, DXN and DXT5A as BC1–BC5. The fetch constant's
  swizzle becomes the view's component mapping.
- **Changes:** each texture is checked once a frame, by a hash of its
  memory. The movies' three YUV planes are converted again every frame, and
  the game's own shaders turn them into RGB.
- **Samplers:** from the fetch constant's clamps, filters and border colour.
  Only the base level is converted so far.

**Two more fixes on the way:**
- **Destination swizzle.** Direct3D also rewrites a vertex fetch's
  destination swizzle when it binds a declaration: components the format
  lacks become 0 or 1. `ShaderKey` now ignores that too. That matched the
  last game shader, a textured quad (still 249 keys, still unambiguous).
- **Fast clears.** Direct3D clears the 1280×720 target through a 640×360
  4× MSAA view of the same EDRAM, and the same samples. With a separate
  image per view, the 1280×720 depth buffer was never cleared, and every UI
  quad failed its depth test. A clear now reaches every render target laid
  out over the same tiles, in that target's own pixels.

**Diagnostics:** `--capture-frame n` logs every clear, resolve and draw of
frame n: the shaders, primitive, targets, viewport, scissor, blend state
and textures. That's how the clear problem showed up.

**Result** (60-second boot):

| | Count |
|---|---|
| Draws | 148,449 through 22 pipelines, none skipped |
| Textures from memory | 49, converted again about 1,800 times as the movies play |
| Frame rate | about 37 frames a second, every one presented |
| Validation layer | no errors or warnings |

**Not done yet:**
- **Textures:** mipmaps, 3D and cube textures, and gamma.
- **Resolves:** depth resolves, and EDRAM aliasing beyond clears (copies
  between differently laid-out views).
- **Direct3D's draws:** the point lists in recorded command buffers, and
  draws that use rectangle lists or screen-space positions.
- **Speed:** vertices are converted on the CPU for every draw, and the
  barriers are coarse.

## Rendering in game: the skater editor

With saves working ([PHASE5.md](PHASE5.md)), the new-player flow reaches the
skater editor. Its 3D view had two faults:
- the skater wasn't drawn;
- a white area covered the lower half.

Three fixes:

**1. Direct3D reorders vertex fetches.** The skater's meshes use five skinned
vertex shaders (150–201 dwords), and none matched the cache. Their microcode
matched a container in every instruction but two: the two vertex fetches had
changed places. Direct3D writes a shader's fetches in the order of the vertex
declaration it's bound to (here by offset in the vertex), and that needn't be
the shader's order.
- **`ShaderKey`** now hashes the vertex fetches sorted, each without its
  bound fields, one to each fetch slot. Still 249 keys, still unambiguous.
- **Inputs are matched by register.** An input's fetch is the one, among the
  inputs' slots, that writes the register the container's fetch writes. Mini
  fetches still take their buffer from the full fetch before them, in the
  bound order.
- **Direct3D's clear shader** has a new key, `CBE4133AF97083F3`.

**2. The bound destination swizzle counts.** Once drawn, the skater lay on
its side, with spikes for arms. Its positions and texture coordinates are
16-bit pairs (`SHORT4N`, `SHORT2N`) in a big-endian buffer. The endian swap
puts the first of each pair in the dword's high half, and the GPU takes x
from the low half. So Direct3D binds the fetch with destination swizzle
(y, x, w, z), and the runtime had been using the container's (x, y, z).
- The raw data settled it. Read first-in-memory as x, the first mesh's
  vertices sat at floor height (the shoes) and the second's at about 1.5 m
  (the head). Read the other way, both sat at 0.9 m.
- Vertex conversion now applies the bound swizzle: each fetched component
  (or 0 or 1) goes where the container's swizzle makes the translated shader
  look for it. Packed formats (8_8_8_8, 10_11_11) are bound with the
  identity, so they're unchanged.

**3. Tiled resolves.** The 3D view is drawn at 1152×640 with 2× MSAA, in two
tiles of 320 rows, each resolved out on its own:
- The first tile's resolve goes to `01AC1000`.
- The second's goes to `01C29000`: 320 rows (1152 × 320 × 4 bytes) further
  into the same texture, with the rectangle at EDRAM row 0.
- So the GPU writes a resolve's rectangle at its EDRAM position, from the
  destination address, and Direct3D moves the address for each tile.
- The runtime had made a second texture for the second tile, so the real
  texture's lower half stayed white. A destination address inside a resolve
  texture (same width and format, a whole number of rows in) is now that
  texture, at that row.

**Diagnostics:** `--capture-frame` also logs each draw's vertex inputs:
location, format, offset and stride.

**Result.** The skater editor renders: the skater stands with the board,
textured and lit, in front of its wall. Every screen before it still renders
as before, and the title screen's 3D scene now shows its skater too. In a
200-second run:

| | Count |
|---|---|
| Draws | 625,505 through 51 pipelines, none skipped |
| Unknown shaders | only Direct3D's point-list shader (no render target bound) |

**Not done yet:**
- **Depth resolves** are still skipped. Shadow maps are depth resolved to
  `k_24_8` textures, and those are white for now, so there are no shadows.
- **The world** ("Play Skate 3").

## In the world: first speed work

"Play Skate 3!" now leads into the game. A save that has finished the
new-player flow boots straight to Port Carverton, where the skater rides with
Coach Frank's tips. It rendered at about 3 fps:
- **Converted vertices are cached.** Converting vertices on the CPU took 87 of
  the command processor's 91 s. Runs of 64 vertices or more are now kept by
  their bound layout and range, and checked once a frame against a hash of
  their source memory. Result: about 19 fps in game.
- **WAIT_REG_MEM wakes when the interrupt handler returns.** It used to poll
  with `Sleep(1)`, so each interrupt round trip cost 1.5–1.9 ms; now it costs
  tens of microseconds. The remaining wait is the vsync flip.
- **The ring's read pointer moves per packet**, and past an indirect buffer
  before its commands run, as the GPU's prefetch would move it.

**Diagnostics:** the report gives each thread's CPU time, the command
processor's idle, wait and swap time, and its time in draws by phase.
`--profile-thread n` samples a guest thread's link register. Frame dumps say
when they were taken, for timing `--press`.

## In the world: 30 frames a second

At 19 fps the command processor spent about 36 ms on each frame's draws,
then waited for the vsync flip. Cutting the per-draw work:
- **Vertex conversion** decides each fetch's scaling, valid range and
  swizzle once per draw, then runs one loop per format. Half floats come
  from a table instead of two `ldexp` calls each.
- **Indices** are read by one loop per endian swap and index size. Then a
  pass finds the range and rebases them, with no branches unless primitive
  restart is on.
- **Constants:** each stage's 4 KB is uploaded again only when a constant
  register has been written since, or a new recording has begun.
- **Recording:** pipeline, descriptor sets, viewport, scissor, stencil and
  blend constants are recorded only when they change within a recording.
  The pipeline key hashes 8 bytes at a time.
- **Textures:** a fetch constant binds the same way all frame, so its slots
  are remembered until the next frame or a new resolve image.

**Result:** about 5.7 µs a draw, down from 10.4. The game runs at **30 fps
in Port Carverton**, its own cap (frames are paced by the vsync flip). The
command processor is about half busy in game, so the next frame rate gains
come from lifting that cap (the 60/120/240 fps option), not from here.

**Still slow to load:** the time between screens is the game's own loading.

## The sky: shaders from outside the shader archive

The sky was black. Its pixel shader (`4C2C4E6F…`, 84 dwords, drawn about 4
times a frame) wasn't in `shaders_final.big`, or in any container in memory:
the game hands Direct3D bare microcode. It sits in the game's heap behind an
8-byte hash and a few floats, with no constant table or interpolators.
Translated, it samples a 2D sky texture, lights it against a sun direction,
looks up a 1D ramp, and writes gamma-corrected colour.

**Shader harvesting** (`render/shader_cache.cpp`):
- **Containers in memory.** When a shader key is first found missing, the
  runtime scans the guest's physical memory for shader containers whose code
  the cache lacks. It saves them to `<work>\shaders\source\harvested`.
  XenosRecomp reads that folder with the rest the next time
  `scripts\shaders.ps1` runs.
- **Bare pixel shaders.** When a draw uses a pixel shader the cache lacks with
  a vertex shader it has, the runtime wraps the microcode in a minimal
  container and saves it there too:
  - its inputs are the vertex shader's interpolators, in export order (the
    GPU hands export *i* to pixel register *i*);
  - a constant table names c0–c223 as one array, c224–c255 one register each
    (XenosRecomp's pixel shader arrays stop at c223) and samplers s0–s15, so
    every constant, literals included, comes from the registers.
- **1D textures** (XenosRecomp patch `0003-1d-textures.patch`): a 1D fetch
  samples a one-row 2D texture through the 2D descriptors. The runtime now
  converts 1D textures as one-row 2D ones instead of binding white.
  `scripts\setup_tools.ps1` applies the patch.

**Result:** with the harvested shader in the cache (393 shaders), the sky
renders: blue, with the sun's glow, clouds and the city on the horizon. The
frame capture now also logs each texture's dimension.

**Still white:** cube maps (the environment maps reflective surfaces add,
258 bindings a frame) and depth textures (shadow maps, 278 a frame).

## Cube maps, and samplers again

**Cube maps.** The buildings came out nearly white: their environment maps
were bound as the white stand-in, and reflective surfaces add them on top.
- A cube texture's six faces follow one another in memory, each a 2D slice
  aligned to 4 KB, in Vulkan's own order (+X, −X, +Y, −Y, +Z, −Z).
- They're converted into a six-layer, cube-compatible image, and get a cube
  view in the cube descriptor set the translated shaders sample.

**Samplers.** The texture-binding memo in the speed work lost a line: every
texture was being sampled with the default sampler (the report showed 0
sampler states). Fixed; there are 14 again.

**Result:** the buildings show their textures and windows instead of
washed-out white. What's left is mostly shadows: depth textures (`k_24_8`,
from depth resolves) are still white.

## Depth resolves

Direct3D resolves depth as raw 32-bit texels (destination format 8888), and
the texture fetch decides how they're read. Shadow maps are read as
`k_24_8`, depth in x; the scene's depth is read as 8888 by the post effects.
- **Depth buffers are float** (`D32_SFLOAT_S8_UINT`) where the GPU has it.
  The hardware has a float depth format too.
- **A depth resolve from a single-sample target** copies the rectangle's
  depth through the upload ring (Vulkan copies depth to colour only by way of
  a buffer) into an `R32_SFLOAT` texture, which shaders sample directly.
- **Not right yet:** a texture read as 8888 gets depth, 0, 0, 1 rather than
  the packed depth and stencil bytes; that needs a pass of our own. Depth
  resolves from MSAA targets (the 3D scene's) are still skipped.

**Result:** no texture fetch is unsupported any more, and nothing earlier
changed. Shadows still don't show. The shadow maps are turned into a 16_16
texture by the game's own passes before the world samples it, and that chain
is the next thing to follow.

## Shadows: the depth resolve pass

**The shadow chain.** For each of three cascades the game:
1. renders depth into a 512×512 slice of a 1536-wide surface;
2. resolves it (`00164000`);
3. turns it into two moments in a 16_16 target with a horizontal blur;
4. blurs those vertically into `00564000`, which the world samples.

The first blur reads the depth resolve **as 8888**. It rebuilds 24-bit depth
from three of the bytes (its literals are 255·2¹⁶, 255·2⁸ and 255 over
2²⁴−1) and uses the fourth, the stencil, as a flag. The float texture gave it
depth, 0, 0, 1, so the moments, and the shadows, came out wrong.

**The depth resolve pass** (`render/shaders/depth_resolve.comp`) is the
runtime's own compute shader:
- **Built in:** compiled to SPIR-V by the Vulkan SDK's `glslc` and embedded,
  once as is and once for MSAA depth buffers (sample 0).
- **Two outputs per resolve:** for each texel it writes the raw word the GPU
  would (24-bit normalised depth above 8-bit stencil) as RGBA8 bytes, and the
  depth as a float into a twin R32F image.
- **The fetch picks:** a fetch as `k_24_8` gets the float twin; any other
  format gets the bytes, through the fetch constant's swizzle as usual.
- **Every depth resolve,** single-sample or MSAA, now goes through it. Depth
  buffers are D24S8 again (the pass reads either format as a float).

**Result:** the skater and the pedestrians cast shadows, and no resolve is
skipped. With the Khronos validation layer on, the only report is a warning
that some depth-only passes' pixel shaders write colour.

## Next

1. **Input** (phase 4): done, see [PHASE4.md](PHASE4.md). Start and A now get
   the game past the title screen and its autosave notice, to the storage
   device prompt.
2. **Rendering, continued:** cube maps, depth resolves (shadows), and the
   60/120/240 fps option.
3. **Audio output** (phase 4): real XMA decoding behind the same context
   interface, and the render driver's frames played through SDL3.
4. **Saves and DLC** (phase 5): the hard drive and cache partitions, the
   content APIs, and importing your save and DLC packages.
