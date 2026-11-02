# Skate 3 Native — Architecture & Roadmap

A native Windows port of the Xbox 360 version of Skate 3, built by static
recompilation plus our own reverse engineering. You need your own copy of the
game: nothing from the game ships with this project.

## Ground rules

1. **Your own copy only.** The launcher reads your ISO. Game code, game data,
   recompiled output and save files are never committed or redistributed.
   `.gitignore` enforces this for `game/`, `recomp/out/` and `build/`.
2. **Clean room relative to Skate 3: Recompiled.** Don't read, copy or link
   its source, configs, patches or symbol names. Every Skate 3-specific
   finding in this repo comes from our own analysis of our own copy.
3. **Generic tools are fine.** XenonRecomp (PPC → C++) and XenosRecomp
   (Xenos shaders → HLSL/SPIR-V) are general-purpose, MIT-licensed tools that
   know nothing about Skate 3. We use them as compiler backends, the same way
   we use MSVC or Clang. Third-party licenses go in `THIRD_PARTY.md`.

## How the port works

```
 your ISO ──► ingest ──► default.xex (+ title update) ──► XenonAnalyse ──► recomp config (ours)
                │                                              │
                │                                              ▼
                │                                      XenonRecomp ──► ppc_recomp_*.cpp  (local only)
                ▼                                                              │
          game data files ◄────── runtime reads them directly from the ISO     │
                                                                               ▼
   launcher ──► config.ini ──► s3native.exe = recompiled game code + our runtime:
                                 ├─ memory      4 GB guest address space, big-endian access
                                 ├─ kernel      xboxkrnl / xam imports re-implemented (HLE)
                                 ├─ files       game:\ → ISO, saves → %APPDATA%
                                 ├─ threads     guest threads on host threads, events, critsecs
                                 ├─ gpu         Xbox 360 D3D calls hooked → Vulkan backend
                                 ├─ shaders     Xenos microcode → SPIR-V via XenosRecomp
                                 ├─ audio       XMA decode → PCM → SDL3 audio
                                 ├─ input       SDL3 gamepads → XInput state the game polls
                                 ├─ net         system link / session layer → UDP over Radmin
                                 └─ patches     framerate, resolution, modifiers, unlocks
```

Static recompilation means the game's PowerPC functions become C++ functions
compiled for x64. No emulation loop, no JIT. Anything the game asked the
console for (kernel, GPU, audio, network, storage) we re-implement natively.
That boundary is where all the Skate 3-specific reverse engineering happens.

## Your goals, mapped

| Goal | How | Where it lands | Difficulty |
|---|---|---|---|
| Launcher with game location | Pick the ISO, verify it's Skate 3 by reading `default.xex`'s headers | Phase 1 (started) | Low |
| Player modifiers | Launcher writes values; runtime patches the physics/tuning constants once we locate them | UI phase 1, effect phase 6 | Medium: needs RE of the tuning data |
| Native Vulkan rendering | Hook the game's statically linked D3D library at function level, drive our own Vulkan backend | Phase 3 | High: the largest single subsystem |
| Your skater's save | Import your Xbox 360 save (STFS `CON` package) in the launcher; runtime mounts it | Import phase 1, use phase 5 | Medium |
| 100% save option | Unlock-all applied by the port itself once the save/progress format is mapped. We never ship someone else's save file | Phase 5 | Medium |
| VSync, 60 / 120 / 240 fps | Present-mode control is easy. Uncapping is not: the 360 build targets 30 fps, so every frame-time-dependent system (physics step, animation, camera, UI) must be found and fixed | 60 in phase 6, 120/240 after | High |
| RTX ray tracing | The game never had RT data, and a recompiled renderer only sees draw calls. Feasible later as an add-on: RT shadows/AO from reconstructed scene geometry in our Vulkan backend. The launcher shows it as an experimental option, greyed out until then | Stretch, after phase 6 | Very high |
| Multiplayer via Radmin | Radmin VPN is a virtual LAN (26.x.x.x addresses). Step one is finding out how the game's multiplayer finds sessions: if it has a LAN/system-link path we route it over the Radmin adapter; if it only knows EA's online backend we re-implement that session layer, host-as-server, over UDP | Phase 7 | High, and the biggest unknown: research first |
| Xbox One controller | SDL3 gamepad → XInput state | Phase 4 | Low |
| PS4 and Switch controllers | Same SDL3 path (DualShock 4, DualSense, Switch Pro, Joy-Con pairs), plus Nintendo positional layout. Matching on-screen button prompts means swapping UI textures: later polish | Input phase 4, prompts phase 8 | Low / Medium |
| Keyboard and mouse | Turned into the same virtual pad the game reads. Keys drive the left stick (WASD by default; a key pressed means full tilt, so diagonals and a walk modifier matter). Mouse movement drives the right stick for Flick-It: each motion becomes a stick deflection that springs back to centre, with sensitivity, a deadzone and a return speed to tune, so a quick flick reads as a flick. Keys and mouse buttons cover the face buttons, bumpers, triggers, D-pad, Start and Back. Bindings in the launcher's Controls page, next to the pad tester | Phase 4 | Medium: the stick feel needs tuning on real tricks |
| Your DLC | Import the DLC packages you own (from a USB stick or hard-drive dump of your 360) in the launcher, the same way as saves. The runtime mounts them as installed content for the game's content APIs (its `dlc_enumerator` thread already looks for them). Only packages you supply, never downloaded | Import phase 5, use phase 5 | Medium |

## Phases and exit criteria

