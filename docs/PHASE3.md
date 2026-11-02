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

## Next

1. **Rendering, step 2: find Direct3D.** Map the game's Direct3D library:
   device creation, render targets and depth buffers, shader creation,
   vertex declarations and buffers, textures and samplers, render states,
   draws, resolves and present. The command processor's packets help here,
   because each function leaves a recognisable trace in the ring buffer.
   Then replace them with Vulkan, starting with what the EA intro and the
   title screen use.
2. **Input** (phase 4): pads through SDL3 (`XamInputGetState`), and keyboard
   and mouse mapped onto the same virtual pad (see the roadmap). That's what
   gets past "press Start".
3. **Audio output** (phase 4): real XMA decoding behind the same context
   interface, and the render driver's frames played through SDL3.
4. **Saves and DLC** (phase 5): the hard drive and cache partitions, the
   content APIs, and importing your save and DLC packages.
