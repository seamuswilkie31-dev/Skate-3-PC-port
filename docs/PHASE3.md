# Phase 3 — progress

Goal: implement the kernel in the order the game needs it, until the first
frame renders. Steps (from [PHASE2.md](PHASE2.md)): threads, real
synchronization objects, files from the ISO, then whatever the log asks for
next, up to D3D device creation.

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

## Next: files from the ISO

Serve `d:\` (and `game:\`) from the disc image the launcher points at.
`common/xdvdfs` already reads it. That needs:

- `NtCreateFile` / `NtOpenFile` / `NtQueryFullAttributesFile` resolving paths
  on the disc.
- `NtReadFile` with byte offsets, plus the asynchronous form (an event or APC
  on completion).
- `NtQueryInformationFile` (size, position), `NtQueryVolumeInformationFile`
  and `NtQueryDirectoryFile`.
- `NtClose` on file handles.

Then follow the log again: `XGetVideoMode`, `XexLoadImage` /
`XexGetProcedureAddress` and the XAM content and profile functions are the
stubs start-up has touched so far.
