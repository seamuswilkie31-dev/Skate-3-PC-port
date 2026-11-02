# Phase 5 — progress

Goal ([ROADMAP.md](ROADMAP.md)): saves, DLC and progression. The exit: your
imported skater loads, a new save round-trips, and imported DLC shows up in
the game.

## Done: saves, and the way from the title screen into the game's menus

After Start and A, the game asked for a storage device. Behind that prompt
came the rest of what a console provides:
- content packages and writable files;
- the player's profile settings;
- system dialogs and notifications.

Each was found by running with scripted presses (`--press`) and reading
where the game stopped.

**Writable files** (`kernel/file_imports.cpp`):
- **Mounted folders.** Host folders mounted as devices now sit beside the
  read-only disc.
- **Operations.** Files there can be created, opened, overwritten,
  truncated, read, written, renamed and deleted (on request, or when
  closed), and directories listed, with their sizes and times.
- **Names keep the case the title gave them.** With lower-cased names the
  game's own check didn't find `SKATER.P` and called its save damaged.

**Storage and content** (`kernel/content_imports.cpp`):
- **One storage device: the hard drive.** The device selector picks it
  without asking, as a console with one device does.
- **Saves are folders.** A content package is a folder under
  `%APPDATA%\Skate3Native\saves\<XUID>\<title ID>\<content type>\<file name>`,
  mounted under the root name the game chooses (`rmcsave:\` here).
- **Operations.** Create, open, close, flush, delete, enumerate, creator,
  thumbnail and license mask.
- **Sizes read right as 32-bit numbers.** With 8 GB free (`0x2_0000_0000`),
  the game read none and warned of low storage.

**Profile, console and system** (`kernel/user_imports.cpp`,
`config_imports.cpp`, `time.cpp`, `ui_imports.cpp`, `xam_imports.cpp`):
- **Profile settings** have their defaults; the title's own settings have
  no value yet.
- **Privileges** are all allowed.
- **Console settings:** English, UTC.
- **Timestamps:** times convert to dates and back, for saves.
- **System dialogs answer at once:**
  - the on-screen keyboard takes the title's suggestion, or the player's
    name;
  - a message box picks its focused button;
  - sign-in returns at once.
- **Notifications** have real listeners and `XNotifyGetNext`.
  - A dialog posts "UI shown" (`XN_SYS_UI` 1) at once and "UI closed" (0)
    half a second later.
  - Skate 3 only starts listening once the keyboard returns, then waits for
    "closed" before it takes the text. With both sent at once it waited
    forever.
- **Messages to XAM's apps** (`XMsg*`) complete at once.
- **Enumerations** hand out real items. The DLC thread's marketplace query
  (`XamCreateEnumeratorHandle`, `XamGetPrivateEnumStructureFromHandle`) gets
  an empty one; until then it crashed that thread.
- **Completion routines** of asynchronous calls run, as APCs.

**Result.** Run with `--press start:40,a:50` and A every 15 seconds after.
The game creates its save package: `ALIAS_SKATER`, holding `SKATER.P`
(2.6 MB, EA's `MC02` container) and `RMCDEL`. Then it plays the new-player
flow:
- difficulty;
- camera;
- the intro film;
- the team name ("New Team") and your name ("The Legend"), both through the
  keyboard;
- the team logo;
- "Start your team!";
- the skater editor.

**The save round-trips.** On the next boot the game opens the package,
reads the save back and goes on without a warning. In an 8-minute run there
were 2.1 million draws through 36 pipelines, none skipped. The validation
layer reports nothing.

**Not done yet:**
- **Your own data:** importing your Xbox 360 save (STFS) and DLC, and the
  unlock-all option.
- **Profile settings aren't stored.** The game doesn't write any.
- **The keyboard can't be typed on yet.** Dialogs take their defaults.

The skater editor's 3D view now renders too; see
[PHASE3.md](PHASE3.md#rendering-in-game-the-skater-editor).

## Next

1. **Rendering in game:** shadows (depth resolves), then the world ("Play
   Skate 3").
2. **The keyboard dialog with real typing**, in the window.
3. **Your save and DLC:** STFS import in the launcher, mounted as content.
4. **Audio** (phase 4): real XMA decoding and output.
