# Technical notes

Every change that alters runtime behavior is recorded here with what was verified and how.

## Game analysis (S2LP01)

- `main.dol` holds all the code; the disc has no REL modules.
- Function list: 18,275 functions from decomp-toolkit 1.8.4 (`dtk dol config` + `dtk dol split`), converted to `game/analysis/dol_functions.csv`. The lifter discovers 41 more entries (18,316 translated, 1,035,189 instructions).
- System functions not named by dtk, identified by hand from their position in `OSContext.c` / `OSMemory.c` and their instructions:
  - `OSGetCurrentContext` 0x8028f2d0, `OSSaveContext` 0x8028f2e0 (`stmw r13, 0x34(r3)`), `OSSwitchFiber` 0x8028f450, `OSSwitchFiberEx` 0x8028f480.
  - `OSRealModeCall` 0x80294740: `clrlwi r3,r3,2; mtsrr0; mfmsr; rlwinm; mtsrr1; rfi`, the SDK's `RealMode`.
  - `OSSelectThread` is dtk's `SelectThread` (0x80296860).
- The game has no `setjmp`/`longjmp`. 0x80401488 restores registers and jumps to a handler: it is the CodeWarrior C++ exception runtime, not a coroutine switch.
- AX DSP microcode: 0x2000 bytes at DOL offset 0x47baa0 (0x8047f9a0), checksum `bc128f9d`. The 0x80-byte block at 0x8046d988 is `DSPInitCode`, used by `__OSInitAudioSystem`.
- Middleware found in the strings: NW4R (G3D, EF, LYT), CRI ADX2 / Atom (`.acb`/`.awb`), Mobiclip (`.mo`), the developer's own `nsNile` engine.
- Frame rate: the game renders at a steady 30 fps (release build, 45 s run, about 6,000 vertices per frame).

## GX: PE token interrupt (`GXSetDrawSync`)

- Symptom: after three EFB copies and no draw, the main thread spun forever at 0x80031eb8, waiting for `+0x34` of the frame object to become -1.
- Cause: the frame code queues a request and calls `GXSetDrawSync` (0x802a6b70, BP 0x48 then 0x47). The token interrupt handler (0x802a6eb0) calls the game's callback 0x8002d960, which runs the queued requests up to the token's id; one of them (0x80031bd0) clears `+0x34`. The engine only raised the draw-done interrupt, so the token interrupt never came.
- Change (`engine/src/gpu/gx.cpp`, `engine/src/core/interrupts.cpp`): BP 0x47 and 0x48 store the token; 0x48 also marks the token interrupt pending. When enabled in the PE control register, the poll delivers OS interrupt 18 (`PI_PE_TOKEN`) with the token in `PE_TOKEN` (0xCC00100E); writing the control register with bit 2 clears it.
- Ordering: the GPU thread processes a token before a later draw-done, so the poll takes the draw-done flag first and delivers the token before it. Delivering draw-done first let the game run its queue on draw-done and the late token found the queue empty (call to 0x30310000, the disc id at address 0). The pending flag is consumed before the token value is read, so a token that arrives in between is never reported with an older value.
- Verified: 12 runs of 40-50 s with no crash or hang; the token sequence logged on 3 runs has no repeat and no gap; the game draws (about 6,000 vertices per frame).

## IOS: `/dev/fs` ReadDir forms

- Symptom: a host write fault in `wp::ios::update()`, at guest physical offset 0x15333332.
- Cause: `ReadDir` (ioctlv 4) has a count-only form (1 input, 1 output) and a listing form (2 inputs, 2 outputs). The engine always used the listing layout, so in the count-only form it read the count address past the vector array.
- Change (`engine/src/ios/ios.cpp`): the output count selects the form; in the listing form the count is clamped to the maximum, as IOS reports the entries it copied.
- Verified: `test_read_directory_forms` in `tests/engine/runtime_tests.cpp` covers both forms and checks that nothing past the vectors is written.

## Recompiler: jump tables

- A base built by `lis` far from its `addi` (0x8037eb00 sets `r27` at the start and uses it 660 instructions later) was not found because the constant search looked back only 256 instructions. Jump table bases are now searched back to the start of the function.
- Tables without a bound may contain null entries (0x8042c5e0: six handlers, two zeros, two more handlers). Null entries are skipped instead of ending the table; the table still ends at the first non-null word that is not code.
- `tools/classify_jumps.py`: of the 384 indirect jumps left, 382 load a function pointer, one is `__ptmf_scall` and one jumps to an argument; none is an undetected switch.

## Boot straight to the title

- `nsNile::nsSequence::SequencerBoot` (update 0x801314b0, state at +4) plays Strap (scene 1, the Wii Remote strap notice), then CheckData (scene 3) only when the save is not ready, then Hold (scene 2, how to hold the Wii Remote), then returns 2 for the title. Scene table: 0x80469cc8, 0x68 bytes per scene.
- Strap and Hold cannot be skipped: Strap starts loading the save and the common data, and skipping Hold crashed the sound thread right after the title started (`DVDConvertPathToEntrynum` called with 7 as the path).
- Change: the update is replaced (`SequencerBootUpdate` in `hle_functions.csv`, handler in `game/game.cpp`). It runs the same scenes, but while Strap and Hold run the engine keeps the screen black (`gx::set_screen_hidden`, no copy to the shown frame) and player 1 taps A (`input::set_auto_confirm`). CheckData is left visible, as it can ask about the save.
- Verified: headless runs at 1x; frames are black until the title, which appears with its intro; no crash in 40 s.

## IOS reply speed

- Most of the boot time was the game copying data to `/tmp/Pokemon/*.dan` on the NAND, with replies delayed like the real NAND (about 5 ms per 16 KB cluster written).
- Change (`engine/src/ios/ipc.cpp`): reply delays are divided by 100; replies keep their order.
- Verified: the title shows 5.3-5.4 s after start instead of 15.4 s (3 runs).

## Removed Wii Party features

- Mii loading, the controls hint window, the sideways grip per minigame and the instruction-screen watch are gone, with their settings (`input.auto_grip`, `input.controls_hint`, `system.skip_notices`) and launcher pages. The grip is now a single per-game value (`sideways_grip` in `game::Description`).

## D3D11: freed texture bound by a draw

- Symptom: crash in the graphics driver (NVIDIA `nvwgf2umx.dll`, and in WARP too) about 1.5 s after the Reshiram and Zekrom scene, when the player first controls Pikachu.
- Found with the Direct3D debug layer (`WP_D3D_DEBUG=1`, Windows optional feature "Graphics Tools"): `PSSetShaderResources` in `flush_pending` received a view that was already released.
- Cause: `draw` looks up the texture of each map in turn. A later lookup that hits the same cache entry with other parameters recreates it and releases the view an earlier map had just returned. The texture cache key also mixed its fields with XOR (address << 20 against height << 32), so different textures could share an entry.
- Change (`engine/src/gpu/gx_render.cpp`, `engine/src/gpu/gx_render_common.cpp`): draws hold their own reference on each view until the batch is replaced; the key (`gx::texture_key`) puts each field in its own bits (address 24, width and height 10 each, format 4, mip levels 4, palette 12).
- Verified: `test_texture_keys`; with the debug layer, which stops on any invalid call, 3 runs of 200 s through the scene into gameplay; one run of 5 minutes on the NVIDIA driver. One run of 200 s crashed once in `save_context` from the decrementer, at 131 s; it did not happen again in 4 more runs and is not explained yet.
- Crash reports now list the host frames of the crashing thread (module+offset) and stderr is unbuffered, so a redirected report is complete.

## EFB copies to a null buffer

Some of the game's render targets are created without memory (their buffer pointer stays 0, see 0x80070fc0), yet
the game still copies the screen into them every frame (GXCopyTex from 0x80071204). Dolphin keeps EFB copies on the
GPU by default, so nothing happens there. Writing them back to RAM overwrote the OS globals, and a full-screen copy
reached the interrupt table at 0x80003040: the next interrupt jumped to texture data and the game crashed at the
start of gameplay. Copies whose destination is below 0x80004000 now stay texture-only.

## Frame pacing: 15 fps phases

The game does not pace itself on vsync. Two periodic OSAlarms run from the same instant: a 60 Hz one samples the
inputs (0x8002c9c0) and a 30 Hz one wakes the main loop (0x8002d1d4). Each loop runs one logic step per queued input
sample and expects two; with three it skips the next render (0x8002cfd0).

Both alarms are due together every other tick. On the console the second decrementer exception fires before the
woken thread runs any further. Here it was delivered at the next poll, about a thousand poll sites later, so the
loop sometimes read one sample, then three, and the skip kept itself going: 15 fps for tens of seconds after any
hitch. In that mode the game also copied to null buffers (see above). A decrementer due within 100 us is now
delivered at the very next poll site, even when interrupts were masked at the first try.

## EFB copy read-backs

Draw done and tokens wait for pending copies to reach RAM, like the console. Not waiting was tried and reverted:
a late write can land in memory the game has already reused. Dolphin needs RAM copies for this game only for the
snapshot feature. All pending copies share one staging atlas, so a frame costs two GPU synchronisations instead of
about forty. 
## Specialised pixel shaders

The generic pixel shader interprets the TEV stages for every pixel. At 6x that took about 8 ms of GPU time per frame
in the first cave, and with the read-back wait the game could not hold 30 fps (24 fps measured). Each TEV
configuration now gets its own variant of the same source, with the stages, swap tables and indirect setup turned
into constants so the compiler folds the interpreter away (about 30 ms to compile, on a worker thread; the generic
shader draws until the variant is ready). Same scene at 6x: 30 fps. `WP_GENERIC_SHADER=1` turns the variants off.
The OpenGL renderer still uses the generic shader only.

## 60 fps

The main loop is paced by two periodic alarms in the timer manager at 0x804BA4D8 (0x70 bytes each): input at
59.94 Hz and frames at 29.97 Hz. It runs one logic step per input sample and skips the next render when it had
more steps than `frame period / input period`. `video.fps=60` (`WP_FPS`) writes the input period into the frame
alarm's pending period (0x804BA588), the same change as Dolphin's 60 fps code; the loop applies it by itself.
Three more things were needed:

- The game picks its render mode from `VIGetTvFormat`. Boot wrote PAL (1) to 0x800000CC even with `pal60=1`, so the
  game ran a 50 Hz mode and could not present more than 50 frames. A PAL console set to 60 Hz announces EURGB60 (5).
- Buffers copied again every frame (bloom and similar, about 39 copies per frame in the first cave) stay on the
  GPU. Reading them back to RAM held that scene at 42 fps. A buffer that was not copied in the last two frames is
  still written, so a one-off copy such as a snapshot reaches RAM. The photo (640x350, RGBA8) is taken into
  `0x8148DBA0`, a buffer an effect also fills every frame (640x456 halved, another format): a copy only counts as
  copied again when it has the size and format of the one before at that address. Without that the photo never
  reached RAM and the game saved stale memory, black with pink streaks, at 60 fps as well as 120.
- Movies (`Movie/*.mo`) advance one picture per rendered frame. While such a file is being read the frame alarm
  keeps the 30 fps period.

Measured in the first cave, D3D11: 59.9 to 60.0 fps at 3x and 6x.

## 45 and 90 fps

- 45 fps (`video.fps=45`): the frame alarm gets 4/3 of the input period. The main loop skips a render when a frame
  ran more logic steps than `frame period / input period` (0x8002cfd0, an integer division), which is 1 here while
  every third frame runs two steps, so the game fell back to 30 fps. Once the timer manager has armed the alarm at
  4/3 (0x8000efa0), its period at +0x38 and its pending period at +0x40 are both set to twice the input period plus
  one tick: the check then allows two steps, the alarm keeps firing at 4/3, and nothing re-arms it since both fields
  agree. The skip check is the only reader of +0x38 that uses the result (0x8000f1e0); any other setting writes
  another pending period, which re-arms the alarm. The logic still runs 60 steps a second, so things move one, one,
  then two steps between images.
- 90 fps (`video.fps=90`, Direct3D 11, a screen of 85 Hz or more): the 120 fps interpolation with images every 2/3
  of a game frame. For a game at 60 fps the images fall 2/3 of the way to a frame, then 1/3 of the way to the next
  one followed by that frame: every other game frame is not shown. `pacing::plan_frame` carries the place of the
  next image from one frame to the next. It starts again on a shown frame whenever the images per frame are a whole
  number (120 fps, or 90 fps with 30 fps content), and after a frame that got fewer images than planned.
- Headless at 3x in the first zone: 45.0 fps at 45. At 90 every game frame got its image, and the game's own rate
  matched 120 fps in runs made one after the other.

## Boot hang in `__AI_SRC_INIT`

