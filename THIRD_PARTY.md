# Third-party components

| Component | Used for | License |
|---|---|---|
| [SDL3](https://github.com/libsdl-org/SDL) | Windowing, gamepads, dialogs, audio | zlib |
| [Dear ImGui](https://github.com/ocornut/imgui) | Launcher UI | MIT |
| [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) @ `ddd128b` | PowerPC → C++ recompilation backend; `xexscan` links its XenonUtils | MIT |
| [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) @ `990d03b` | Xenos shader → HLSL translation, built by `setup_tools.ps1` and run by `shaders.ps1` | MIT |
| [Vulkan SDK](https://vulkan.lunarg.com) | The runtime's renderer (headers and loader); DXC compiles translated shaders to SPIR-V | Apache-2.0 (headers, loader); DXC: University of Illinois/NCSA |

XenonRecomp and XenosRecomp are generic tools with no Skate 3-specific
content. Skate 3: Recompiled is **not** used, referenced or consulted.

Our changes to XenonRecomp live in `recomp/xenonrecomp-patches/` and to
XenosRecomp in `recomp/xenosrecomp-patches/` (MIT, like the tools themselves;
their licenses are included there). They are generic instruction support and
fixes and could be offered upstream. The runtime also includes XenonRecomp's
kernel export-name tables (`XenonUtils/xbox/*.inc`) from the XenonRecomp
checkout at build time.

[Xenia](https://github.com/xenia-project/xenia) (BSD-3-Clause) is consulted as
a behavioural reference only: the VMX128 D3D pack/unpack instructions, and the
console interfaces the runtime re-implements (kernel function signatures and
structure layouts, GPU register numbers and command packet formats, the
audio driver and XMA context layout). No Xenia code is copied into this
project.
