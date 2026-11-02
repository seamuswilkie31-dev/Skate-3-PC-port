# Phase 4 — progress

Goal ([ROADMAP.md](ROADMAP.md)): input and audio. Pads (Xbox, PlayStation,
Switch) and keyboard and mouse drive the game through one virtual pad, and
the game's sound plays. The exit: menus navigable with each kind of
controller, with sound, and basic flick tricks landing with the mouse.

## Done: controllers, keyboard and mouse

**How the game reads input.** It polls XAM for users 0–3 every frame, with
the XInput structures, big-endian:
- `XamInputGetCapabilities`;
- `XamInputGetState`;
- `XamInputSetState` (vibration).

All three were stubs returning success, so the game saw four connected pads
that never moved.

**The input module** (`runtime/input`):
- **Pads.** SDL3 opens up to four pads, Xbox, PlayStation and Switch alike,
  as users 0–3. Face buttons go by position (A at the bottom), unless the
  launcher's *Nintendo layout* setting asks a Switch pad to go by label.
- **Launcher settings.** The sticks get the launcher's radial dead zones,
  rescaled to keep full range. The triggers get its threshold. Vibration the
  game asks for goes to the pad, scaled by the launcher's strength (or off).
- **Keyboard and mouse.** They're one more pad, merged into user 0, so user
  0 is always connected. Combined buttons and the stronger stick or trigger
  win.
- **Threads.** The window's thread feeds it (each event, then a poll every
  loop), and the game's threads read a snapshot under a lock
  (`kernel/input_imports.cpp`).

**Default keyboard and mouse layout:**

| Control | Pad |
|---|---|
| W A S D | left stick |
| Mouse movement | right stick: a deflection that springs back when the mouse stops, so a quick flick reads as a flick |
| Space / E / Q / F | A / B / X / Y |
| Enter / Tab | Start / Back |
| Z / C | left / right bumper |
| Right / left mouse button | left / right trigger |
| Arrow keys | D-pad |
| Left Ctrl / middle mouse button | left / right stick click |

Clicking in the window captures the mouse, and Escape frees it. Losing focus
releases everything.

**`--press button:seconds[,...]`** presses pad buttons on a schedule (each
held for a fifth of a second), so runs can be checked without anyone at the
keyboard.

**A local profile.** Pressing Start makes the game look up the signed-in
player and their achievements. With XAM's user functions stubbed, it read
through a null pointer. `kernel/user_imports.cpp` now provides:
- one offline profile signed in on user 0, named after the launcher's player
  name (`XamUserGetSigninState`, `XamUserGetSigninInfo`, `XamUserGetXUID`,
  `XamUserGetName`);
- achievement enumerators that are empty (`XamUserCreateAchievementEnumerator`,
  `XamEnumerate`). Enumerators are kernel objects, so `NtClose` closes them.

Real profiles come with the saves in phase 5.

**Result.** A run with `--press start:40,a:50`:
- **Start** at the title screen makes the game sign in the profile and show
  its autosave notice ("Warning! Skate 3 autosaves at certain points...").
- **A** continues past the notice.
- **Next stop:** the game asks for a storage device. A thread calls
  `XamShowDeviceSelectorUI` over and over, and that's still a stub, so the
  screen stays black. Storage devices and the content APIs are phase 5's
  saves work.

SDL found the pad connected to this machine. The keyboard, mouse and pad
haven't been tried by hand yet; the scripted presses go through the same
path.

## Next

1. **Saves (phase 5, brought forward):** done, see [PHASE5.md](PHASE5.md). The
   game now gets past the storage prompt, creates and reloads its save, and
   plays the new-player flow into the skater editor.
2. **Keyboard and mouse bindings in the launcher**, next to the pad tester,
   and tuning the mouse's flick feel on real tricks.
3. **Audio:** real XMA decoding behind the existing context interface, and
   the render driver's frames played through SDL3.