0x802b5290 times the audio sample counter with interrupts masked and retries until the measure is plausible. The
quick polls added for prompt alarm delivery skipped `audio::update()` for 4096 polls at a time whenever an alarm was
overdue during that function, so the counter never moved in time and about one boot in three hung on a black screen.
The quick-poll allowance is now renewed only by a poll that finds interrupts unmasked.

## Tiny draws in the open zones

The first open zone sends close to a million draws a second, nearly all under 16 vertices (grass, foliage), and
most of them change only a matrix in between. Recomputing the pixel-shader constants, textures and samplers for
each one held the GPU thread at about 52 fps whatever the resolution. `render::draw` now keeps a serial of the BP,
konst and XF control register writes (`register_serial`): when it, the texture epoch and the scale are unchanged
since the previous draw, the vertices join the pending batch directly. Splitting the vertex work of one draw
between threads gained nothing, since the draws are too small. Measured: 59.9 to 60.0 fps at 3x and 6x.

## Pointer, shake and options menu

- The mouse and the gamepad no longer aim all the time: the pointer follows the mouse while Ctrl is held
  (`keys.pointer`) and the right stick while L3 is held (`pad.pointer`), both remappable.
- A sharp jolt of a gamepad with a motion sensor (more than 0.9 g away from rest) holds the Wii Remote shake for
  250 ms.
- F1 opens the options menu (see "Options menu, play time and performance overlay"). F10 no longer does anything.
- The title screen and cutscenes are pre-rendered 30 fps movies, so they stay at 30 fps (see "60 fps").
- Discord shows the game when `system.discord` is on and `discord_application` in the game description holds an
  Application ID; it talks to the local client over its IPC pipe and retries every 15 s.

## Options menu, play time and performance overlay

- The F1 menu is drawn on the CPU into one image (`options_render_stb.cpp`, on the shared canvas of `canvas.cpp`) that
  the renderer shows over the game. Its size follows the window height: `unit = clamp(height / 40, 14, 40)`, the panel
  is 60 units wide, and its orange edge, `max(3, unit * 3 / 5)` pixels, is the outline polygon drawn once offset in
  orange and once in place in the panel colour. Tabs sit in a column on the left, skewed by 12 degrees; rows on the
  right, then a help line and the key hints.
- Palette: panel `#F8F8F6`, orange `#F27D0C`, ink `#3B3531`, selected row `#FFEBD2`, red marker `#E2343B`.
- Font: Nunito ExtraBold and Black (SIL Open Font License, `third_party/fonts/nunito`), embedded in the executable with
  `.incbin` so the menu looks the same everywhere. The performance overlay uses Consolas, or DejaVu Sans Mono.
- Tabs: Screen, Game, Sound, Controls, Language, Advanced. The Controls tab's first row switches between keyboard and
  gamepad bindings and is back on the keyboard each time the menu opens. Each row's help text is `help.<key>` in
  `ui_text.cpp` (see "Controls" for the key and button rows).
- Play time: counted while the game runs, saved as `play_time=<seconds>` in `game/stats.ini` every minute
  and on exit (written to a temporary file, then renamed), shown in the Game tab.
- Gamepads keep working while the window is in the background; the keyboard and the mouse count only while it has the
  focus.
- Performance overlay (F2, `keys.overlay`, or the Advanced tab): levels minimal, standard and full, in any corner, a
  margin of 1.4 % of the window width away from the edges. F2 does nothing while the menu waits for a key. Screenshots
  read the game frame, so neither the overlay nor the menu shows in them.
  - The GPU thread publishes the figures once a second from its statistics block in `gx.cpp`. CPU, GPU and RAM come
    from a sampler thread that runs only while the overlay is on: the GPU counters can take tens of milliseconds to
    read, which would hold a frame back on the GPU thread.
  - CPU %: the whole system (GetSystemTimes). Game: the share of the second a game thread was running (100 minus the
    guest idle share; the host thread's own time is useless, it spins while the game idles and read near 100 at a
    steady 60 fps). GPU thr.: the share of the second the GPU thread was not asleep on an empty queue, so time blocked
    in the driver counts. Either one near 100 is the limit. There is no real-frame-rate line any more.
  - GPU %: the busiest adapter's 3D engine (PDH `GPU Engine(*engtype_3D)`), the figure Task Manager shows. GPU ms: this
    process's share of that engine times the shown frame time. A timestamp pair around a frame would also count the
    idle gaps between the game's command bursts.
  - VRAM: this process's local video memory (IDXGIAdapter3, Direct3D 11 only). RAM: the process's working set.
  - Late/s: real frames shown with no image interpolated before them, at 120 fps.
  - Temperatures are left out: Windows has no common way to read them without a vendor library or a driver.
  - On Linux and macOS the system figures show a dash.

## Controls

- What each Wii Remote button does, held sideways (always the case in PokÃ©Park 2), from the tutorial texts in
  `Message/EU_English/Fd0801.msbt`, `Fd0101`, `Fd0102` and `Fd0302`. Their button icons are MSBT tags of group 2:
  2.0 the + Control Pad (move), 2.1 call or switch a partner, 2.2 turn the view to where Pikachu faces, 2.4 open the
  PokÃ©Park Pad, 2.5 take a Photo, 2.6 the 1 button (tap: Dash; hold then let go: Thunderbolt), 2.7 the 2 button (jump,
  talk, pick up, confirm). Shaking the Wii Remote uses Iron Tail, and Shake Sprint during Chase. Checked in game
  with the keyboard: + opens the Pad and âˆ’ takes a Photo.
- Default keys (US positions, moved to the local layout by `local_defaults`): 2 = Space, Enter; 1 = left click,
  Backspace; shake = right click; A = E; B = C; + = Tab; âˆ’ = P; HOME = H; tilts = J, L, I, K; swing = wheel, T, G;
  pointer = Ctrl. The number row is left free: on AZERTY its keys write `&` and `Ã©` and cannot be reached while
  playing.
- `upgrade_bindings` runs once at start. It first moves US letters to the local layout (`unlocalized`), then, while
  `input.bindings_version` is below 2, gives the new default to every action whose stored keys are exactly its old
  default (US or local letters), unless one of the new keys is held by an action that keeps its keys: one the player
  changed, or one held back this way, repeated until nothing changes (with E on another action, A keeps Enter, Space
  and left click, so 1 and 2 keep their old keys too). Then it stores version 2, so a player who later puts an old
  default back keeps it. `pad.*=auto` becomes the real default.
- The mouse aims only while the pointer key is held, so it never points by accident. While it is held, a mouse button
  bound to 1 sends A: on a Wii, A clicks what the Wii Remote points at, and 1 would go back.
- Gamepad bindings (`padmap`): each action holds combos, a combo being a bit mask of `Physical` buttons that must all
  be held. Text form: combos separated by `,`, buttons of a combo by `+`, in `Physical` order; `none` is no button;
  `auto`, an empty value or only unknown names give the default. Defaults depend on the grip:

  | action | sideways | upright |
  |---|---|---|
  | a | north, right_shoulder | south, right_shoulder |
  | b | right_trigger | east, right_trigger |
  | one | east, west | west |
  | two | south | north |
  | plus / minus / home | start / back / guide | same |
  | shake | left_trigger | same |
  | swing up / down | left_shoulder+dpad_up / left_shoulder+dpad_down | same |
  | tilts | left_shoulder+right_stick_left / right / up / down | same |
  | pointer | left_stick | same |

  A combo that is a strict subset of another combo held at the same time does nothing, so LB + D-pad up swings
  without also pressing what the D-pad alone is bound to. The defaults are explicit button lists, so a rebinding
  now works in the sideways grip too (the old code dropped every custom A/B/1/2/+/âˆ’/HOME button when sideways).
  Giving a button to an action in the menu removes it, as a single-button combo, from the other actions.
- The gamepad pointer exists only while the pointer combo is held, and starts from the centre of the screen.
- Button names (`control_names`): the gamepad worker records the family of each pad from `SDL_GetGamepadType`
  (Xbox One/Series, Xbox 360, PS3, PS4, PS5, Switch Pro and Joy-Con; anything else counts as Xbox) and publishes the
  family of the last pad whose buttons went from released to pressed, or of the first one connected. Without a pad,
  or before the worker runs (tests), names are Xbox ones. Switch face buttons are named by position: the bottom one
  is B. D-pad and right stick directions, PlayStation shapes and Switch stick presses are translated labels.
- Key names: mouse buttons, the wheel and keys that write no character are `key.<name>` labels; the numpad is
  `key.numpad` and the symbol; other keys show the character this keyboard layout writes (`MapVirtualKeyW`,
  `MAPVK_VK_TO_CHAR`), so `&` on AZERTY. An empty binding shows an em dash. `settings.ini` keeps the English names.
- Help lines: a key row shows `help.keys.<action>`; a gamepad row shows the same text, except motions
  (`pad.motion_help`) and the pointer (`help.pad.pointer`). The row being captured shows how to capture instead
  (`help.keys`, `help.pad.capture`): the help area holds two lines, too few for both.

## Installer

- `tools/make_setup.py` makes `build/dist/WondersBeyondRecomp-Setup-Windows.zip`: Python 3.12.10 from its NuGet package and
  the Tcl/Tk part of the python.org installer (both checked against a SHA-256), the scripts of `installer/`,
  `installer/sources.zip` and `Setup.exe`. `sources.zip` holds the files
  `git ls-files` lists for the engine, the recompiler, `game`, `third_party`, `tests` and
  `tools/fetch_sdl.py`. Nothing of the disc is tracked, so nothing of it is in the zip.
- Why a zip and scripts: Smart App Control refuses an unsigned installer executable, and nothing of ours is signed.
  Every program in the zip but the launcher is signed by the Python Software Foundation; `make_setup.py` stops if one is not, which is
  why pip, Tix and the nmake helper are left out.
- `python/Setup (if blocked).exe` is `pythonw.exe` under another name, which keeps its signature, in the folder of Python so that the root of the zip only shows `Setup.exe`.
  Its `._pth` file gives it its module folders and turns `site` on, and `site` imports `installer/sitecustomize.py`, which
  opens the window when the program is not named `python*`. Its icon in the file explorer is therefore the one of
  Python; the window and the taskbar show the game icon.
- `Setup.exe` is `installer/launcher.c`, built by `make_setup.py` with the game icon: it only starts
  `Setup (if blocked).exe`. It is the one program in the zip without a signature, so Smart App Control may refuse it
  on a PC that has it on; the signed program beside it is the way in there.
- Install steps (`installer/steps.py`): unpack the sources, download the tools that are missing (nodtool, CMake, Ninja,
  MinGW, each checked against a SHA-256), verify and extract the disc, check that it is `S2LP01`,
  `tools/fetch_sdl.py`, `recompiler/recomp.py`, `recompiler/dsp/recomp_dsp.py`, configure, build, copy the game and its
  DLLs to the install folder, add the shortcuts. Any other disc is refused after extraction and its files removed. The
  bundled Python runs the scripts, so none is downloaded.
- A disc image that Redump does not list is accepted: WBFS and other trimmed copies never match, and nodtool marks
  that line with the same cross as a real checksum error. Only a cross on another line refuses the image.
- The MinGW package is a self-extracting 7z archive. `tar.exe` of Windows unpacks it, so that unsigned program is
  never run.
- The game reads its files relative to the working folder, so the executable is copied to the install folder itself
  and the shortcuts set that folder: a double click on it works too.
- The window (`installer/setup.py`, tkinter) only installs: disc, folder, shortcut, progress, in English. The log is `logs/install.log` in the install folder.
- The default folder is `%LOCALAPPDATA%\WondersBeyondRecomp`, or the system drive when that path has accented
  letters. Paths with them are refused, MinGW and Ninja being unreliable there.
- No resource compiler: `windres.exe` of the downloaded MinGW is refused by Smart App Control. `engine/cmake/icon.cmake`
  reads the `.ico` at configure time and writes a source file whose assembly lays out the `.rsrc` section (icon group
  1 and its images), which the linker takes as the resources of the executable.
- The previous installer (`launcher/`, a single executable with settings pages, with `tools/make_appimage.py` and
  `tools/make_offline_pack.py`) was removed on 2026-11-12; it is in the history up to commit 547ea1e.

## Color settings

- Screen tab: brightness, contrast and gamma from 50 to 150 %, saturation from 0 to 200 %, in steps of 5, 100 % being
  the original picture (`video.brightness`, `video.contrast`, `video.saturation`, `video.gamma`). "Original colors"
  (`video.colors_reset`) puts the four back to 100. Values outside the ranges in a hand-edited file are clamped.
