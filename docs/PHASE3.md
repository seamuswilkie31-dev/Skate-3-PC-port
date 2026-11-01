# Phase 3 — progress

Goal: implement the kernel in the order the game needs it, until the first
frame renders. Steps (from [PHASE2.md](PHASE2.md)): threads, real
synchronization objects, files from the ISO, then whatever the log asks for
next, up to D3D device creation. All of those are done; start-up now stops
at the GPU itself (see the end of this page).

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

## Next: the GPU front end

This is where the renderer work starts:

1. **GPU registers.** Map the register window at `0x7FC80000`.
2. **Ring buffer.** A command processor thread that watches the write
   pointer, reads the command packets the game's D3D library writes, and
   advances the read pointer write-back so the game sees its commands
   consumed. A first version can execute nothing and only log the packet
   types.
3. **Video.** Real answers from `VdQueryVideoMode`, `XGetVideoMode` and
   friends (1280×720, 60 Hz), and the graphics interrupt (vertical blank and
   swap) called at 60 Hz on the game's callback.
4. Then the packets themselves: draw calls, shaders (XenosRecomp) and the
   Vulkan backend.
