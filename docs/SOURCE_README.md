# Gunlock (Taito, 1993) — native recompilation + RetroFront

Gunlock's 68EC020 program is statically recompiled into C and compiled natively. The rest of the
Taito F3 board is reimplemented in C: the TC0630FDP video chip, the I/O and EEPROM, and the Taito EN
sound board (68000 + Ensoniq ES5505). The shared **RetroFront** frontend, also used by the ESP Ra.De.
recomp, adds the menus, save states, rewind, display/CRT options, controller mapping and gadgets.

**No game data is included.** You need your own MAME `gunlock` romset (Gunlock Ver 2.3O 1994/01/20),
either as `gunlock.zip` or as an unzipped folder.

## First start

The program looks for `gunlock.zip` in these places:

- next to the program or the AppImage, and its `roms/` folder
- `~/Downloads`, `~/Downloads/gunlock`, `~/roms`
- `~/Emulation/roms/arcade`, `~/Emulation/roms/mame`

If it isn't found, a file browser opens. Pick the zip, or open the unzipped folder and choose
**[ Use this folder ]**. You can also drag the file onto the window. The location is remembered; to
change it later, use **Main Menu > ROM Selection**. You can also pass a path on the command line:
`gunlock /path/to/gunlock.zip`.

Every file is checked by CRC. If something is wrong, the message names the file and the problem (it's
missing, it has the wrong size, it has a bad checksum, or it's the unsupported `gunlocko` revision).

### Windows 10/11
Unzip `Gunlock-Recomp-Windows-x64.zip` anywhere and run `gunlock.exe`. It's one file with no DLLs.

### Steam Deck / Linux
1. In Desktop Mode, copy `Gunlock-Recomp-x86_64.AppImage` to a folder such as `~/Games`. Put
   `gunlock.zip` next to it, or leave it in `~/Downloads`.
2. Right-click the AppImage, then choose **Properties > Permissions > Is executable**.
3. In Steam, choose **Add a Non-Steam Game** and select the AppImage. It starts fullscreen in
   Gaming Mode; use the "Gamepad" controller layout. The menus are laid out for 1280x800.

This is a native Linux build, so it doesn't need Proton. It works with glibc 2.17 or newer. The
AppImage uses the system SDL2 when there is one, and its bundled copy otherwise.

Settings, saves and EEPROM data are stored outside the program:

- Linux: `~/.local/share/RetroFront/gunlock/`
- Windows: `%APPDATA%\RetroFront\gunlock\`

## Controls (defaults; change them in Controller Settings, presets included)

| | Keyboard | Controller / Steam Deck |
|---|---|---|
| Move | Arrow keys | D-pad / left stick |
| Button A – Shot | Z | A |
| Button B – Lock-on laser | X | B |
| Start / Coin | 1 / 5 | Menu (Start) / View (Select) |
| Player 2 | WASD, J K, 2, 6 | second controller |
| Pause / quick menu | Esc | View + Menu (configurable) |
| Service menu (test switch) | F2 | R3 |
| Service credit | 9 | – |
| Quick save / quick load | F5 / F8 | bind in Controller Settings > Frontend Hotkeys |
| Fast-forward / rewind | Tab / Backspace (hold) | bindable |
| Screenshot / fullscreen | F12 / F11 | |

In menus, arrows or the D-pad move, Enter/A selects, Esc/B goes back, and Q/E or LB/RB change
pages. The mouse works too.

Presets: **Standard** (face buttons), **Shot on triggers**, and **Arcade stick / fight pad**.
Autofire, the stick dead zone and the menu shortcut are all configurable. Controllers can be
plugged in or removed while the game runs. When you leave a menu, the game doesn't receive any
input until every button is released, so the button you pressed in the menu never fires in the game.

## Save states

- You get 8 slots, a quick slot and an autosave ("Continue"). Each one shows a thumbnail, its date
  and the play time.
- Overwriting a slot or deleting a save asks for confirmation.
- The autosave is written when you exit or return to the main menu. You can turn it off in
  Gameplay Settings.
- A state is an exact snapshot of the whole machine, taken between frames:
  - both CPUs (all registers, stack pointers, stop state and interrupt lines)
  - 128 KB of work RAM
  - palette, sprite, playfield, text, character, line and pivot RAM, plus the scroll registers
  - the sprite framebuffer and the parsed sprite list (sprite lag)
  - the dual-port RAM between the CPUs
  - sound RAM and all 32 ES5505 voices with their filters
  - the DUART timer and IRQ, the MB87078 volumes, and the ES5510 host registers
  - the EEPROM serial state, frame timing and coin-pulse state
- Each state records the game ID, the state-format version, the ROM fingerprint (built from the
  CRCs of all 14 ROMs) and a CRC32 of its contents. Truncated, damaged or mismatched states are
  rejected before anything changes, so the running game is left untouched.
- Audio queued before a load is dropped. Held inputs are cleared, so nothing sticks.
- Save states are kept separate from the game's own EEPROM (`gunlock.nv`), which holds high scores
  and operator settings just as on the real board.
- Rewind uses the same snapshots, compressed, in a ring buffer capped at 96 MB. Turn it on in
  Gameplay Settings.

## Menus

- **Main menu:** Start Game, Continue, Load Game, Display, Audio, Controller and Gameplay
  Settings, Gadgets, ROM Selection, Extras, About, Exit.
- **Quick menu (Esc):** Resume, Restart, Save State, Load State, Quick Save/Load, Display, Audio,
  Controller Configuration, Gadgets, Gameplay, Screenshot, Return to Main Menu, Exit.
- **Display:**
  - TATE (upright, as in the cabinet) or YOKO orientation, original 4:3 aspect, integer scaling
  - nearest, bilinear or sharp-bilinear filtering, fullscreen, V-Sync
  - CRT effects: scanlines, curvature, aperture grille or shadow mask, phosphor glow
  - brightness, gamma, borders and wallpapers, interface themes
- **Audio:** volume, mute, output device and latency. The hardware doesn't mix music and effects
  separately, so there are no separate sliders for them.
- **Gadgets:** 1P score, high score, 2P score, input display, FPS, frame time and play time. You
  can move and resize them with **Arrange Gadgets**. The score values are read from the game's own
  on-screen counters and are only shown when the HUD labels and digits are present.
- **Extras:** the game's Service Menu (difficulty, lives, extends, sound and input tests), service
  credit, EEPROM reset, data folders, licences.

## Building

Requirements: python3, gcc/g++, SDL2 development files, mingw-w64 (Windows build),
`pip install ziglang` (Steam Deck baseline build), and appimagetool (AppImage).

```sh
./gen.sh /path/to/gunlock.zip     # extract the program from YOUR romset and generate gen/*.c (static recompilation)
./build_frontend.sh linux         # build/fe-linux/gunlock
./build_frontend.sh windows       # build/fe-windows/gunlock.exe
./build_frontend.sh deck          # build/fe-deck/gunlock (glibc 2.17 baseline, system libSDL2)
APPIMAGETOOL=/path/to/appimagetool ./build_release.sh   # dist/: Windows zip, AppImage, Linux tar.gz, SHA256SUMS.txt
```

Developer tools:

- `./build_test.sh [interp]` builds the headless harness `build/gltest`.
- `tools/lockstep.sh N` runs the recompiled code against the Musashi interpreter and compares the
  full machine-state hash every frame.
- `STATE_TEST=frame build/gltest rom N recomp` saves a state, plays on, reloads it, replays the same
  inputs and checks that every frame matches.

## Status

| Feature | Status |
|---|---|
| Static recompilation of the 68EC020 program (98% of traced instructions native; the rest go through Musashi) | Implemented and tested: bit-exact with the interpreter over 20,000 frames of scripted play |
| Taito F3 video (4 playfields, line scroll, zoom, clip planes, alpha blending, sprites with zoom/blocks, text and pivot layers) | Implemented and tested (boot, attract, gameplay screenshots) |
| ES5505 sound, 68000 sound CPU, DUART timer, MB87078 | Implemented and tested (non-silent output measured; I haven't listened to it) |
| ES5510 "ESP" effects DSP (reverb/echo) | Not emulated: the effect channels are mixed dry, so reverb is missing |
| Save states, slots, quick save/load, autosave, damage and version rejection | Implemented and tested (0/2000 frames mismatched after reload, same result across processes, corrupt or truncated files rejected) |
| Frontend menus, quick menu, ROM picker, remembered paths, Unicode paths | Implemented and tested under Xvfb on Linux |
| Rewind, fast-forward, screenshots | Implemented; rewind uses the tested snapshot path |
| Score / high-score gadgets | Implemented and tested (values match the on-screen HUD) |
| Lives, stage and timer gadgets | Not provided: there's no verified data source yet |
| Controller navigation, hotplug, Steam Deck controls | Implemented (shared RetroFront code) but not tested with physical hardware |
| Windows .exe | Builds; started under Wine only |
| Gameplay video recording | Planned |
| Recompiling the sound CPU program | Planned (it runs on the Musashi interpreter) |
| Other revisions (gunlocko, rayforce, rayforcej) | Not supported (different program ROMs) |