- The present shaders (`adjusted` in `kPresentShaderSource` for Direct3D 11, `kPresentFragment` for OpenGL) apply,
  in this order: contrast around mid grey, brightness as a factor, saturation toward the Rec. 709 luma, a clamp to
  0..1, then `pow(c, 1 / gamma)`. Direct3D 11 reads the four values from a 16-byte constant buffer on slot b0, which
  every game draw batch binds again for its own constants. With smoothing on, the step runs after the 4x4 average.
- The F1 menu and the F2 overlay are drawn after that pass, so they keep their colors.
- F9 reads the game frame before presentation, so `color::apply` runs the same steps on the CPU before the PNG is
  written. The headless frame dumps used by the tests stay unadjusted.

## Free camera

- The view in the field comes from a camera of the class whose vtable is at `0x80435278`; its update (slot 7,
  `0x80024EE0`) writes the eye at +0x0C, a copy at +0x18 (from the base update `0x80019B00`) and the target at +0x28.
  `CameraDolly` (`0x80434210`) also runs but does not drive the picture. Pikachu's moves follow the shown eye, so
  turning it is enough for walking toward the screen's direction.
- The update reads its previous eye back, so the eye written by the free camera is swapped for the game's own one
  before each call; without that the game camera spins.
- `hook_functions.csv` (address, name) makes the recompiler rename the original body to `f_hooked_<address>` and emit
  a wrapper calling `game::hook(name)` with the original. `FieldCameraUpdate` runs the original, then
  `free_camera::Orbit` keeps its own yaw and a pitch offset over the game's distance and elevation (elevation kept
  between -0.2 and 1.3 rad). It starts from the game's view, and starts over when the camera object changes, after
  half a second without updates or when the target jumps more than 40 units. Wii B eases it back to the game's yaw.
- Mouse: raw input on Windows, relative mode with SDL. The cursor is held at the window's center while the free camera
  drives the view and the window has focus, the F1 menu is closed and neither Ctrl (`keys.pointer`) nor the mouse
  motion key is held. 0.0016 rad per count at 100 % (0.004 was found too fast); the stick turns 2.6 rad/s sideways and 1.6 rad/s up and down at
  100 %, with a 0.2 dead zone. Settings: `camera.free`, `camera.mouse_speed`, `camera.stick_speed` (10 to 300 %),
  `camera.invert_y`.
- `audio.volume` goes from 0 to 300 % and the output is multiplied by a 300th of it: 100 % is a third of the game's
  own level, which was found too loud, and 300 % is that level, so no value can clip.
- The game aims about 4.1 units above Pikachu, but its target trails him by up to 8 units; seen from another angle that
  lag puts him off center. The free camera asks for the followed position the same way the update does (`0x8001A610`
  for the handle, `0x800C6710` for "none", `0x800C5D80` for the position, `0x800C64C0` to release it, called from the
  hook on a scratch frame below the stack pointer) and aims 4.1 units above it, only the height being eased. Its eye
  and target are swapped back for the game's own before each update.
- The eye stays 1.5 units above the ground. The game's camera has a vertical probe, `0x800277B0` (point, reach
  above in f1, reach below in f2, true when the field collision is met); the point met is then read from the
  collision manager (`0x80076E90` returns it, `0x8012FE60` returns the point). The hook probes from 3 units above
  the higher of eye and target down to the clearance below the eye. The collision only covers where one can walk:
  past its edge (toward the sea on the beach) the height of the followed character's feet stands in for the ground,
  or the slope from those feet to the last ground found on the way to the eye (three samples) when that is higher.
  The same collision is then asked along the segment from the target to the eye (`0x8012DD40`: manager, start,
  start again, extent, 1, 0, 0, 0): what it meets brings the eye 1 unit in front of it, never closer than 2 units,
  and the eye eases back out afterwards. Scenery drawn without collision can still be entered.
  While the player pushes down the orbit's pitch stops at the ground (`Orbit::rest_on`), so pushing up answers at
  once.
- The lowest height allowed for the eye is eased (16 per second upward, 8 downward) instead of applied at once. The
  probe under the eye meets rocks, ledges and wall tops from one frame to the next: with a low camera turning near
  them, the eye jumped up to 5 units up or down in a single frame (83 jumps over 0.4 unit in a 2.5 minute walk on
  the beach with the camera spinning 0.45 rad under the game's pitch; 36 after, the largest 1.5, each one a step of
  the easing). On a slope the eased height trails by speed / 16, under the 1.5 clearance up to about 20 units a second.
  The test used two temporary switches (a camera log line, a forced spin) that are not kept.
- Not seen yet in game: scenes that move this same camera themselves would be overridden too.

## Indirect maps left unbound

`render::draw` bound only the maps named by TEV stage orders. A map read only by an indirect stage, such as the
normal map of the dash shockwave (64x64 IA8), stayed unbound and sampled as zero, the largest offset: the ring
showed the scene shifted by 128 texels. `used_maps` now adds the maps of the indirect stages (BP 0x27).


## Frame interpolation (120 fps) â€” work in progress

Status on 2026-11-10, not committed yet. 60 fps is fine. 120 fps works in the title menu but not fully in game.

### Design (D3D11 only, `engine/src/gpu/gx_render.cpp`)

- Each frame's commands (batches, EFB copies, clears) are recorded. At the XFB copy the frame is matched to the
  previous one and replayed `between` times with lerped clip positions (MOVED vertex stream) into
  `interpolated[kMaxBetween=3]`. `between = max(1, round(g_period*120)-1)`, so 30 fps content gets 3 images.
- Matching: first an exact key (draw key ^ array base or display-list address, `DrawSource.exact`, set in `gx.cpp`
  only for indexed draws or inside display lists), trusted with no distance check; then the loose key, rejected
  if a vertex moves more than 0.5 NDC. Camera cut when `moved_too_far > 8 && moved_too_far*3 > matches`.
- EFB copy targets get "twins": replays draw into a copy of the target taken at its first copy in the frame, so the
  real texture (shadows, bloom) is not overwritten. `note_copy_target`, `prune_twins` every 600 frames.
- EFB snapshot: `g_efb_after` is saved after the real XFB copy and restored after the replays; replays start from
  `g_efb_before` (previous frame end). Replays need `g_efb_before_valid`.
- Present queue: show the oldest due image, skip only when 3+ are due, chain new images after the last queued one.
  `g_slot = clamp(g_period/(n+1), 2 ms, 25 ms)`. The GPU thread polls `render::service_presents()` with
  `sleep_for(1ms)`: on MinGW `condition_variable::wait_for(1ms)` lasts 15.6 ms, which held the title at 95 fps.
- Adaptive skip: interpolation only when `gx::gpu_backlog() < 256 KB`. Not validated: the measured backlog was
  0 KB in loaded runs, so this metric may be the wrong one.

- Replays after the signal (2026-11-10, built, not tested in game yet): the frame copy only stores its arguments
  (`g_frame_end`); `render::finish_frame()` draws the replays once the draw-done or token that follows has been
  raised, or when the GPU queue runs empty, and at the latest before the next draw, copy or present. The clear that
  comes with the frame copy is held back and run after the replays. The game no longer waits for the replays, and the
  backlog test now runs when the game has had time to queue the next frame.

- User test of the above: pause menu noise still there, then every texture flickers after leaving the menu. Their
  log shows the process growing from about 1 GB at 60 fps to 7â€“10.8 GB at 120 (scale 6), falling back 10â€“20 s after
  recording stops (the twin prune), and hundreds of EFB write-backs a second at 120 against none at 60.
- Twins only where needed (2026-11-10, built, not tested in game yet): a twin (two textures and 1 + `between`
  full copies per frame) is now made only for a target the frame sampled before its first copy into it. Other
  targets are filled before they are read, so replays copy straight into the game's texture, which then holds the
  last replay's image until the next frame fills it again. To check in the next log: the "memory" lines at 120.

- User test of the twin change: memory is fixed (1.6â€“2.3 GB at 120 instead of 7â€“10.8 GB) but the pause menu, shadows
  and footprints are as before. The log still has 50â€“230 write-backs a second at 120 once the player moves (GPU
  thread 85â€“92% busy) and none at 60 (80% busy): the game copies to addresses it did not use in the last two frames.
- Hypothesis: the game frees a frame's buffers on its token interrupt and takes the next frame's on its frame alarm.
  When the GPU thread reaches the token after the alarm, the game takes fresh buffers, every non-recurring copy is
  written back with a GPU wait, the thread gets later still, and in the pause menu the heap runs out.
- Timers wait for the token (2026-11-10, built, not run at all yet, changes timing at 60 fps too): `push` notes the
  token of each BP 0x48 the game writes (`g_token_sent`), the GPU thread the one it reached (`g_token_done`);
  `gx::frame_owed()` makes `poll_interrupts` hold the decrementer while they differ or the interrupt is not delivered
  yet, 100 ms at most. A frame whose token made the timers wait gets no replays. The "memory" log line counts the
  holds. Pass criteria: boots, 0 write-backs a second in the zones at 120, few holds at 60.

- User test of the timer hold (scale 6, first open zone): 60 fps fine (0â€“3 holds a second, far characters pop in
  and flicker, not known whether that is new). At 120 the write-backs are gone (0â€“36 a second, the pause menu looked
  fine) but only 75 fps is shown: 15â€“25 holds a second and one frame in four gets its image. Shadows (the one under
  Pikachu) and footprints still look wrong in the images, so that part is the interpolation itself, not the buffers.
- After that (built 18:44, not run): the late flag is tied to the token (`g_held_token`) so one hold skips one
  frame's images, not several; a "replay" log line gives frames, frames with images, late frames, ms per frame spent
  on images and draws per frame.
- Next ideas. Cost: the replays do three full EFB colour+depth copies per frame (save after, load before, load
  after), 250 MB per frame at 6x; drawing replays into a second EFB would need none when the frame starts with a
  full clear. Shadows and footprints: draws sent inline through the FIFO are paired by order under the plain key, so
  a new footprint shifts every pairing, and an unmatched decal stays at the new frame's place on ground drawn at the
  halfway place (depth mismatch). To see it, dump an interpolated image in a headless run.

- User test of 18:48: still 75 fps. "replay" lines in the zone: 60 frames, 18 with images, 22 late, 0.6â€“0.7 ms per
  frame for the images (about 2.2 ms per replay); 19,800 draws per frame, 15,030 exact, 57 by order, 4,740 unpaired
  (most likely culled draws, which never pair), 0â€“10 unpaired inline. In the title menu, with almost no load, 20 of
  60 frames were "late" too: most holds are the one poll between the token and its interrupt, not a late thread, and
  each late frame also cost the next one its images (no saved EFB).
- Built 19:08, not run: a frame is late only when the timers waited over 1 ms for a token the thread had not
  reached (the "memory" line counts both kinds of hold); a late frame still saves its EFB; replays draw batches with
  depth test on and depth write off with a rasterizer depth bias (-64, slope -2), a guess at the blinking shadow.
- User description: the shadow under Pikachu blinks and the footprints tremble, only above 60 fps.
- Built 19:20 (not noted at the time): draws of a mesh drawn many times follow their camera's motion (`find_cameras`,
  `kByCamera`), fitted from the pairs whose key is drawn once in both frames. User test: 90 fps at most, shadow,
  footprints, far PokÃ©mon and the sea still blink.

- 2026-11-10 evening, found with headless image dumps (see "Image dumps" below):
  - The decal depth bias (-64, slope -2) was the sea's blue flicker: the sea and the wet sand layers (depth test on,
    write off) passed over the sand on every interpolated image, a bluer image twice per frame. With the bias off, an
    unmoved replay matches the real frame exactly (0.00 mean difference). Removed.
  - Ghost footprints and the darker shadow: each frame first draws the footprints and shadows from above into a
    ground decal texture (draws 0â€“29, projections `c36825a9`/`0fe8fbc9`, no fitted motion), then copies it. The
    footprints are one mesh drawn many times, paired by order, and the order shifts when one is added: each slid
    toward its neighbour, ghost footprints on the images in between. A pair that is not sure under a projection with
    no known motion now stays put (counted as "rejected" in the replay line).
  - Depth agreement: the camera fit is very good on the scenery (screen 1e-5, depth 2e-7 of z/w), but 2e-7 is already
    a few 24-bit depth steps, so a footprint following the camera and ground lerped by its pair disagreed. Now every
    pair under a known camera that does not move in the world (`moves_in_world`: limits 5e-4 screen, 1e-6 depth,
    points on screen only) follows the camera exactly like the multi-drawn meshes, and a thing that moves is
    interpolated in the world: its previous place seen by this frame's camera (`g_forward`, the inverse fit), lerped,
    then the camera's motion. Lerping clip positions directly put a moving decal off the ground whenever the camera
    turned, since the error is t(1-t)Â·Î”MÂ·Î”P.
  - Result on the beach run: blinking pixels per image (outside the range of their two real neighbours by more
    than 12) 0.53â€“0.60 % before, 0.29 % after; the remaining ones are the electric aura, Pikachu's legs and the
    water line on them. Far PokÃ©mon and cliffs measured stable.
  - Not changed: copy targets written by the replays keep the last replay's content (a non-twin target the next
    frame samples before copying would show it). Restoring them changed nothing measurable once the bias was gone.
  - Performance (headless, scale 6, beach): game at 56â€“57 fps with 120 on against 60 with 60, GPU thread 80 % against
    63 %, images 2.9â€“3.4 ms per frame. Measuring further was stopped: the user was playing another game at the time,
    and a run at the same settings fell to 30 fps.
