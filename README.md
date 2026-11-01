# Skate 3 Native

A native Windows port of the Xbox 360 version of Skate 3: static
recompilation plus a fresh, clean-room reverse engineering effort. It is not a
fork of Skate 3: Recompiled and shares no code with it.

**You need your own copy of the game.** The launcher reads your disc image;
no game code or data is included here or ever committed.

See [docs/ROADMAP.md](docs/ROADMAP.md) for the architecture, how each feature
will be built, and the phase plan.

## Status

Phase 3 in progress. Skate 3's recompiled code runs natively on 14 guest
threads (its loader, timer, file-system, network and job threads) with real
waits, events, semaphores, mutexes, critical sections and timers, and reads
its data from your ISO. Start-up loads its first archives, sets up Direct3D
and stops at the GPU's registers, where the renderer work begins. No
graphics, audio or input yet. See [docs/PHASE3.md](docs/PHASE3.md) for the
details and what's next, and [docs/PHASE2.md](docs/PHASE2.md) for the
recompilation work.

| Piece | State |
|---|---|
| `tools/s3inspect.py` | Works. Identifies an ISO, lists and extracts files, dumps `default.xex` headers |
| Launcher | Game check, display, controls (with live tester), save import, Radmin detection, modifiers |
| `recomp/tools/xexscan` | Finds all 10 addresses XenonRecomp needs; resolves 375 jump tables (354 guarded, 21 unguarded) and 43 leaf-function boundaries; marks the end-of-.text data block as non-code; disassembles and searches code |
| `scripts/recomp.ps1` | ISO → `default.xex` → xexscan → 219 MB of C++, in the work folder |
| XenonRecomp patches | 39 missing instruction forms, D3D half-float packs, the console's 50 MHz timebase, fixes; Skate 3 has 0 unrecognized instructions and 0 traps |
| `scripts/test_recomp.ps1` | 110 instruction tests (344 checks) against a Python reference model |
| Runtime (`s3native.exe`) | Runs the game's start-up natively: guest memory, loader, guest threads on host threads, kernel objects and waits, critical sections, timers, files from the disc; 73 kernel functions implemented, logging stubs for the other 213 |

## Requirements

Windows 10/11 x64, Visual Studio 2022 Build Tools with the C++ workload and
the "C++ Clang tools for Windows" component, Python 3, Git. CMake fetches SDL3
and Dear ImGui on first configure; `setup_tools.ps1` fetches XenonRecomp.

## From your ISO to a running game

```
powershell -ExecutionPolicy Bypass -File scripts\setup_tools.ps1
powershell -ExecutionPolicy Bypass -File scripts\test_recomp.ps1
powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Target xexscan
powershell -ExecutionPolicy Bypass -File scripts\recomp.ps1 -Iso "D:\Games\Skate 3.iso"
powershell -ExecutionPolicy Bypass -File scripts\build.ps1
%LOCALAPPDATA%\Skate3Native\build\bin\s3native.exe
```

1. `setup_tools.ps1` builds XenonRecomp at the pinned commit with our patches
   from `recomp/xenonrecomp-patches/` (change XenonRecomp by editing those).
2. `test_recomp.ps1` runs the instruction tests for those patches.
3. `recomp.ps1` turns your copy's `default.xex` into C++ in
   `%LOCALAPPDATA%\Skate3Native\recomp\out` (game-derived, never committed).
4. `build.ps1` builds everything with Ninja + clang-cl into
   `%LOCALAPPDATA%\Skate3Native\build` (the generated code compiles in a few
   minutes on 8 parallel jobs; Visual Studio's generator works but is serial).
5. `s3native.exe` runs the game and logs every kernel call to the console and
   `%LOCALAPPDATA%\Skate3Native\s3native.log`. It reads game files from the
   ISO or folder chosen in the launcher, or from `--game <path>`. Other
   options: `--xex <path>`, `--log <file>`, `--stall <seconds>`,
   `--time-limit <seconds>`, and `--watch <hex address>` to log every write
   to one guest word.

The launcher builds without any game files: `build.ps1 -Target s3launcher`.
Settings live in `%APPDATA%\Skate3Native\config.ini`, saves in
`%APPDATA%\Skate3Native\saves`. `s3n_selftest.exe [your.iso]` checks the
shared code.

Keep build folders on short paths: MSBuild fails with `FTK2009: path is too
long` in deep folders, which is why `build.ps1` builds out of tree.

## Inspect your copy

```
python -I tools/s3inspect.py info "D:\Games\Skate 3.iso"
python -I tools/s3inspect.py ls   "D:\Games\Skate 3.iso" data
python -I tools/s3inspect.py extract "D:\Games\Skate 3.iso" game
```

Skate 3's title ID is `454108E6`; the USA/Europe retail disc's media ID is
`5C087C2C`.
