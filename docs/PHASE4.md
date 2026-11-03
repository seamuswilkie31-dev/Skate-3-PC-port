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

## Done: sound

**How the game makes sound.** XAudio2, linked into the game from the XDK,
mixes on the CPU. Compressed sound is XMA, which the console decodes in
hardware:
- At boot the game creates 256 decoder contexts (`XMACreateContext`) and
  enables them all.
- Each context takes two input buffers of 2048-byte packets and writes 16-bit
  PCM into an output ring.
- Every 5.3 ms XAudio2 hands the audio driver a frame of 256 samples for each
  of 6 channels (5.1), through `XAudioSubmitRenderDriverFrame`.

Until now the decoder wrote silence and the frames were dropped.

**The decoder** (`runtime/apu/xma.cpp`):
- **FFmpeg does the decoding.** XMA frames are WMA Pro frames. FFmpeg's own
  XMA decoders hold back about 4096 samples to line up multi-stream files,
  and only let them go at the end of the stream. The hardware puts out every
  frame as it decodes it, so those decoders don't fit. Instead the runtime
  walks the packets itself and hands FFmpeg's WMA Pro decoder one frame at a
  time. Each frame goes in a packet of its own, with the decode flags FFmpeg
  sets for XMA.
- **Walking the input, as the hardware does:**
  - Each packet's header says where its first frame starts, and how many
    packets of other streams to skip.
  - Frames run on into the next packet, and from the end of one input buffer
    into the other.
  - A used-up buffer goes back to the game by clearing its valid bit.
  - XAudio2's loop points (bit offsets and a count) send the read offset back.
- **Output.** Each frame is 512 samples. It goes into the ring as 16-bit
  big-endian samples, channels interleaved, in 256-byte blocks. When the ring
  is full, or the input runs out, the output's valid bit is cleared. The game
  sets it again once it has read.
- **The first frame of a sound.** After a context is initialised, FFmpeg uses
  the first frame only to prime the decoder, and it comes out with no
  samples. FFmpeg's XMA decoders drop that frame too, as encoder delay. The
  runtime writes silence in its place, so input and output stay in step.
- **Not handled yet:** loop points inside a frame (loop subframe end and skip)
  and the subframes-to-decode count. The decoder works in whole frames.

**FFmpeg in the build** (`third_party/ffmpeg`):
- **What's built.** CMake downloads the 8.1.3 release source, checks its hash,
  and compiles about 150 C files of libavutil and libavcodec with our own
  CMake file.
- **Configuration.** It comes from FFmpeg's configure, kept in the repo: only
  the WMA Pro decoder, no assembly, no threads. So nothing beyond Visual
  Studio is needed to build. `scripts/ffmpeg_config.ps1` regenerates it for
  another release (that needs MSYS2).
- **Threads.** With threads configured out, FFmpeg's one-time table setup
  isn't thread safe. Every FFmpeg call is made under the decoder's lock.

**Output** (`runtime/apu/output.cpp`):
- **Playback.** The frames go to the default device through an SDL3 audio
  stream, as 48 kHz float stereo. 5.1 is folded down: centre and rear
  channels at -3 dB into the front pair, the LFE left out.
- **Timing.** The driver clock now follows the device. It runs an eighth
  faster or slower to keep 4–12 frames (21–64 ms) queued.
- **Options.** `--mute` plays silence with the same timing. `--audio-wav
  <file>` records the stereo mix. The report gives per-channel levels and
  counts underruns.

**Result.** Two muted runs, recorded with `--audio-wav`:
- **100 s to the menus.** 42,731 frames decoded, no FFmpeg errors. The game
  sends sound on all six channels, loudest on the centre channel.
- **240 s into the world** (`--press start:40,a:50`, then A every 15 s,
  ending in the skate park):
  - 185,713 frames decoded, with no FFmpeg errors and no broken frames.
  - 103 frames came out empty and were written as silence. FFmpeg reported
    nothing for them.
  - No underruns: 238.8 s of sound in a 240 s run.
  - Decoding costs about 2.4% of one core: 5.7 s over a repeat of the
    same run.
- **The recording.** There's sound in 208 of 238 seconds. Spectral flatness
  stays at or below 0.05 in every second; music and effects sit well below
  0.3, while noise or misread bits come near 1. One second clips briefly
  (0.15% of its samples), a loud moment in the game's own mix.

Nobody has listened to it yet.

## Next

1. **Saves (phase 5, brought forward):** done, see [PHASE5.md](PHASE5.md). The
   game now gets past the storage prompt, creates and reloads its save, and
   plays the new-player flow into the skater editor.
2. **Keyboard and mouse bindings in the launcher**, next to the pad tester,
   and tuning the mouse's flick feel on real tricks.
3. **Audio:** done, see above. Next for it: listening by ear, loop points
   inside a frame, and a volume setting in the launcher.
