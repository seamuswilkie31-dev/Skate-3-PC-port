# Phase 2 — results

Goal (from the roadmap): the game's entry point runs natively and we see its
first kernel calls. **Done**: see "The runtime" below.

## Done

- **Tooling.** `scripts/setup_tools.ps1` builds XenonRecomp + XenonAnalyse
  (pinned `ddd128b`) with clang-cl into `%LOCALAPPDATA%\Skate3Native\tools`.
- **Our address finder.** `recomp/tools/xexscan` finds the 10 addresses
  XenonRecomp can't detect itself. On Skate 3 every save/restore helper
  pattern matched exactly once, and the spacing between them matches the
  expected layout (GPR save/restore 0x50 apart, VMX 0x94 apart).
  - `setjmp` = `0x82F44E40`: stores f14–f31, r13–r31 into the buffer at r3.
  - `longjmp` = `0x82F25260`: reloads r14/r15 from those same slots (offset
    160) and calls `RtlUnwind`. XenonRecomp's README hint ("look for calls to
    RtlUnwind") gives three candidates here; matching the jmp_buf layout picks
    the right one.
- **Pipeline.** `scripts/recomp.ps1` extracts `default.xex` from your ISO,
  checks its sha1, runs xexscan and XenonRecomp. Output goes to
  `%LOCALAPPDATA%\Skate3Native\recomp\out` (not in the repo).

- **Jump tables.** `xexscan --generate` replaces XenonAnalyse, which found
  none: it matches `lis addi rlwinm lwzx`, while Skate 3's compiler schedules
  `lis rlwinm addi lwzx`. Of the **1,550** `bctr` sites, **354** sit behind a
  bounds check (`cmplwi` + `bgt`/`bgtlr`) and are switches; xexscan resolves
  **all 354** (8,644 cases). The other 1,196 are vtable or function-pointer
  tail calls, which the runtime's indirect-call table handles.
  - Instead of matching fixed instruction orders, xexscan simulates the few
    instructions between the bounds check and the `bctr` (constants, the
    scaled case number, table loads), so any scheduling works, along with word
    tables and computed byte/halfword-offset tables (`lbzx` → `<<2` → `+base`).
    It refuses to guess on an instruction it can't model after the check.
  - 35 of the switches live in **leaf functions**, which have no `.pdata`
    entry. XenonRecomp's analysis ends a function at its first `bctr`, which
    would cut their case blocks off into fake functions, so xexscan also
    writes explicit boundaries for them (`skate3_generated.toml`). It measures
    each one by following its control flow through every case. Its entry is
    either the nearest `bl` target, or the nearest instruction after an
    unconditional end that no branch or case targets.
  - Spot checks: leaf `0x824CCE68` spans exactly from the previous function's
    final `b` to the next function's `mflr r12` prologue; computed table
    `0x82F2DBA4` has 12 labels, all inside its function.
  - Result: the generated C++ has 354 `switch` statements and XenonRecomp
    reports no case jumping outside its function. E.g. `sub_823C2528` gets
    `switch (ctx.r11.u32) { case 0: goto loc_823C2718; ... }`, matching the
    table words after the `bctr`.
  - **Unguarded switches** (found by the first runtime runs). Some switches
    have no bounds check at all: the compiler knew the index was in range.
    `sub_82AEBC80` scales its argument r5 and jumps through a 13-entry table
    right after the `bctr`; without a table XenonRecomp emitted an indirect
    *function* call to the case address, which crashed. xexscan now also
    simulates `bctr` sites without a check, treating every register as an
    input, and accepts only an inline word table (starting right after the
    `bctr`, ending exactly at the lowest case address) indexed by a register
    that still holds the case number. Function-pointer arrays live in data,
    so they can't match. **21** more switches (1,009 cases), for **375**
    tables and 43 leaf-function boundaries in all.

