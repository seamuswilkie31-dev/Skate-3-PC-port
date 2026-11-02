# Skate 3 Native

A native Windows port of the Xbox 360 version of Skate 3: static
recompilation plus a fresh, clean-room reverse engineering effort. It is not a
fork of Skate 3: Recompiled and shares no code with it.

**You need your own copy of the game.** The launcher reads your disc image;
no game code or data is included here or ever committed.

See [docs/ROADMAP.md](docs/ROADMAP.md) for the architecture, how each feature
will be built, and the phase plan.

## Status

Phase 3 in progress. Skate 3's recompiled code runs natively on 24 guest
threads (render, loader, file-system, audio, movie, network and job threads)
with real waits, events, mutexes, critical sections and timers. It reads its
data from your ISO, drives an emulated GPU front end, and has an audio clock
and a silent XMA decoder. Its whole boot sequence now runs: the EA intro,
loading, then the title screen, where with no input it starts the
attract-mode video. All of it now renders in a Vulkan window: the language
select, the intros, the title screen with its 3D scene, and the attract video.
The game's own shaders are translated to SPIR-V, and its draws, textures,
render targets and resolves run in Vulkan. Pads, keyboard and mouse work,
and so do saves: the game creates its save in
`%APPDATA%\Skate3Native\saves`, loads it on the next boot, and plays the
new-player flow (difficulty, camera, intro film, team and skater) into the
skater editor. The 3D world isn't drawn right yet, and there's no sound. See
[docs/PHASE3.md](docs/PHASE3.md) for the details and what's next, and
[docs/PHASE2.md](docs/PHASE2.md) for the recompilation work.

| Piece | State |
|---|---|
| `tools/s3inspect.py` | Works. Identifies an ISO, lists and extracts files, dumps `default.xex` headers |
| Launcher | Game check, display, controls (with live tester), save import, Radmin detection, modifiers |
| `recomp/tools/xexscan` | Finds all 10 addresses XenonRecomp needs; resolves 375 jump tables (354 guarded, 21 unguarded) and 43 leaf-function boundaries; marks the end-of-.text data block as non-code; disassembles and searches code |
| `scripts/recomp.ps1` | ISO → `default.xex` → xexscan → 219 MB of C++, in the work folder |
| XenonRecomp patches | 39 missing instruction forms, D3D half-float packs, the console's 50 MHz timebase, fixes; Skate 3 has 0 unrecognized instructions and 0 traps |
| `scripts/test_recomp.ps1` | 110 instruction tests (344 checks) against a Python reference model |
| Runtime (`s3native.exe`) | Runs the game's start-up natively: guest memory, loader, guest threads on host threads, kernel objects and waits, critical sections, timers, files from the disc, GPU front end (`runtime/gpu`), Vulkan renderer: draws, textures, render targets, resolves and frames (`runtime/render`), input from pads, keyboard and mouse (`runtime/input`), saves as host folders, profile, dialogs and notifications (`runtime/kernel`), audio clock, silent XMA decoder (`runtime/apu`); 152 kernel functions implemented, logging stubs for the other 134 |

## Requirements

Windows 10 (1803 or later) or 11, x64, a Vulkan 1.3 GPU, Visual Studio 2022
Build Tools with the C++ workload and the "C++ Clang tools for Windows"
component, the Vulkan SDK, Python 3, Git. CMake fetches SDL3 and Dear ImGui on
first configure; `setup_tools.ps1` fetches XenonRecomp and XenosRecomp.

## From your ISO to a running game

```
powershell -ExecutionPolicy Bypass -File scripts\setup_tools.ps1
powershell -ExecutionPolicy Bypass -File scripts\test_recomp.ps1
powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Target xexscan
powershell -ExecutionPolicy Bypass -File scripts\recomp.ps1 -Iso "D:\Games\Skate 3.iso"
powershell -ExecutionPolicy Bypass -File scripts\shaders.ps1 -Iso "D:\Games\Skate 3.iso"
powershell -ExecutionPolicy Bypass -File scripts\build.ps1
%LOCALAPPDATA%\Skate3Native\build\bin\s3native.exe
```

1. `setup_tools.ps1` builds XenonRecomp and XenosRecomp at their pinned
   commits with our patches from `recomp/xenonrecomp-patches/` and
   `recomp/xenosrecomp-patches/` (change the tools by editing those).
2. `test_recomp.ps1` runs the instruction tests for the XenonRecomp patches.
3. `recomp.ps1` turns your copy's `default.xex` into C++ in
   `%LOCALAPPDATA%\Skate3Native\recomp\out` (game-derived, never committed).
4. `shaders.ps1` translates the game's shaders to SPIR-V in
   `%LOCALAPPDATA%\Skate3Native\shaders` (game-derived too).
5. `build.ps1` builds everything with Ninja + clang-cl into
   `%LOCALAPPDATA%\Skate3Native\build` (the generated code compiles in a few
   minutes on 8 parallel jobs; Visual Studio's generator works but is serial).
6. `s3native.exe` opens the game window and runs the game, logging every
   kernel call to the console and `%LOCALAPPDATA%\Skate3Native\s3native.log`.
   It reads game files from the ISO or folder chosen in the launcher, or
   from `--game <path>`. Other options: `--no-window`, `--xex <path>`,
   `--log <file>`, `--stall <seconds>`, `--time-limit <seconds>`,
   `--watch <hex address>` to log every write to one guest word,
   `--watch-reads <hex address>` for reads too, `--dump-frames <n>` to save
   every nth frame to `%LOCALAPPDATA%\Skate3Native\frames` as a BMP, and
   `--capture-frame <n>` to log every clear, resolve and draw of frame n.
   `--press <button:seconds,...>` presses pad buttons on a schedule (for
   example `--press start:40,a:50`).

Controls: any SDL3 pad (Xbox, PlayStation, Switch), or keyboard and mouse:
WASD for the left stick, mouse movement for the right stick (flick-it),
Space/E/Q/F for A/B/X/Y, Enter for Start, Tab for Back, Z/C for the bumpers,
the mouse buttons for the triggers, the arrows for the D-pad. Click the window
to capture the mouse; Escape frees it. See [docs/PHASE4.md](docs/PHASE4.md).

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
