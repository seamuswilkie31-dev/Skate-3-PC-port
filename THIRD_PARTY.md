# Third-party components

| Component | Used for | License |
|---|---|---|
| [SDL3](https://github.com/libsdl-org/SDL) | Windowing, gamepads, dialogs, audio | zlib |
| [Dear ImGui](https://github.com/ocornut/imgui) | Launcher UI | MIT |
| [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) @ `ddd128b` | PowerPC → C++ recompilation backend; `xexscan` links its XenonUtils | MIT |
| [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) (phase 3) | Xenos shader translation backend | MIT |

XenonRecomp and XenosRecomp are generic tools with no Skate 3-specific
content. Skate 3: Recompiled is **not** used, referenced or consulted.

Our changes to XenonRecomp live in `recomp/xenonrecomp-patches/` (MIT, like
XenonRecomp itself; its license is included there as `LICENSE-XenonRecomp.md`).
They are generic instruction support and fixes, written from the
PowerPC/VMX128 definitions, and could be offered upstream. The runtime also
includes XenonRecomp's kernel export-name tables (`XenonUtils/xbox/*.inc`)
from the XenonRecomp checkout at build time.

[Xenia](https://github.com/xenia-project/xenia) (BSD-3-Clause) was consulted as
a behavioural reference for the VMX128 D3D pack/unpack instructions: which
destination words each pack mode and shift writes, and which halfwords an
unpack reads. No Xenia code is copied into this project.