- **Missing instructions.** `recomp/xenonrecomp-patches/0001-skate3-instructions.patch`
  (applied by `setup_tools.ps1`) adds the **39** instruction forms Skate 3
  uses that XenonRecomp lacked, at **5,327** sites. XenonRecomp now reports
  none unrecognized for Skate 3. The forms: `bdzf` (1,966 sites; the compiler's
  dense-switch idiom `mtctr idx; cmpwi; bdzf; bdzf; ...`), the update-form
  loads/stores (`stfsu`, `lfsu`, `sthu`, `lhzu`, `lhau`, `lfdu`, `stfdu`,
  `lbzux`, `lhzux`, `lwzux`, `lfsux`, `stbux`, `sthux`, `stfsux`, `stdux`),
  carry/logic ops (`addc`, `addme`, `subfze`, `eqv`, `mulhd`, `mulhdu`),
  `bdnzt`, `lvehx`, and the vector ops (`vsel128`, `vcfpuxws128`, `vaddsws`,
  `vpkuwus128`, `vsubuwm`, `vsl`, `vpkuwum128`, `vpkswss128`, `vsububm`,
  `vnor128`, `vcmpgtuw`, `vpkuhum128`, `vrlw128`, `vspltish`).
  The same patch fixes four existing problems:
  - **Unknown instructions were silently dropped**: the generated code only had
    a comment, so those sites would have done nothing at runtime. They now
    emit a trap, so any future gap fails loudly.
  - **`vcmpbfp`/`vcmpbfp128` were stubs** that trapped at runtime (14 Skate 3
    sites, including 8 record forms). Now implemented, CR6 included.
  - **`bdnzf` ignored its condition bit** and always tested `eq` (a shortcut
    that held for Sonic Unleashed). It now tests the bit it names.
  - **Switches tested all 64 bits** of the index while the game's bounds check
    (`cmplwi`) looks at 32; junk in the upper half would have hit
    `__builtin_unreachable()`. They now switch on `.u32`.
- **Instruction tests.** `scripts/test_recomp.ps1` generates 81 tests (228
  checks) from `recomp/tests/isa_tests.py` and runs them through XenonRecomp's
  own test mode. The expected values come from a Python model written from the
  PowerPC/VMX128 definitions, not from the C++. A deliberate "canary" failure
  proves on every run that the harness reports failures. A mutation run
  (breaking `subfze`'s carry, `vsl`, and the unsigned float conversion in the
  generated code) made exactly the affected tests fail.

- **D3D half-float packs and `blrl`.** `0002-d3d-half-packs-and-blrl.patch`
  covers the last 11 sites that would have trapped or misbehaved: 3 ×
  `vupkd3d128` type 5 (unpack four half-floats), 1 × `vpkd3d128` type 3 pack 1
  (two half-floats), 6 × `vpkd3d128` type 5 pack 3, and 1 × `blrl` (call
  through LR) in the runtime library's exception code. **Skate 3's generated
  code now has zero traps.**
  - Behaviour follows Xenia's implementation, used as a reference only (our
    code is our own). A pack converts to half floats, places `x:y` in packed
    word 3 (`float16_2`) or `x:y, z:w` in words 2–3 (`float16_4`), then a
    small pack-mode × shift table picks which destination words receive
    them; the rest keep their old value. Unpack `float16_4` reads guest
    halfwords 4–7; `float16_2` reads 6–7 and fills in `0.0, 1.0`.
  - The 6 "unexpected" `float16_4` packs were in fact already correct: pack
    modes 2 and 3 place data identically for shifts 0–2, and XenonRecomp
    only warned because it checked for mode 2. The new code handles every
    mode and shift through one table, for both half-float types.
  - Half floats follow the Xbox format, which has no INF/NaN. Packing
    truncates and saturates NaN or |x| > 65504 to `0x7FFF` (what
    XenonRecomp already did). Unpacking treats exponent 31 as an ordinary
    exponent (65536–131008), where Xenia uses IEEE conversion for speed
    (INF/NaN). This only matters for values outside the IEEE half range.
  - Tests: 29 more cases, covering every pack mode × shift for both types,
    the unpacks, and aliasing (source = destination), for 110 tests / 344
    checks in all. A mutation run (unpack reading the wrong halves, pack
    swapping x and y) failed exactly the tests that use those words.
- **Data block at the end of .text.** The 487 undecodable words were not the
  import table (an earlier guess). They're a data block right after the last
  import thunk: the word `0x00485645` (`"\0HVE"`) at `0x82F72D20`, a 32-byte
  header (including `0x8200`, the image base's upper half), then a count
  (483), one more word, and 483 image-relative code addresses, ending at
  `0x82F734D4` (`0x7B4` bytes in all). It's the same structure XenonRecomp's
  README skips in its own example config (`size = 44` there = one entry).
  - XenonRecomp's gap analysis had turned it into **492 fake one-word
    functions** (`sub_82F72D20` … `sub_82F734D0`), which would also have
    landed in the runtime's indirect-call table.
  - `xexscan --generate` now finds the marker, reads the count and emits
    `invalid_instructions = [{ data = 0x00485645, size = 0x7B4 }]` into
    `skate3_generated.toml`, so the size comes from the data, not a constant.
  - The table's entries are function entry points (354 distinct; mostly tiny
    leaf functions that store a constant pointer, likely a vtable, into an
    object). Only 77 are known from `.pdata` or `bl`, so the runtime will
    need all of them as indirect-call targets; all 354 are present as
    recompiled functions.
  - Result: XenonRecomp's log is now **empty** (no undecodable words, no
    unrecognized instructions, no warnings). Exactly the 492 fake functions
    disappeared (70,747 → 70,255) and nothing else changed.