**Phase 0 — Groundwork.** *(Done.)* Toolchain installed (VS 2022 Build Tools with
Clang, Vulkan SDK). `tools/s3inspect.py` identifies your ISO and
`default.xex`: title ID, version, media ID, XDK libraries, imports.
*Exit:* we know exactly which build we're porting and whether a title update
applies.

**Phase 1 — Launcher v0.** *(Done.)* Game picker + verification, display / controls /
saves / multiplayer / modifiers settings, config file the runtime will read,
save import. *Exit:* launcher writes a config that describes everything the
runtime will need.

**Phase 2 — First instructions.** *(Done; details in
[PHASE2.md](PHASE2.md).)* Decrypt/decompress the XEX (+ apply the title
update if present), analyse it (our xexscan replaces XenonAnalyse: jump
tables, function boundaries, `setjmp`/`longjmp`) into `recomp/skate3.toml`.
Runtime skeleton: guest memory, PPC context, import thunks that log every
kernel call they receive. *Exit:* the game's entry point runs and we see its
first kernel calls.

**Phase 3 — Kernel until graphics.** *(In progress: threads,
synchronization, files, the GPU front end, audio clock and a silent XMA
decoder done. Exit reached: the whole boot renders in Vulkan, from the
language select and the EA intro to the title screen and the attract video.
See [PHASE3.md](PHASE3.md).)*
Implement kernel/XAM imports in the
order the game hits them: memory, threads, sync, files (served from the
ISO), timers, XAM content/profile APIs. Identify the D3D library functions
and hook them. Vulkan device, swapchain, shader translation. *Exit:* first
frame — the EA logo / main menu renders.

**Phase 4 — Input & audio.** *(In progress: pads, keyboard and mouse done;
the game takes Start and A and stops at the storage device prompt, which is
phase 5's. See [PHASE4.md](PHASE4.md).)* SDL3 gamepads mapped to XInput; keyboard and
mouse mapped onto the same virtual pad (keys for the left stick and buttons,
mouse movement for the right stick and Flick-It), with bindings in the
launcher; XMA audio decode. *Exit:* menus are navigable with Xbox,
PlayStation and Switch pads and with keyboard and mouse, with sound, and
basic flick tricks land with the mouse.

**Phase 5 — Saves, DLC & progression.** *(In progress: saves done; the game
creates and reloads its save and plays the new-player flow into the skater
editor. See [PHASE5.md](PHASE5.md).)* Content APIs backed by
`%APPDATA%/Skate3Native/saves`, import of your 360 save, import of the DLC
packages you own (mounted as installed content), unlock-all option.
*Exit:* your imported skater loads; a new save round-trips; imported DLC
shows up in the game.

**Phase 6 — Gameplay & frame rate.** Fix rendering/simulation bugs through a
full playthrough. Locate the frame limiter and every frame-time-dependent
system; 60 fps first, then 120/240. Resolution, aspect ratio, VSync. *(Begun:
the game paces itself by vertical blanks and times itself by the clock, so
the launcher's cap and VSync work; see [PHASE3.md](PHASE3.md). The world runs
at about 58 fps at the 60 cap.)*
Modifiers wired to the physics constants. *Exit:* career mode completable at
60 fps without timing bugs.

**Phase 7 — Multiplayer.** Research how sessions are created and found;
implement the session layer over the Radmin adapter. *Exit:* two PCs on
Radmin VPN skate together.

**Phase 8 — Polish & stretch.** Controller-specific button prompts,
installer flow in the launcher, RT experiments.

## Repository layout

```
docs/ROADMAP.md     this file
tools/s3inspect.py  identify your ISO, list/extract files, dump XEX headers
common/             code shared by launcher and runtime (config, ISO/XEX/STFS readers)
launcher/           the launcher (C++20, SDL3, Dear ImGui)
recomp/skate3.toml  our XenonRecomp config for Skate 3 (addresses only, no game code)
recomp/tools/       xexscan: finds the helper/setjmp/longjmp addresses XenonRecomp needs
scripts/            setup_tools.ps1 (build XenonRecomp), recomp.ps1 (run phase 2 pipeline)
runtime/            the native runtime: guest memory, loader, kernel imports (kernel/)
```

Everything derived from the game lives in the work folder
`%LOCALAPPDATA%\Skate3Native`, never in the repo: `game\` (extracted
files), `recomp\out\` (generated C++), `tools\` (XenonRecomp checkout and
build). This also keeps paths short, which MSBuild needs.

## Known facts about the target

| | |
|---|---|
| Title ID | `454108E6` (EA-2278) |
| Retail disc media ID | `5C087C2C` (USA / Europe), region free |
| Executable | `default.xex` version 0.0.0.3, sha1 `2f1e63fb…91295b`, original name `sk83_na_f.self` |
| Disc | XGD2, 103 files, 5.97 GiB |
| Image | loads at `0x82000000`, size `0x01130000`, entry `0x82EB4F80`; 12 MiB of code, 33,653 `.pdata` functions |
| XDK | 2.0.9328: `D3D9LTCG`, `XAUDIO2`, `XAPOBA`, `XMCORE`, `XNET`, `XONLINE`, `XHV2`, `XPARTY`, `XMP` |
| Kernel imports | `xboxkrnl.exe` 344, `xam.xex` 240 |
| Title updates | several exist; we build on the disc version for now |

Multiplayer lead: the XEX carries a `lan_key` header and links `XNET`, which is
what system link uses. If Skate 3 has a working LAN path, Radmin support is
routing rather than reimplementing EA's backend. Not confirmed yet.