- User test 21:12 of that build: no texture blinks any more, 94â€“98 fps shown. Its "replay" lines: 54â€“57 frames,
  8â€“16 late (a late frame gets no image, hence under 120), 1.8â€“2.2 ms per frame for the images, of which pairing
  1.5â€“1.9 ms, positions 0.1, commands 0.2; GPU thread 90 % busy. Far PokÃ©mon still vanish and come back at a
  distance; the 18:xx test above saw far characters pop in at 60 fps too, so it is likely not the interpolation.
- Pairing is now split over helper threads (`parallel_for`, up to 3 helpers when the CPU has more than 4 cores): the
  close-enough checks of `match_by` run after the key pass over all candidates at once (same result: the trusted pass
  never used them to choose, the loose pass only rejects), and the per-draw choice (camera, partner, moving in the
  world) runs per draw. Headless with another game running: pairing 2.0 â†’ 1.1 ms. Running the pairing on its own
  thread from the frame copy on gained nothing: the token right after the copy calls `finish_frame` at once, so the
  GPU thread waited the whole time. Blink rate unchanged (0.26â€“0.30 %, one run 0.54 % with Pikachu in the water).
- The "replay" log line now splits the image time: pairing, positions (filling the moved buffer), commands.
- Far PokÃ©mon that vanish depending on the camera angle (the pink one on the islet off the beach): the same on a
  real Wii and on Dolphin (tested by the user), so it is the game's own behaviour. Checked on the way: the renderer
  only drops back faces (never a whole model), the game reads no PE pixel counters and makes no depth copies; it
  stops sending the model itself.
- User report: a thin grey/blue line on the ground for an instant. Found in dumps as a straight blue line on wet
  sand in one image, during a camera turn. Cause: the scene under the main projection moves as two groups when the
  camera follows Pikachu (near ground and far scenery, likely drawn with another view), the fit took the larger
  one, and up to 7,000 draws of the other counted as moving; the ones close enough to the first motion did not.
  Neighbouring pieces were then placed by two formulas (camera path, world lerp) and the sea showed between them.
  Changes: `fit_motion` has two more rounds whose limit follows the kept pairs' error (static error now about 1e-6
  instead of 1e-3); the pairs the first motion leaves out get a second motion under the same projection
  (`CameraMotion::second`), used by a draw that moves under the first but not under the second; standing still is
  within 2e-3 of the screen and 2e-5 of the depth range (`kStillScreen`); samples are three points a draw for
  the first 2,048 points, then one, up to 8,192, so the far scenery is sampled too. Whole script: at most about
  700 draws a frame count as moving (was up to 7,000); a second motion is used in about 12 % of the frames. Beach
  blink rate unchanged (0.25â€“0.35 %). Pairing about 2 ms headless with another game running.
- 2026-11-11, bug recorder captures from the user (`_between` images only): thin lines of background along polygon
  edges of Pikachu and Piplup standing, and of the ground while the camera turns. Same family as above: the still or
  moving choice was per draw, so a vertex shared by a still draw (camera path, on the graphics card) and a moving
  draw (world lerp) got two positions, up to (1 - t) Â· 2e-3 of the screen apart. The choice is now per vertex inside
  a moving draw (`stillness`), under the first motion then the second.
- Second round the same evening (captures 22:16 and 22:19): lighter on the models, still long lines on the ground of
  the candy zone while the camera moves. Three per-draw choices were left: a draw of more than 8 positions was judged
  on 8 of them, a draw still under the second motion followed it as a whole, and a point off screen followed the
  camera in a still draw but was lerped in a moving one. Now `moves_in_world` tests every position, the second motion
  is only chosen per vertex, and an untested point follows the first motion everywhere: a paired vertex under a known
  camera gets the same place whatever draw it is in. Headless beach run: no crash, pairing 1.8 ms and positions
  0.3 ms as before. Not confirmed in game; a pair next to an unpaired draw can still differ.
- Shadow wobble during a jump (capture 22:13, measured on the images): on the images in between, Pikachu's shadow is
  about 0.5 pixel (at 960 wide) behind the middle of its two real places, near or just past the previous frame's
  place, so it steps back and forth. The shadow goes through the ground decal texture, whose draws stay put
  ("rejected"). Cause not found yet; look at what the twin of the decal target holds during a replay.