**Phase 2's translation side is done**: every instruction in Skate 3 is
recognized, every switch has its table, and the generated code has no traps.

## The runtime (`runtime/`, `s3native.exe`)

**Phase 2's exit criterion is met: Skate 3's entry point runs natively and
its kernel calls are logged.**

- **Build.** `scripts/build.ps1` (Ninja + clang-cl) compiles the 276
  generated files in about two minutes on 8 parallel jobs; `s3native.exe` is
  48 MB. Generated code needs `-fno-strict-aliasing` and `-march=sandybridge`
  (SSE4/AVX, no FMA). The whole project now uses the static CRT, like
  XenonRecomp's libraries.
- **Guest memory.** The 4 GB guest space is reserved; only what the runtime
  hands out is committed, so stray accesses fault and get reported. Regions:
  the image at `0x82000000`, the indirect-call table after it, title virtual
  memory from `0x40000000`, runtime objects at `0x7F000000`, and 512 MB of
  physical memory (first-fit with merging, from 1 MB up) seen through the
  console's three windows (`0xA0000000` 64 KB pages, `0xC0000000` 16 MB,
  `0xE0000000` 4 KB).
- **Loader.** XenonUtils decrypts and decompresses `default.xex`; the image is
  copied into guest memory and the indirect-call table is filled from
  `PPCFuncMappings` (70,541 functions). Import slots: XenonUtils turns
  function thunks into `__imp__<Name>` symbols; the runtime points each
  function's address slot at its thunk and gives the 12 imported kernel
  variables (`XboxHardwareInfo`, `KeTimeStampBundle`, `XboxKrnlVersion`, …)
  zeroed storage, named through XenonUtils' export tables.
- **Main thread.** Stack, a TLS block (static TLS from the XEX template, then
  the dynamic slots right after it), a thread object, and a processor block
  in r13 with the fields Xbox code reads (TLS pointer, self, stack bounds,
  current thread). It runs on a host thread with a 256 MB stack.
- **Imports.** `gen_import_stubs.py` writes a logging stub (returns 0) for
  every one of the 286 imported functions not implemented in
  `runtime/kernel/`. Implemented so far, because start-up needed them:
  `NtAllocateVirtualMemory`, `NtFreeVirtualMemory`,
  `MmAllocatePhysicalMemoryEx`, `MmFreePhysicalMemory`,
  `MmGetPhysicalAddress`, `MmQueryStatistics`, `KeTlsAlloc/Free/GetValue/
  SetValue`, `NtCreateMutant/Semaphore/Event` (handles only) and
  `NtWaitForSingleObjectEx` (succeeds at once).
- **Diagnostics.** Every kernel call is traced (first 8 per import in full),
  guest faults, traps and host crashes are caught, and a watchdog ends stalled
  or over-long runs; each run ends with the reason, the main thread's
  registers, the last 16 calls and a per-import summary.

### How far the game gets

| Run | What changed | Result |
|---|---|---|
| 1 | all imports stubbed | 2 calls (`NtAllocateVirtualMemory` reserve + commit), then a write to `0x4C`: the stub never returned an address |
| 2 | real virtual memory | 53 calls: CRT start-up (critical sections, TLS, strings, mutex, semaphore), then a null read |
| 3 | TLS slots + object handles | 20 imports, then `MmAllocatePhysicalMemoryEx` 1.5 billion times: the stub returned NULL |
| 4–5 | physical memory, statistics, a freeing allocator | the game probes the largest block (511 MB), frees it and takes 475 MB for its heap; then an indirect call to a case label: the unguarded switch above |
| 6 | 21 unguarded switches | **35 imports**: video mode, license mask, thread priorities, events, a timer, three `ExCreateThread`, file-system cache; then the main thread polls `KeDelayExecutionThread` forever, because the stubbed `ExCreateThread` never starts the workers it waits for |

## Phase 3, first steps

1. **Threads.** `ExCreateThread` running guest threads on host threads (each
   with its own context, stack, TLS block and processor block), and
   `KeDelayExecutionThread` actually sleeping.
2. **Real synchronization objects** behind the handles: events, semaphores,
   mutants, timers, critical sections, and waits that block.
3. **Files.** Serve `game:\` from the ISO (common/xdvdfs already reads it).
4. Then follow the log: the next stubs the game depends on, up to the D3D
   device creation that starts the renderer work.