- Far castle of the candy zone trembling (capture 22:47, then `still.txt` dumps from the user's save, frame 2400):
  the glow quads on the castle windows and gate lose the depth test against the wall in triangle-shaped patches.
  The castle sits at z/w 0.99994-0.99999, where the whole tower spans about 50 depth steps and the glows are less
  than a step in front of the wall. `project` built the depth as z * z_scale + w * z_offset, two terms near
  16,777,215 * w that cancel: about one step of noise per vertex, new at every camera move, so it trembles at 30 fps
  too (user test) and any recomputed position on an image in between flips it (forced t = 1: 0 blinking pixels).
  Tried on the images in between only and dropped: depth rebuilt as z_b / w_b * w' (no change, 1.5 ulp is enough);
  z and w copied from the real frame (fixed, but bends the near ground decals while the camera turns); the copy
  limited to far points (the user then saw Pikachu's ear and mouth deformed on the loading screen, not reproduced).
  Fix: reversed depth on Direct3D. `project` outputs w * (range - z_offset) - z * z_scale, which has no
  cancellation (GX clip z is 0 at the far plane), so far depth keeps its float precision; the depth functions are
  mirrored in `depth_for`, the buffer clears to 0, the clear value, the fog and the depth copies read 1 - depth.
  The OpenGL renderer keeps the old depth. Castle blinking on the images in between over 87 images: 746 pixels lit
  3 times or more before, 17 after; moving run: no image over 500 bad pixels; unit tests pass.
- Shadow under the Pokémon trembling above 60 fps (fine at 30, user test). The shadows are quads drawn from above
  into the ground's decal texture, several with the same mesh under a projection with no known motion, so they
  stayed put on the images in between: the shadow sat at the real frame's place while Pikachu was halfway
  (`shadow_track.py` on a `run.txt` dump: shadow minus feet 11 px on the images in between against 3 px on the real
  frames, a sawtooth inside every frame). `follow_decals` now handles each such group (same projection, 2 to 24
  quads, w = 1): the shift most quads agree on is the decal camera's own motion (the still shadows all slide by it),
  a quad that lands on some earlier quad plus that shift stands still and stays put, also when the order changed
  (the ghost footprints), any other quad goes from its earlier place plus the shift to its new place
  (`g_decal_shift`). After: 5 px against 6 px, no sawtooth.
- `fit_motion` gave up when a late refinement round (limit down to 1e-4) kept under 60 % of the points, about one
  moving frame in seven in the candy zone: that frame had no main camera, every draw was lerped on the screen and
  787 repeated meshes stayed put. A late round that runs short now keeps the previous round's fit.
- Loading screens with the running Pikachu showed him torn apart on the in-between image (user's captures of
  2026-11-12, `139_between` on). Most samples there are the flat background at one depth, so clip z and w are both
  constant and their columns cannot be told apart. The fit's small pull toward zero split the motion between them
  (w row `10393 z + 0.65 w`), right for the background and wrong at any other depth: Pikachu counted as moved and
  went through an inverse with elements near 1e7. `fit_motion` now fits the difference from no motion, so a
  direction the samples do not settle stays the identity.
- `WP_PROFILE_GPU=<file>` (Windows) samples the GPU thread every millisecond after a warm-up and writes, every five
  seconds, how often each call chain (three return addresses inside the exe) was seen. First zone at 120 fps, scale
  4: flat. `draw_primitive` 43 % by itself and 70 % with what it calls (17,000 draws of about 9 vertices a frame,
  about 550 ns a draw), pairing and replay of the interpolation about 20 %, vertex copies about 12 %, `texture_hash`
  5.5 %. In the user's slow seconds the GPU thread is near 80 % and the guest idle near 55 %: the GPU thread is the
  limit, and no single cheap fix shows.
- The candy zone sends about 33,000 draws a frame (18,000 on the beach): the GPU thread limit explains the lower
  rate there.
- Second profile, in the heavy spot of the user's save (21,900 draws a frame, 120 fps, scale 3): the draws already
  reach Direct3D grouped (about 200 batches a frame), but 20 % of the GPU thread's time was spent waiting in
  `ntdll` under `run_copy`. The game copies the frame buffer about 50 times a real frame, and each update of the
  copy's constant buffer blocked about 0.1 ms in the NVIDIA driver, with `UpdateSubresource` and with
  `Map(WRITE_DISCARD)` alike. Why is not known. The scene uses 16 distinct sets of constants, so `run_copy` now keeps
  one immutable buffer per set and only binds it: 38 to 40 real frames a second became 48 to 53, an in-between image
  5.8 ms became 2.8 ms (its commands 4.2 ms became 0.2 ms), and the GPU thread went from 70 % to 93 % busy with no
  wait left. `hash_bytes` runs four multiply chains side by side (`texture_hash` was 5.7 % of the thread). The
  profile is flat again: `draw_primitive` 44 % by itself, 74 % with what it calls.
- With the images in between on, the same spot gives the user 53 to 57 real frames a second (110 at the 120 setting,
  80 at the 90 setting) against a steady 60 without them: the GPU thread is 76 to 86 % busy at 60 and the pairing
  adds about 2 ms a frame (exact keys 0.8, loose keys 0.26, camera fits 0.6, still or moving 0.35).
- Tried and dropped: matching the draws on four threads, split by key hash (same pairs, per-thread tables and
  result lists, helpers kept spinning between jobs). The four parts together took 3.4 ms against 1.06 ms on one
  thread and the wall time did not move: the draw and position arrays were just written by the GPU thread and each
  helper has to pull them over from that core. Camera fits per camera in parallel gained nothing either, one camera
  holds nearly all samples.
- Line profile of `draw_primitive` (48 % of the thread by itself): the two largest lines are the position transform
  and the vertex clear at 7 % each of the exe time, everything else is under 2 % a line. The next real gain is
  running the vertex work of several draws at once.
- One run in four of that series showed 31 frames a second with the in-between commands at 8.8 ms and the GPU
  thread 56 % busy, while a headless browser and a recorder of other programs were using the PC: taken as outside
  load, not proven.
- That slow mode was not outside load. It came back in about four runs of ten (31 to 45 real frames a second,
  commands 3 to 9 ms, GPU thread about 50 % busy), a whole run being either fast or slow, and its profile had 43 % of
  the GPU thread waiting in `ntdll` under `set_batch_state`: the same driver wait as for the copies, on the
  `Map(WRITE_DISCARD)` of the per-batch constant buffer. `constant_buffer` (gx_render.cpp) now keeps immutable
  buffers keyed by their bytes for the batches and the copies alike (4096 at most, then all released). The six
  runs made afterwards were all fast. Cost: about 150 new sets of constants a frame, so about 9,000 `CreateBuffer` a second,
  about 1.3 ms a frame. Not tried: a few dynamic buffers used in turn. The OpenGL path was not touched.
- Draws on several threads (gx.cpp). Measured first: 21,000 to 22,000 draws a frame, 97 % of them with no BP, CP or
  XF write since the draw before, 82 % in runs of 32 draws or more, 59 % in runs of 128 or more, about 45 top-level
  parses a frame. `draw_primitive` therefore only queues the draw (`g_run`). `flush_run` runs at every state write
  (`load_bp`, `load_cp`, `write_xf`), at 512 draws and at the end of each top-level parse: it does the per-draw
  setup once, builds the vertices and triangles of each draw (`build_draw`, which only reads the state) into its own
  `DrawOutput`, then hands them to `render::draw` in the original order. Runs of 16 draws or more are built by the
  GPU thread and up to three helpers, which take four draws at a time by a compare-and-swap on one packed word
  (generation, count, next index). Helpers spin 4000 times between runs before sleeping on a condition variable; a
  sleeping helper costs a run about 50 µs, more than the run itself. Machines of four cores or fewer, draw logging
  and captures stay on one thread.
- Result in the heavy spot (120 fps setting, scale 3, headless): 48 to 53 real frames a second with the GPU thread
  93 % busy became 60 with an image in between every frame and the thread 70 to 86 % busy; about 50 % busy at the
  60 setting. `WP_SERIAL_DRAWS=1` starts no helper. `WP_CHECK_DRAWS=1` builds every helper-built draw again on the
  GPU thread and compares the bytes: 35.6 million draws, none differed.
- In play the user still saw 100 to 110 at the 120 setting and 80 at 90, with the game itself at 60. The headless
  runs had hidden it: without a window nothing is presented. `WP_HEADLESS=window` (hidden window, images presented)
  reproduces it: timers held 50 times a second, 5 to 14 images in between dropped a second. Two causes.
  The GPU thread slept a full millisecond whenever its queue was empty and an image was waiting to be shown, so
  every burst of commands started up to 1 ms late; it now waits on the queue for 1 ms at most and wakes when work
  comes (holds 50 a second became 6). And a frame counted as late once the timers had waited 1 ms for a token, which
  dropped the next image in between; a trace showed those holds follow a game frame that ended 1 to 2 ms later than
  usual, the thread catching up by itself within 2 to 4 ms, and the dropped image upset the pacing enough to cause
  the next hold. The limit is now 4 ms. Same spot, hidden window: 119.9 images a second with 60 of 60 in between at
  the 120 setting (scale 3, and scale 6 with the bug recorder), 88 to 90 at the 90 setting, 60 at 60.
- Known limits: the helpers keep up to three cores busy during heavy frames, and a helper the system takes off its
  core in the middle of four draws holds the GPU thread until it comes back. Not measured in the candy zone or in
  real play.
- Tried and dropped: a camera per drawing pass (projection plus copies so far): the main scene is one pass, and the
  decal pass cameras lost their fit.
- A run crashed once in the game's render-target code (0x80070fc0) after 175 s, with the images taking 8 ms on a
  loaded PC: the heap exhaustion seen before when the game's loop slows down, not tied to these changes.

### Image dumps (headless)

- `WP_DUMP_SEQUENCE=<first frame>,<frames>` saves each frame's interpolated images then the real frame to
  `captures/seq/<frame>_<n>.png` / `_f.png`, and makes images for every frame, late or not. Use
  `WP_SCRIPT_CLOCK=frames` with `run.txt`: frame 2400 is on the beach with Pikachu.
- The other debugging switches used to find the bugs above (`WP_REPLAY_SEGMENTS`, `WP_REPLAY_STILL`,
  `WP_REPLAY_MODE`, `WP_REPLAY_ONLY_CAMERA`, `WP_REPLAY_RESTORE`, `WP_LOG_DRAWS_AT`, `WP_LOG_BIG_DRAWS`, `WP_LOG_FIT`)
  were removed once they were fixed.
- Blink rate: a pixel of an image blinks when it leaves the range of its two real neighbours by more than 12 in luma;
  sum over 40 frames of the beach run gives a map where artifacts show as shapes lit again and again. Runs differ
  from one another (game timing): compare rates over a few runs, not one.

### Open bugs at 120 fps (all fine at 60)

1. Photo screen noise once (`Texturebug.png` at the repo root, local only).
2. Shadows and footprints still flicker in game (user report), far textures tremble and some turn black for an
   instant (user report, only above 60 fps). Not reproduced in the dumps of `run.txt`; the bug recorder below is
   there to get the images from the user.
3. In game the rate does not hold 120 (GPU thread 85â€“95% busy at 6x).

Solved on 2026-11-11, see "Temporary heap and late draw tokens": the pause menu noise, the black bars after leaving
the pause menu and the crash in the texture-object init `f_80070fc0`.

### Test method

- Scripts in `build/test-scripts/` (ignored): `run.sh <tag> <runs> <timeout> <script> [ENV=..]` copies the current
  NAND, `settings.ini` and `stats.ini`, runs headless and muted with save backups off, retries while Windows Smart App
  Control blocks a fresh exe (rc=126, "An Application Control policy has blocked this file"; rebuilding with
  different bytes gets past it), kills leftovers, puts the copies back and checks them. `hl.sh` is `run.sh` with
  `pause.txt` (opens the pause at ~105 s), `cam.sh` with the frame clock. `run.txt` moves 32â€“54 s, `empty.txt` stays
  on the title. With the frame clock a script time T acts about 250 frames before frame T * 60 / 1000 of
  `WP_DUMP_SEQUENCE` (A at 40000 shows at frame 2144). Moving in the field needs the sideways flag held
  (`30000,200000,800000`). In that run the save has no partner: A only makes Pikachu wave.
- Never restore a save copy taken earlier: on 2026-11-11 a stale copy overwrote a save of the day (recovered from the
  game's own backup).
- `WP_HEADLESS=window` keeps the window, hidden: the real present path runs (interpolated images, bug recorder,
  realistic GPU thread load). `build/test-scripts/press.py <virtual key>` posts a key to it. Headless runs no longer
  count play time.
- A program argument after the data folder is a timeout in seconds: the run ends with the guest thread report.
- Rules from the user: headless only (no window), ask before a test series, never build or run while
  `pokepark2.exe` runs, runs muted, never commit official images or the untracked files at the root.

## Temporary heap and late draw tokens

- The game draws with temporary buffers taken from six heaps of one pool (`0x804b6538`, 0x3C bytes per heap, free list
  at +0x0C, next block at +8, allocation `0x8000DF50` â†’ `0x8037D4A0`). Heap 4 holds 4 MB: a frame in the first zone
  takes 1.6 MB of it, a pause menu frame 3.5 MB.
- Buffers are not freed at once: `0x8000E0B0` puts the pointer in a list of 40 entries (pool +0x1F0 entries, +0x1F4
  links, +0x1F8 free slot, +0x1FC pending head), `0x8000E1C0` tags the untagged entries with the id of the draw token
  the frame sends, and the token callback (`0x80031BD0` â†’ `0x8000E270`) frees the entries of that id. A zone frame
  queues 19 frees, a pause menu frame 34. When the list is full the original code drops the free: the memory is lost
  for good.
- With the GPU thread a frame late (stalled 250 ms on purpose in the pause menu): 68 frees for 40 entries, about
  1.4 MB of heap 4 lost per incident, then allocations return 0 (184 in one run). That was the pause menu noise, the
  black bars and, once the heap was gone, the crash in the zone.
- Fix (`game/game.cpp`, hooks `DeferredFree`, `ReleaseDeferredFrees`, `TempPoolAlloc`): a free that finds
  the list full is kept aside and queued again after the next release; an allocation that does not fit while frees are
  pending waits for the GPU thread to reach the last token sent (`gx::wait_for_token`, 500 ms at most) and polls the
  interrupts so the token callback frees the buffers, then runs. Same stall with the fix: 0 failed allocations, heap 4
  back to its 4 MB after each release, four waits of about 190 ms logged as `memory`.
- Crash in `largest_free` (seen on a friend's Ryzen + RX 9070 XT at 283 s, then once here in an OpenGL run under
  load, both from the call at 0x80070fc0 into heap 4): the walk read a free block link outside guest memory
  (`mmio::read32` of an address near 0xFFFFFFFF). The game's allocator (0x8037D4A0) stops at the first block that
  fits, so it never reaches that part of the list there; our walk goes to the end. It now stops at any link that is
  not an aligned address inside guest memory and logs the first such link once (`memory ... links to`). Our hooks had
  not acted before either crash (no wait, no full free list). Whether that link is a real corruption is unknown: if
  the game later crashes inside 0x8037D4A0, the log line gives the heap, the block and the value.
- It is a real corruption: in an OpenGL run with the frame wait still at the copy (late tokens, about 12
  write-backs a second) the log said `free block 1 after 814f9950 links to ffffffff`, heap 4 shrank to 0 KB and the
  game crashed inside 0x8037D4A0 itself 18 ms later. Not found: a build that walked heap 4 before and after every
  write-back and compared each written range with the free blocks never saw it break in seven runs (63 to 1003
  write-backs each). Every sighting came with a renderer late on its tokens (OpenGL waiting at the copy, the friend's
  Direct3D at 120 fps), so that is the condition to recreate when looking further.
- Looked at and ruled out for the gray lines between polygons of moving models: the game changing vertex arrays or
  indexed matrices while the GPU thread still draws from them (0 changes before the frame end over 600,000 samples a
  second, even with the thread stalled 150 ms), and face culling in single precision (wrong for 10,000 to 17,000
  triangles a second, all under 0.03 pixel of area; now computed in double precision by `gx::facing_area`).

## Bug recorder and hang report

- `video.bug_recorder` (Advanced tab, off by default, Direct3D 11): every image shown, real or interpolated, is copied
  to a ring of textures, halved until it is at most 1400 pixels wide, 360 images (three seconds at 120 fps) or 1 GB at most. The screenshot key
  then also saves the ring to `screenshots/<name>_recent/NNN.png` or `NNN_between.png` (interpolated), oldest first.
- A watchdog in `main.cpp` writes a `hang` line to the log when the game has drawn no frame for 15 seconds outside the
  options menu, followed by every guest thread (state, priority, saved pc and lr, the queue and mutex it waits on, its
  call chain read from the stack back chain) and the IPC state and trace.
- Without `WP_LOG_FILE` and outside headless runs the game logs to `logs/pokepark2.log` under the working directory
  (the install folder), after renaming the last one to `pokepark2.previous.log` (`use_default_log` in `main.cpp`). Its
  first lines give the processor, the logical processor count, mains or battery, and the Direct3D adapter with its
  driver version. The `threads` line ends with "without work": the share of the second the graphics thread slept on
  an empty queue. 100 minus busy minus without work is time it was blocked, in the driver or elsewhere.
- Direct3D 11 takes the first adapter of `EnumAdapterByGpuPreference(HIGH_PERFORMANCE)` and falls back to the default
  adapter; the exe also exports `NvOptimusEnablement` and `AmdPowerXpressRequestHighPerformance` for the OpenGL path.
  Before, a laptop with two graphics cards ran the game on the integrated one (friend's Ryzen 5 5600H + RX 6500M,
  2026-11-12).
- Headless runs do not measure the input path: the input script returns before the keyboard, mouse and controller
  are read. In real play `update_wiimote` (`bluetooth.cpp`) called `input::wakes_remote` on every poll, a full sample
  with about 21 `GetAsyncKeyState` calls and one `GetCursorPos`, each a kernel call of about 0.5 us. Emulated in a
  headless run it cost 500 ms of kernel time a second on the game thread in menus and took the guest idle share from
  47 to 28 % in the main zone. The wake check now runs only while the remote is disconnected, and
  `send_completed_packets` no longer builds its event when nothing completed (5 % of the game thread).
- The `batches` log line splits the Direct3D time of each second: state, constants, vertex copy with its megabytes,
  draw calls. On the RX 6500M laptop the copy ran at 2.4-2.7 GB/s whatever the batch count, the buffer size or the
  map mode, against 15-20 on an RTX 4070; the likely cause is its four-lane link on a PCIe 3.0 machine. When a second
  with at least 128 MB of vertices runs under 6 GB/s (`WP_TRIM_BELOW` changes the limit, 1000 forces it),
  `draw_trimmed` (`gx_render.cpp`) takes over for frames that are not recorded: it finds the highest texture
  coordinate with a non-zero bit in the batch, packs the vertices up to it in ordinary memory, copies that in one
  piece and draws with a stride of 48 + 12 per coordinate; the coordinates left out come from a 16-byte zero buffer
  bound with stride 0 (`trimmed_layouts`), so the vertex shader gets the same bits as before. Never write vertex by
  vertex into the mapped buffer: that ran at 0.7 GB/s on NVIDIA. Recorded frames (in-between images) keep the full
  vertices. Cards other than NVIDIA also rewrite one dynamic constant buffer per size (`g_nvidia`,
  `WP_MAP_CONSTANTS=1` forces it), since creating immutable ones cost 40 us each there.
- Option `video.gpu_vertices` (`WP_GPU_VERTICES=0` or the settings file to switch it off, on by default, no menu entry) moves the transform unit to the
  graphics card for frames that are not recorded. `flush_run` (`gx.cpp`) then calls `pack_run` instead of building
  screen vertices: each draw is decoded into compact vertices (position, two colours, then the normal, 12 bytes of
  matrix numbers and the texture inputs only when the run needs them, 20 to 108 bytes) with 16-bit indices, and the
  run's XF state goes into rows of four floats (`begin_packed_run`): projection and viewport, position matrix, seven
  rows per texture coordinate and, when lighting is on, 40 rows with the channel controls, material and ambient
  colours, normal matrix and the eight lights. Runs with matrix numbers in the vertices share a copy of XF words
  0-0x107 and 0x400-0x467 that is sent again only after one of them is written (`g_palette_serial`). The renderer
  (`begin_run`, `draw_runs` in `gx_render.cpp`) appends vertices, indices and rows to three buffers and issues one
  `DrawIndexedInstanced` per run; the start instance reads the run's first row number from a fixed buffer of
  0, 1, 2..., and `vertex_run` loads the rows from a `Buffer<float4>`. Appending to a buffer that shaders read needs
  `MapNoOverwriteOnDynamicBufferSRV`; without it, or when the shader fails to compile, the log says "vertices stay
  on the processor". The state must be per run and never per draw: the main zone has about 21,000 draws of 7
  vertices in 600 runs a frame. Culling uses two rasterizer states (GX cull 1 is `CULL_BACK`, 2 is `CULL_FRONT`).
  Lines, points, draw logging, captures and recorded frames (in-between images, unless `video.gpu_between`) keep the processor path, and every
  pass of a multi-pass draw must take the same path or depth-equal tests fail. Shader lighting follows
  `gx_lighting.cpp` step for step, including the integer `modulate`; `WP_CPU_LIGHTS=1` leaves lighting on the
  processor for comparison. Main zone at 60 fps on an RTX 4070: GPU thread busy 57 to 29 %, vertices copied 1370 to
  390 MB a second, same frame rate. Two headless runs of the same mode differ on about 2 % of the pixels (characters
  do not move the same way), the two modes on 0.3 %, so image dumps cannot prove more than "no visible difference".
- Option `video.keep_models` (`WP_KEEP_MODELS`, on by default, no menu entry, needs `video.gpu_vertices`) keeps the
  packed vertices and indices of runs that come entirely from display lists in two fixed buffers of video memory
  (64 MB and 16 MB, `keep_run` / `draw_kept` in `gx_render.cpp`), after the display-list cache of the PokéPark 1
  recompilation. On a Ryzen 5 5600H laptop with a Radeon RX 6500M the four options together hold 60 fps in the heavy zone, where the processor path gave 25 to 42. `pack_run` looks the
  run up in `g_kept` by its first and last draw address, draw, vertex and index counts, vertex format, VCD and VAT
  (`find_kept`). `kept_content` hashes everything else the packed bytes depend on: the list bytes with their draw
  headers, the range of each indexed array the list reads (`scan_ranges` walks the indices once per list content),
  the array addresses and strides, the material colours when a vertex takes them, and the default matrix numbers.
  The second time a run shows the same hash it is stored after decoding (and hashed again, in case the game wrote
  meanwhile); from then on a matching hash draws it from the kept buffers without decoding or copying. A run whose
  hash changes four times in a row, whose arrays lie outside memory or whose array ranges exceed 64 bytes per
  vertex (hashing would cost more than decoding) is left alone for 64 frames. A changed run is stored again in its
  old place, after flushing pending draws that still read it. When the buffers are full or 32,768 lists are known,
  everything is forgotten (`forget_kept`, `reset_kept` bumps a generation). Runs under 32 vertices, immediate draws
  and runs lit on the processor are never kept. The log line `models` gives the runs drawn from video memory,
  decoded and stored each second and the KB hashed. `WP_CHECK_KEPT=1` decodes every kept run anyway and counts those
  whose bytes differ from the stored copy ("verified", "differ"): it must report 0.
- Option `video.gpu_between` (`WP_GPU_BETWEEN`, on by default, no menu entry, needs `video.gpu_vertices`) lets
  display-list runs go through the vertex shader while a frame is recorded for the in-between images (90 and
  120 fps), after `gx_frames.c` of the PokéPark 1 recompilation. Only runs whose draws all come from display lists take this path (`run_in_lists`
  in `flush_run`); immediate draws, lines and points keep the processor path and its pairing. `draw_runs` records a
  `kRuns` command with the rows of every run (`record_runs`); the vertices stay where they are, in the kept buffers
  or in the 16 MB ring, which `end_record` forces to restart at each recorded frame. A ring that wraps after a run
  was recorded, a kept run rewritten in place and `reset_kept` break the record for that frame (no images in
  between), so the option is best used with `video.keep_models`. `pair_runs` pairs each run with one of the previous
  frame that has the same key (first draw address, draw, vertex and index counts, format, row count, cull, first
  texture): runs with a single candidate vote for the camera motion (`find_run_camera`, 48 samples of current times
  inverse previous position matrix, tolerance 0.02 on rotation and 2 + 2 % on translation, at least 3 votes and a
  third), the others take the nearest of 48 candidates once the previous position is moved by that motion.
  `replay_runs` redraws each run with rows blended between the two frames (`blend_run`, `blend_palette`): projection,
  position, texture and post matrices, normal matrix, light positions and directions, and for skinned runs the
  first 21 matrices and the normal rows of the palette. A matrix whose rotation part changes by more than 0.3 of its
  scale or whose translation moves more than half its length plus 200 is taken from the current frame
  (`matrix_change`), and so is one where a single row changes by more than half of that row's own scale: the game
  draws the shadows of the Pokémon as discs seen from above the player, turns that view by a quarter turn in one
  frame when the player changes direction, and the ground's matrix into that picture has rows of very different
  sizes, so the whole-matrix test let it through and the images in between showed shadows half as large again,
  turned 45 degrees around the player. Post matrices go through the same test (they were blended unconditionally);
  the `runs` log line counts the texture matrices held back this way. A copy of a model that just appeared does not
  take the previous place of another copy that a later copy is nearer to (`later`, `taken` in `pair_runs`). When more than 8 and a third of the paired runs do that, the frame is a cut and gets no
  images. Unpaired runs and runs whose row layout changed are drawn as in the current frame. The camera motion
  found from the matrices is also given to the processor path (`adopt_run_camera`) when the few draws left there
  could not fit it. The log line `runs` gives the runs redrawn, paired and moved too far per frame.
- Option `system.idle_sleep` (`WP_IDLE_SLEEP`, on by default, no menu entry) lets the game thread sleep in
  `poll_interrupts` while the guest runs its idle thread, on the model of PokéPark 1's `hle_wait_for_event`: it
  sleeps until the earliest of the next retrace, the decrementer (`decrementer_wait`), the end of the sound buffer
  being played (`dsp::idle_wait`, which also refuses to sleep while the DSP has work) and the next IPC event
  (`ipc::idle_wait`), 1 ms at most, and is woken at once by the token and finish interrupts of the GPU thread
  (`gx::wait_for_interrupt`, a high-resolution waitable timer on Windows because an ordinary timed wait ends on
  the system tick). The wait ends 250 µs early and the idle loop spins for the rest; waking early only before the
  retrace and the decrementer was measured worse (late sound buffers 4 -> 40, late in-between images 1 -> 28 in
  48 s) for the same processor use. Measured headless in the heavy zone at 120 fps: game thread busy 98 -> 56-59 %,
  same frame rate, no sound block missing, stale or overwritten, mean start of a sound buffer 0.45 -> 0.85 ms
  late. A first version that slept a flat 1 ms without looking at these deadlines crackled. The GPU thread already sleeps on its queue and the draw helpers
  after 4000 pauses, so nothing else spins.
- The NVIDIA constant-buffer cache dropped all 4096 buffers whenever it was full, about twice a second in the main
  zone, so 8000 to 9000 immutable buffers were created each second (53 ms). It now drops only those not used since
  the previous clean-up, and everything only when three quarters were in use.
- The log file is opened in append mode like the redirected stderr. Before, the log wrote at its own position and
  overwrote whatever stderr had added, so the thread report after a `hang` line was lost as soon as another line came.
- The "endless loading after a cutscene" (user's logs of 2026-11-11 14:24 and 2026-11-12 01:02, both stopping after
  `ios open /tmp/Pokemon/Data.dan` while sound goes on) was the IOS descriptor number. The SDK's IPC profiler
  (`IPCiProfInit` 0x802cde40, tables at 0x805264a0) copies the name of every opened device to `0x80527520 + fd * 0x30`
  when the open reply comes back and clears those 0x30 bytes on close. The table holds 128 descriptors, the 96 entries
  of the next table follow, then the PAD variables at 0x80529f20 and, for descriptor 228, the WPAD alarm at 0x80529fe0.
  The engine handed out a new number for every open, so the 228th successful open of a session wrote a path over that
  alarm: its `next` pointer no longer led to the alarms behind it, among them the frame timer of the main loop
  (0x804ba548), and the main thread waited for ever on its message queue. `ios::open` now returns the lowest free
  number, as IOS does. Found from a copy of the hung game's memory taken with gdb (alarm queue head at r13-0x2e08).

## OpenGL renderer (macOS and Linux)

- Configuring with `-DWP_OPENGL=ON` on Windows builds the OpenGL renderer and the SDL window loop instead of
  Direct3D 11, only to test the renderer of the other systems on a Windows PC. Releases for Windows stay Direct3D 11:
  it has every feature and was measured faster.
- The OpenGL renderer has the vertex shader (`kRunSource`, lighting included) and the models kept in video memory.
  It reads the same per-run rows as Direct3D but writes ordinary depth: `(z * scale + w * offset) / 16777215`, where
  the header holds `16777215 - offset` for the reversed depth of Direct3D. `gx::project` likewise picks the reversed
  formula only for `_WIN32 && !WP_OPENGL`; with plain `_WIN32` the Windows OpenGL test build lost every 3D model and
  only the sky and the map were left.
- A hidden or occluded window does not make `SDL_GL_SwapWindow` wait, so nothing limited how many frames the GPU
  thread queued: headless, the driver's private memory grew by about 250 MB a second up to 27 GB. A fence is placed
  at every copy to the frame buffer, and the fence of the frame before is waited for at the first draw of the next
  frame (`wait_for_frame` in `flush_pending`), so at most two frames are queued, as Direct3D's frame latency of two.
  Memory then stays near 1.1 GB and the drawing time fell from 708 to 120 ms a second. Waiting at the copy itself
  held back the draw token that follows it, so the game got its temporary buffers back late, copied into buffers
  that were not the ones of the frame before and had them written back to RAM: three interleaved pairs of runs gave
  12-53 write-backs and 42-221 slow frames for the wait at the copy, none and 5-6 for the wait at the next draw.
- `WP_DUMP_SEQUENCE=first,count` writes the final image of those frames to `captures/seq` (no in-between images).
- Pixel shader variants, as in Direct3D: `variant_key` (now in gx_render_common.cpp, shared) and `variant_fragment`
  turn the TEV stages, swaps and indirect setup into GLSL constants. Without them the heavy zone ran at 54-57 fps at
  scale 3 and 59-60 at scale 1 (the generic shader was the limit on the graphics card); with them 59-60 at scale 3.
  With `GL_KHR_parallel_shader_compile` (or the ARB one) at most four compile in the background at once: starting
  sixty at a zone load froze the NVIDIA driver for 4.5 s. Without it (macOS) one variant compiles per image, 1-3 ms
  each on NVIDIA, about 30 ms for the first ones. The generic shader draws until a variant is ready, and keeps
  drawing those that fail (logged). Dead branches must not index out of bounds once the stages are constants
  (NVIDIA rejects `x[bump - 1u]` with `bump` = 0 even under `bump != 0u`), hence the `max(v, 1u) - 1u` indices.
  `WP_GENERIC_SHADER=1` turns variants off. Log line `video ... pixel shader variants ready` while some are built.
- Measured headless on an RTX 4070, frames 1500-1505 of `still.txt`: 2 % of pixels differ from Direct3D, as between
  two Direct3D runs.

## Code notes

What the code added for frame interpolation, the timers and the input changes does, kept here since the sources hold no comments (CONTRIBUTING.md). Each entry names the declaration or line it describes.

### `engine/include/wp/gx.h`

- `size_t gpu_backlog();`: Bytes of commands sent by the game and not yet run by the GPU thread.
- `bool frame_owed();`: True while the GPU thread has not reached the last token the game sent: timer interrupts wait for it.
- `bool take_frame_held();`: True once after frame_owed made the timers wait: the GPU thread is late.

### `engine/include/wp/gx_render.h`

- `struct DrawSource`: What frame interpolation needs to place a draw between two frames: the draw's identity across frames, the clip position of each vertex the game sent, and for each emitted vertex the one it came from.
- `uint64_t exact = 0;`: The key plus where the positions come from, so copies of one mesh are told apart; 0 when unknown.
- `uint32_t camera = 0;`: Identifies the projection and viewport: draws sharing them move together when only the camera moves.
- `bool interpolating();`: True while frames are recorded to be interpolated: draws then pass their DrawSource.
- `void finish_frame();`: Draws the interpolated images of the last frame copy; called once the game has its draw-done or token signal.
- `bool service_presents();`: Shows the queued images whose time has come; called often by the GPU thread. True while images still wait.

### `engine/include/wp/gx_state.h`

- `bool fit_motion(const float (*from)[4], const float (*to)[4], size_t count, float matri...`: Fits the matrix that takes each clip-space point `from` to its `to`, leaving out the pairs that do not follow the rest (things that move by themselves). False when too few pairs agree on one motion.

### `engine/include/wp/frame_pacing.h`
- `FramePlan plan_frame(double& next, double period, int rate, uint32_t max_images);`: The interpolated images of the frame just made: their places between the previous frame (0) and this one (1), whether this frame is shown, and the time between images. next carries the place of the next image across frames; 0 starts again on a shown frame.
- `AlarmPeriods frame_alarm(uint64_t input, uint64_t period, uint64_t pending, int fps, bool movie);`: The frame alarm's period and pending period for a frame-rate limit (docs/NOTES.md, "45 and 90 fps").
### `engine/include/wp/perf.h`
- `struct Sample`: The figures the performance overlay shows; -1, 0 or empty when unknown, drawn as a dash.
- `SystemFigures system_figures();`: The last CPU, GPU and RAM figures. The first call starts the sampler thread, which reads them once a second while `overlay.show` is on.
- `double gpu_engine_percent();`: The busiest adapter's 3D engine use, also keeping this process's share for gpu_process_percent. The first call only starts the counters and returns -1.
### `engine/include/wp/perf_overlay.h`
- `void update();`: Called by the window's 16 ms timer: draws the overlay image again when a new sample, level, corner or window height comes, and hides it when `overlay.show` is off.
- `options::Rect corner_rect(int client_width, int client_height, int width, int height, ...`: The overlay's place in the window, shrunk when the window is too small for it.
### `engine/include/wp/input.h`

- `uint32_t keyboard_buttons(const keymap::Bindings& keys, const std::function<bool(int code)>& down, bool aiming);`: The Wii Remote buttons the held keys give; with aiming, a mouse button bound to 1 gives A.
- `void upgrade_bindings();`: Called once at start: US letters to this layout, old default keys to the new ones (once, `input.bindings_version`), `pad.*=auto` to real buttons (see "Controls").

### `engine/include/wp/color.h`

- `inline constexpr Range kRanges[] = {kBrightness, kContrast, kSaturation, kGamma};`: The color settings and their slider ranges, in percent; the menu builds its sliders from them.
- `Adjust current();`: The four settings as factors (100 % is 1), clamped to their ranges.
- `uint32_t apply(uint32_t rgb, const Adjust& adjust);`: The present shaders' color step on one 0x00RRGGBB pixel, for screenshots; the pixel comes back unchanged when every factor is 1.

### `engine/include/wp/control_names.h`

- `std::string pad_button(int physical, padmap::Family family);`: The name printed on that button of that family of controller, in the menu language for translated ones.
- `std::string pad_binding(const padmap::Binding& binding, padmap::Family family);`: Combos joined by ", ", buttons of a combo by " + ", an em dash when empty.
- `std::string key(int code, const std::function<uint32_t(int code)>& character);`: A key as the player sees it; character gives the character the layout writes for a key, 0 if none (tests pass their own layout).
- `std::string key(int code);`: Same, with this computer's layout.
- `std::string keys(const std::vector<int>& codes);`: Keys joined by ", ", an em dash when empty.

### `engine/include/wp/gamepad.h`

- `int held_physical();`: The first physical input (padmap::Physical) held on any gamepad, or -1: what the menu binds to an action.
- `padmap::Family shown_family();`: The family whose button names the menu shows, published by the gamepad worker; Xbox when the worker does not run.

### `engine/include/wp/padmap.h`

- `using Combo = uint32_t;`: One bit per `Physical`; a combo is pressed when all its buttons are held.
- `Binding default_binding(size_t action, bool sideways);`: The default buttons of an action for the game's grip.
- `Binding parse_binding(const std::string& text, size_t action, bool sideways);`: `none` is empty; `auto`, empty text or only unknown names give the default.
- `std::string format_binding(const Binding& binding);`: The text form stored in settings.ini; `none` when empty.
- `std::string without(const std::string& text, int physical, size_t action, bool sideways);`: The binding without the single-button combo of physical, as text.
- `uint32_t buttons(const Pressed& pressed, const Mapping& mapping);`: A held combo that is a strict subset of another held combo is left out.

### `engine/include/wp/keymap.h`

- `std::string localize(const std::string& list, const std::function<int(int letter)>& let...`: The letters of a default binding are positions on a US keyboard, given back as the letters at those positions on another layout: W, A, S, D become Z, Q, S, D on AZERTY. letter maps a US letter to the local one.
- `int local_letter(int us);`: The letter at the US position of us on this computer's layout.
- `std::string local_defaults(size_t index);`: An action's default binding on this computer's keyboard layout.
- `const char* previous_defaults(size_t index);`: The US defaults used before 2026-11-11, or nullptr for an action whose default did not change.
- `std::vector<size_t> outdated(const std::function<std::string(const std::string& key)>& value, const std::function<int(int letter)>& letter);`: The actions still on their old default whose new keys no action keeping its keys holds; no key ends up on two actions that did not share it before.
- `bool mouse(int code);`: Mouse buttons and the wheel.
- `std::vector<size_t> unlocalized(const std::function<std::string(const std::string& key)...`: The actions whose letters differ on this layout, when every one of them still holds the US defaults (a file written before the layout was followed); empty once any of them was changed.

### `engine/src/app/main.cpp`

- `timeBeginPeriod(1);`: Waits of a millisecond, not the default 15.6: frames shown 8 ms apart at 120 fps depend on it.

### `engine/src/gpu/gx.cpp`

- `struct Vertex`: A draw's vertices share one Vertex: only the attributes the layout sends are written, the rest keep the draw's defaults.
- `template <int Bits>`: Each n-bit channel value as the byte the float path rounded it to.
- `void begin_vertex(Vertex& vertex)`: The attributes a draw's layout does not send.
- `for (uint32_t k = 0; k < 2; k++)`: Without lighting each channel is its material, from the vertex or from the register.
- `bool g_recording = false;`: While frames are interpolated, each emitted vertex notes the vertex of the draw it came from.
- `bool g_in_list = false;`: Inside a display list, whose bytes stay at one place in memory from frame to frame.
- `uint64_t draw_key(uint8_t command, uint32_t count, const Layout& layout, const uint8_t*...`: Identifies a draw from one frame to the next: the same mesh sends the same command, layout, texture and, when its positions come from an array, the same indices.
- `bool indexed = layout.position.mode == kIndex8 || layout.position.mode == kIndex16;`: Indexed positions: the array they come from; inline ones: where the display list holds them. Inline positions sent through the FIFO have no lasting place and only the plain key.
- `std::atomic<uint32_t> g_frame_holds{0};`: Times the game's timers waited for a token (frame_owed) since the last report.
- `g_frames += static_cast<int>(render::take_interpolated_frames());`: Interpolated frames count as shown frames, less the game frames left unshown at 90 fps.
- `std::atomic<uint32_t> g_token_sent{0};`: The last token the game sent with an interrupt and the last one the GPU thread reached (frame_owed).
- `static bool bp_next = false;`: GXSetDrawSync writes the BP opcode as a byte, then the register as a word. A vertex byte taken for one only sets a wrong token, which the next real one replaces.
- `bool frame_owed()`: The game frees the buffers of a frame when its token interrupt comes, and takes the next frame's buffers when its frame alarm fires. With the GPU thread a little late the alarm came first: the game took fresh buffers every frame (EFB copies to ever new addresses, each written back to RAM), then ran out of heap in the pause menu. The alarms wait for the token the GPU thread still owes, for 100 ms at most.
- `if (unreached && now - since > std::chrono::milliseconds(1) && !g_frame_late.exchange(t...`: Most holds last one poll, between the token and its interrupt. Only a real wait means the thread is late.
- `bool take_frame_held()`: Asked right after a token: true when the timers waited more than a millisecond for it.
- `while (g_gpu.items.empty())`: While images wait for their time the thread polls every millisecond: a timed condition wait rounds up to the 15.6 ms system tick with MinGW, a sleep does not.

### `engine/src/gpu/gx_render.cpp`

- `constexpr uint32_t kVertexCapacity = 1 << 18;`: Large enough for a whole frame of the open zones, so an interpolated frame can draw the vertices already uploaded.
- `PixelInput vertex_moved(VertexInput input, float4 moved : MOVED)`: An interpolated frame draws the frame's vertices again with the halfway positions beside them; w = -1e30 keeps the vertex and w = -2e30 moves it with camera number x.
- `constexpr size_t kMaxBetween = 3;`: Interpolated images made per real frame at most: 3 lets 30 fps content reach 120.
- `constexpr size_t kMaxCameras = 8;`: Projections per frame whose camera motion is followed (the scene, the menus, a reflection): the shader's array size.
- `struct PendingBatch : BatchState`: The batch being gathered: its vertices are g_triangles from begin on.
- `struct RecordedDraw`: Frame interpolation (docs/NOTES.md, "Frame interpolation"): each frame is recorded as the commands it ran, then run again with every draw matched in the previous frame halfway between its two positions, and shown in between.
- `struct ReplayTarget`: An EFB copy target as the replays see it: they draw into a twin started from what the target held before the frame first copied into it, so the game's own texture keeps the real frame's result. Effects that read last frame's copy, like the pause menu's blurred background, would otherwise run once more per interpolated image.
- `struct RecordedTarget`: A target copied into during the recorded frame, with its texture and view (owned references).
- `bool twin = false;`: The frame sampled the target before copying into it: replays use a twin.
- `[[gnu::used]] const char kBuildSalt[] = "SALT6335";`: Windows Smart App Control sometimes blocks a fresh build; changing these bytes and rebuilding gets past it.
- `size_t g_frame_vertices = 0;`: Vertices of the recorded frame already drawn, which is also where the next batch goes in the vertex buffer.
- `uint32_t g_interpolated_ready = 0;`: Interpolated images of the last real frame waiting to be queued.
- `[[maybe_unused]] static const bool started = []`: Several compilers: entering a zone asks for dozens of variants, drawn by the slow generic shader until ready.
- `uint32_t draw_batch(const BatchState& pending, const ScreenVertex* vertices, size_t total)`: Returns where the vertices start in the vertex buffer, or ~0u when they did not fit in one piece.
- `if (start != g_frame_vertices)`: The interpolated frame draws from the vertex buffer: each batch must sit where the frame's count says.
- `if (g_recording && source && source->key != 0)`: A draw culled whole still counts in the order draws are matched in.
- `void note_copy_target(ID3D11Texture2D* texture, ID3D11ShaderResourceView* view, ID3D11R...`: The recorded frame's first copy into a target. A target the frame sampled before copying into it builds on its own last content (footprints, the pause menu's blur): replays draw into a twin started from what the target held. Any other target is filled again before it is read, so replays copy straight into it. Twins for every target took gigabytes at 6x, and the allocations that then failed broke the shadows and the textures.
- `void prune_twins()`: Twins of targets no longer copied into are let go.
- `constexpr float kMaxStep = 0.5f;`: A vertex that moves more than this between two frames, in screen halves, belongs to another object or a camera cut.
- `uint32_t step = std::max(1u, draw.positions / 8);`: A few vertices are enough to tell another object or a camera cut.
- `struct EfbSnapshot`: The EFB's color and depth at one moment. The game may build on what the last frame left in the EFB (the pause menu does), so each replay starts from the EFB as the real frame found it, and the real frame's EFB is put back after.
- `constexpr uint32_t kByCamera = 0xFFFFFF00u;`: As a match's partner: no draw of the previous frame, the draw follows camera number (partner - kByCamera).
- `constexpr double kStillScreen = 2e-3;`: The projections of the frame whose camera motion was found: `back` takes a clip position of this frame to where the same point of the world was in the previous frame. How far, in units of the screen's half, a point may sit from where its camera's motion puts it and still count as standing still. The fit's own error reaches about 1e-3 during a turn; a mover this slow moves less than a pixel a frame.
- `size_t second = kMaxCameras;`: A second motion found under the same projection (kMaxCameras when none), and whether this one is such a second.
- `double g_time_pairing = 0.0;`: Time the images took since the last "replay" log line: pairing the draws, filling the moved positions, sending the commands again.
- `std::vector<uint8_t> g_world_camera;`: Per draw of the recorded frame paired with the previous one: the camera whose motion it follows (kMaxCameras: none) and whether it moves in the world. A thing that stands still takes the camera's motion alone, as the draws that follow the camera do, so a footprint and the ground under it get the very same depth.
- `double g_forward[kMaxCameras][4][4];`: Per camera: the inverse of its motion, from a clip position of the previous frame to the same point of the world seen by this frame's camera.
- `bool moves_in_world(const RecordedDraw& draw, const RecordedDraw& before, const CameraM...`: True when some vertex of the draw is not where the camera's motion alone takes its pair: the thing moves in the world. The limits are well above the motion fit's error on the scenery (about 1e-5 of the screen, 2e-7 in depth).
- `if (std::isnan(a[0]) || std::isnan(b[0]) || a[3] <= 0.0f || b[3] <= 0.0f || std::fabs(b...`: Points off screen or near the eye are left out: dividing by a w close to 0 blows the fit's error up.
- `Stillness stillness(const std::array<float, 4>& a, const std::array<float, 4>& b, const...`: Whether one vertex stands still in the world under a camera's motion, the test `moves_in_world` runs on a draw's sample points.
- `bool replay_between(const std::vector<std::pair<uint32_t, uint32_t>>& matches, float t,...`: Draws the recorded frame again with each matched draw at time t between the previous frame (0) and this one (1).
- `size_t follow = stillness(*a, *b, g_cameras[camera]) == kStill ? camera`: Inside a draw that moves, a vertex that stands still follows the camera like the still draws do. A vertex shared by a still piece and a moving piece of one model was placed by two formulas, up to half the still limit apart: thin lines of background between the polygons of PokÃ©mon and of the ground on the images in between (user captures of 2026-11-11).
- `float motion[kMaxCameras][4][4];`: Each camera's motion from this frame back to the previous one, taken as far as time t asks.
- `const double (*forward)[4] = g_forward[camera];`: A thing that moves in the world: its previous place seen by this frame's camera, then halfway in the world, then through the camera's motion like the ground under it.
- `static std::vector<std::pair<const RecordedTarget*, ReplayTarget*>> twins;`: The copy targets' twins start from the frame's beginning; draws sampling a target sample its twin.
- `enum PairKind : uint8_t { kNoPair, kCountChanged, kSameCount, kOnly };`: Pairs this frame's draws still unmatched with the previous frame's free draws of the same key, in drawing order. A trusted key keeps its pairs however far they move, only counting them toward a camera cut: a near piece of ground moves a lot on screen, and leaving it at the new frame's place opens cracks with its neighbours. How sure a pair made by drawing order is: the key was drawn once in each frame, as many times in both, or not.
- `template <typename Body>`: Runs body(i) for i in [0, count) on the GPU thread and a few helper threads. The bodies only read shared state and write their own slots.
- `void match_by(uint64_t RecordedDraw::*key, bool trusted, std::vector<uint32_t>& partner...`: Pairs the draws of one key kind. Whether a pair is close enough is checked after, for all candidates at once.
- `struct Slot`: The previous frame's draws by key, in an open-addressing table: each slot holds the next free draw of its key and how many draws of each frame carry the key.
- `uint32_t g_match[6] = {};`: Places every draw found in the previous frame at evenly spaced times between the two frames and runs the frame's commands again for each, into the interpolated images. Returns how many were made. Draws since the last "replay" log line: paired with a key drawn once, paired by order, rejected, unpaired from a list or an array, unpaired inline, following their camera.
- `void find_cameras(const std::vector<uint32_t>& partner, const std::vector<uint8_t>& kind)`: Finds each projection's camera motion from the pairs that cannot be wrong: keys drawn once in both frames.
- `constexpr size_t kMaxSamples = 8192;`: Three points a draw for the first draws, then one: the whole scene stays sampled, far parts included, at a bounded cost.
- `static std::vector<std::array<float, 4>> rest_from;`: The game draws part of the scene with one projection but another view: far scenery placed around the camera moves differently from near ground when the camera follows Pikachu. Fitted as one, that whole part counted as moving, some of its pieces did not, and the two kinds were placed by different formulas: thin lines of sea showed between pieces of ground. The pairs the first motion leaves out get a motion of their own.
- `bool near_camera_path(const RecordedDraw& draw, const RecordedDraw& before, const Camer...`: True when the draw's pair sits about where the camera's motion alone would have put it: the pair is right, and the thing barely moves in the world.
- `std::vector<std::pair<uint32_t, uint32_t>> g_matches;`: The draws of the recorded frame paired with the previous frame's, as (draw, partner) with partner a draw of the previous frame or kByCamera + camera; whether the frame is a camera cut.
- `void pair_frame()`: Pairs the recorded frame with the previous one.
- `static std::vector<uint32_t> partner;`: Exact keys first, so copies of one mesh pair with themselves; then the plain keys for meshes whose arrays move.
- `constexpr uint8_t kNoDraw = 255;`: Per draw: the g_match column it counts in (kNoDraw for none), and its partner or kByCamera + camera when it moves in the images (kUnmatched when it stays at the new frame's place).
- `if (pair && camera < kMaxCameras && kind[d] != kOnly && fixed_on_screen(draw, g_previous...`: A paired draw whose positions are the same in both frames is fixed on the screen and stays with its pair. On the 2D projection the "camera" is fitted from the HUD pieces drawn once; when the minimap slides away (character switch, scene camera) that motion is the slide, and at more than 0.05 of the screen per frame the HUD pieces drawn several times no longer passed `near_camera_path`: they followed the slide by half a step on the image in between, the screen trembled and a black bar kept under the picture rose about 10 rows into view (user capture of 2026-11-12, images 079 to 087). Reasoned from the capture and the code; the switch screen could not be opened in a headless run (the scripted A only makes Pikachu wave there).
- `if (camera < kMaxCameras && !(pair && (kind[d] == kOnly || (kind[d] == kSameCount && ne...`: A mesh drawn many times (grass, trees, water tiles, shadows, footprints) is paired by drawing order, and the order shifts whenever one copy enters or leaves the view: each copy then slid toward its neighbour's old place for one image, which made them blink and tremble. Such a copy follows the camera instead, as a thing that stands still in the world, unless its pair is sure or sits where a slow mover would have been.
- `column[d] = 2;`: A mesh drawn many times under a projection with no known motion, like the footprints and shadows the game draws from above into the ground's decal texture: its order shifts whenever one is added, and each copy slid toward its neighbour's place, ghost footprints on every image in between. It stays put.
- `column[d] = partner[d] == kRejected ? 2 : (draw.exact != 0 ? 3 : 4);`: Left at the new frame's place: rejected for moving too far, or with nothing to pair with.
- `g_camera_cut = moved_too_far > 8 && moved_too_far * 3 > g_matches.size();`: Most draws jumping at once is a camera cut: the frame is shown twice rather than blended with the last scene.
- `bool g_paired = false;`: The pairs in g_matches belong to the recorded frame.
- `int g_display_hz = 0;`: Refresh rate of the window's monitor, 0 until a frame is shown: interpolated frames need 100 Hz or more.
- `void end_record()`: The recorded frame becomes the previous one, and the next frame is recorded when 120 fps is on.
- `g_vertex_cursor = kVertexCapacity;`: The next frame starts at the beginning of the vertex buffer.
- `double g_period = 1.0 / 60.0;`: Time between real frames, averaged: 1/60 in the zones, 1/30 in the title menu.
- `double g_slot = 1.0 / 120.0;`: Time between two shown images.
- `struct FrameEnd`: What a frame copy leaves for finish_frame, with the clear that came with the copy: it belongs to the next frame.
- `const char* const g_dump_sequence = std::getenv("WP_DUMP_SEQUENCE");`: WP_DUMP_SEQUENCE=<first frame>,<frames>: saves each frame's interpolated images then the frame itself, in the order they are shown, to captures/seq. Images are made for every frame, late or not.
- `void finish_frame()`: The interpolated images are drawn here, after the game got its draw-done or token signal, not at the frame copy: the game waits for that signal, and making it wait for the replays too slowed its loop until, in the pause menu and the camera, it took fresh buffers each frame and ran out of heap (garbled backgrounds, then a crash).
- `uint32_t between = static_cast<uint32_t>(std::max(1.0, std::round(g_period * 120.0) - 1...`: Enough images in between to show 120 per second.
- `constexpr size_t kBehindBytes = 256 << 10;`: Much of the next frame already waiting means the GPU thread is behind: no images until it catches up.
- `bool late = gx::take_frame_held();`: The game's timers had to wait for this frame's token: the thread has no time left for images.
- `bool saved = end.copied && g_recording && save_efb(g_efb_after);`: A late frame still keeps its EFB, or the next frame could not have images either.
- `static auto report = started;`: Once a second: how many frames got images, how many were late, and what the images cost this thread.
- `g_efb_before_valid = saved;`: The next frame's replays start from this frame's end only when it was kept and the EFB keeps its size.
- `static auto last = std::chrono::steady_clock::now();`: Pacing report: how evenly frames reach the screen, once a second.
- `struct ShownImage`: Images waiting for their time on screen: copies, so the next frame can be drawn before they are shown.
- `void service_deferred()`: Shows the oldest image whose time has come.
- `size_t skipped = ready > 2 ? ready - 2 : 0;`: Two due at once is a late call: the older goes now and the other at the next call. More are skipped.
- `g_last_shown = now - image.due < std::chrono::duration<double>(g_slot) ? image.due : now;`: Keeps the cadence from the planned time unless the image is late by more than a slot.
- `bool present_frame(void* window_handle, double aspect)`: With 120 fps the interpolated images go out first and the real frame last, evenly spaced over a frame.
- `static uint64_t queued_frame = 0;`: The game presents every retrace; a frame drawn at 30 fps comes twice and is queued once.
- `auto now = std::chrono::steady_clock::now();`: The new images follow the ones still waiting, unless those reach half a frame ahead: then the screen is behind and they are dropped so it catches up.

### `engine/src/gpu/gx_render_common.cpp`

- `double scale[4] = {};`: Each coordinate is scaled to a similar size first: depth is millions of times larger than x and y.
- `constexpr int kRounds = 5;`: Five rounds: all pairs, then only those the last fit placed within the limit, in units of the screen's half. The last two limits follow the error of the kept pairs: in a fast turn a few movers within 0.01 still pulled the fit off by a pixel or more, enough to split pieces of ground that should meet.
- `double limit = kLimits[round];`: The first fit takes the movers in too and is off for everyone by about their share of the error: the limit after it grows with the typical error, so the scenery is kept and the movers left out.
- `double system[4][8] = {};`: Least squares: (sum f f^T) row = sum to_r f, for the four rows at once.
- `double trace = system[0][0] + system[1][1] + system[2][2] + system[3][3];`: A flat scene (a menu: one depth, w = 1) leaves directions with no data: a small weight keeps them solvable.
- `for (int r = 0; r < 4; r++)`: system[j][4 + r] / system[j][j] is the weight of scaled input j in output r.

### `engine/src/gpu/gx_render_gl.cpp`

- `bool interpolating()`: shortcut: frame interpolation is only in the Direct3D 11 renderer, port it when Linux and macOS builds need 120 fps.

### `engine/src/input/gamepad.cpp`

- `constexpr float kMenuStick = 0.6f;`: In the menu the stick counts only along its main axis and pushed well past the dead zone, so moving down a list never slips sideways onto a value.

### `engine/src/input/keymap.cpp`

- `static constexpr UINT kScanCodes[26] = {0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23,...`: Scan codes of the letters on a US keyboard, A to Z.
- `return us;`: shortcut: SDL builds keep the US letters; map them through SDL_GetKeyFromScancode when Linux needs it.

### `engine/src/platform/options.cpp`

- `if (type != Kind::Choice && type != Kind::Slider && !(type == Kind::Toggle && action ==...`: Left and right change values and sliders; an on/off box changes only when it is accepted.
- `std::string label_key = row.kind == Kind::Pad ? "keys." + key.substr(4) : key;`: A gamepad action reuses the label of the keyboard action with the same name.

### `tests/engine/runtime_tests.cpp`

- `const float camera[4][4] = {{0.99f, 0.0f, 0.00000002f, 0.03f}, {0.0f, 1.0f, 0.0f, -0.01...`: A camera that turned and stepped between two frames, as one matrix on clip coordinates with depth in the millions.
- `if (i % 5 == 0)`: One pair in five belongs to something that moves by itself.
- `for (auto& t : to)`: Pairs with nothing in common have no motion to find.
- `from.clear();`: A menu: everything at one depth with w = 1, shifted by a fixed step.
- `auto azerty = [](int letter) { return letter == 'W' ? 'Z' : letter == 'Z' ? 'W' : lette...`: AZERTY: the letters at the US positions of W, A, Q and Z.
- `auto us_file = [](const std::string& key)`: A file holding the US defaults moves to this layout's letters, unless one of them was changed.
- `std::function<std::string(const std::string&)> file_reader(...)`: Reads a settings file kept in a map, as `outdated` reads settings.ini.
- `custom["keys.a"] = "Space";`: A player who put Space on A keeps the old 2 key: the new default of 2 would share Space with A.
- `menu.select(1);`: An on/off box ignores left and right, so a stick drifting sideways never flips it.
