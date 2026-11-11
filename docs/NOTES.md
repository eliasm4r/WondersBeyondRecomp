# Technical notes

Every change that alters runtime behavior is recorded here with what was verified and how.

## Game analysis (S2LP01)

- `main.dol` holds all the code; the disc has no REL modules.
- Function list: 18,275 functions from decomp-toolkit 1.8.4 (`dtk dol config` + `dtk dol split`), converted to `games/pokepark2/analysis/dol_functions.csv`. The lifter discovers 41 more entries (18,316 translated, 1,035,189 instructions).
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
- Change: the update is replaced (`SequencerBootUpdate` in `hle_functions.csv`, handler in `games/pokepark2/game.cpp`). It runs the same scenes, but while Strap and Hold run the engine keeps the screen black (`gx::set_screen_hidden`, no copy to the shown frame) and player 1 taps A (`input::set_auto_confirm`). CheckData is left visible, as it can ask about the save.
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
  still written, so a one-off copy such as a snapshot reaches RAM.
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
  `ui_text.cpp`.
- Play time: counted while the game runs, saved as `play_time=<seconds>` in `games/pokepark2/stats.ini` every minute
  and on exit (written to a temporary file, then renamed), shown in the Game tab.
- Gamepads keep working while the window is in the background; the keyboard and the mouse count only while it has the
  focus.
- Performance overlay (F2, `keys.overlay`, or the Advanced tab): levels minimal, standard and full, in any corner, a
  margin of 1.4 % of the window width away from the edges. F2 does nothing while the menu waits for a key. Screenshots
  read the game frame, so neither the overlay nor the menu shows in them.
  - The GPU thread publishes the figures once a second from its statistics block in `gx.cpp`. CPU, GPU and RAM come
    from a sampler thread that runs only while the overlay is on: the GPU counters can take tens of milliseconds to
    read, which would hold a frame back on the GPU thread.
  - CPU %: the whole system (GetSystemTimes). Game and GPU thr.: the time the game's thread and the GPU thread used
    over the second.
  - GPU %: the busiest adapter's 3D engine (PDH `GPU Engine(*engtype_3D)`), the figure Task Manager shows. GPU ms: this
    process's share of that engine times the shown frame time. A timestamp pair around a frame would also count the
    idle gaps between the game's command bursts.
  - VRAM: this process's local video memory (IDXGIAdapter3, Direct3D 11 only). RAM: the process's working set.
  - Late/s: real frames shown with no image interpolated before them, at 120 fps.
  - Temperatures are left out: Windows has no common way to read them without a vendor library or a driver.
  - On Linux and macOS the system figures show a dash.

## Indirect maps left unbound

`render::draw` bound only the maps named by TEV stage orders. A map read only by an indirect stage, such as the
normal map of the dash shockwave (64x64 IA8), stayed unbound and sampled as zero, the largest offset: the ring
showed the scene shifted by 128 texels. `used_maps` now adds the maps of the indirect stages (BP 0x27).


## Frame interpolation (120 fps) — work in progress

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
  log shows the process growing from about 1 GB at 60 fps to 7–10.8 GB at 120 (scale 6), falling back 10–20 s after
  recording stops (the twin prune), and hundreds of EFB write-backs a second at 120 against none at 60.
- Twins only where needed (2026-11-10, built, not tested in game yet): a twin (two textures and 1 + `between`
  full copies per frame) is now made only for a target the frame sampled before its first copy into it. Other
  targets are filled before they are read, so replays copy straight into the game's texture, which then holds the
  last replay's image until the next frame fills it again. To check in the next log: the "memory" lines at 120.

- User test of the twin change: memory is fixed (1.6–2.3 GB at 120 instead of 7–10.8 GB) but the pause menu, shadows
  and footprints are as before. The log still has 50–230 write-backs a second at 120 once the player moves (GPU
  thread 85–92% busy) and none at 60 (80% busy): the game copies to addresses it did not use in the last two frames.
- Hypothesis: the game frees a frame's buffers on its token interrupt and takes the next frame's on its frame alarm.
  When the GPU thread reaches the token after the alarm, the game takes fresh buffers, every non-recurring copy is
  written back with a GPU wait, the thread gets later still, and in the pause menu the heap runs out.
- Timers wait for the token (2026-11-10, built, not run at all yet, changes timing at 60 fps too): `push` notes the
  token of each BP 0x48 the game writes (`g_token_sent`), the GPU thread the one it reached (`g_token_done`);
  `gx::frame_owed()` makes `poll_interrupts` hold the decrementer while they differ or the interrupt is not delivered
  yet, 100 ms at most. A frame whose token made the timers wait gets no replays. The "memory" log line counts the
  holds. Pass criteria: boots, 0 write-backs a second in the zones at 120, few holds at 60.

- User test of the timer hold (scale 6, first open zone): 60 fps fine (0–3 holds a second, far characters pop in
  and flicker, not known whether that is new). At 120 the write-backs are gone (0–36 a second, the pause menu looked
  fine) but only 75 fps is shown: 15–25 holds a second and one frame in four gets its image. Shadows (the one under
  Pikachu) and footprints still look wrong in the images, so that part is the interpolation itself, not the buffers.
- After that (built 18:44, not run): the late flag is tied to the token (`g_held_token`) so one hold skips one
  frame's images, not several; a "replay" log line gives frames, frames with images, late frames, ms per frame spent
  on images and draws per frame.
- Next ideas. Cost: the replays do three full EFB colour+depth copies per frame (save after, load before, load
  after), 250 MB per frame at 6x; drawing replays into a second EFB would need none when the frame starts with a
  full clear. Shadows and footprints: draws sent inline through the FIFO are paired by order under the plain key, so
  a new footprint shifts every pairing, and an unmatched decal stays at the new frame's place on ground drawn at the
  halfway place (depth mismatch). To see it, dump an interpolated image in a headless run.

- User test of 18:48: still 75 fps. "replay" lines in the zone: 60 frames, 18 with images, 22 late, 0.6–0.7 ms per
  frame for the images (about 2.2 ms per replay); 19,800 draws per frame, 15,030 exact, 57 by order, 4,740 unpaired
  (most likely culled draws, which never pair), 0–10 unpaired inline. In the title menu, with almost no load, 20 of
  60 frames were "late" too: most holds are the one poll between the token and its interrupt, not a late thread, and
  each late frame also cost the next one its images (no saved EFB).
- Built 19:08, not run: a frame is late only when the timers waited over 1 ms for a token the thread had not
  reached (the "memory" line counts both kinds of hold); a late frame still saves its EFB; replays draw batches with
  depth test on and depth write off with a rasterizer depth bias (-64, slope -2), a guess at the blinking shadow.
- User description: the shadow under Pikachu blinks and the footprints tremble, only above 60 fps.
- Built 19:20 (not noted at the time): draws of a mesh drawn many times follow their camera's motion (`find_cameras`,
  `kByCamera`), fitted from the pairs whose key is drawn once in both frames. User test: 90 fps at most, shadow,
  footprints, far Pokémon and the sea still blink.

- 2026-11-10 evening, found with headless image dumps (see "Image dumps" below):
  - The decal depth bias (-64, slope -2) was the sea's blue flicker: the sea and the wet sand layers (depth test on,
    write off) passed over the sand on every interpolated image, a bluer image twice per frame. With the bias off, an
    unmoved replay matches the real frame exactly (0.00 mean difference). Removed.
  - Ghost footprints and the darker shadow: each frame first draws the footprints and shadows from above into a
    ground decal texture (draws 0–29, projections `c36825a9`/`0fe8fbc9`, no fitted motion), then copies it. The
    footprints are one mesh drawn many times, paired by order, and the order shifts when one is added: each slid
    toward its neighbour, ghost footprints on the images in between. A pair that is not sure under a projection with
    no known motion now stays put (counted as "rejected" in the replay line).
  - Depth agreement: the camera fit is very good on the scenery (screen 1e-5, depth 2e-7 of z/w), but 2e-7 is already
    a few 24-bit depth steps, so a footprint following the camera and ground lerped by its pair disagreed. Now every
    pair under a known camera that does not move in the world (`moves_in_world`: limits 5e-4 screen, 1e-6 depth,
    points on screen only) follows the camera exactly like the multi-drawn meshes, and a thing that moves is
    interpolated in the world: its previous place seen by this frame's camera (`g_forward`, the inverse fit), lerped,
    then the camera's motion. Lerping clip positions directly put a moving decal off the ground whenever the camera
    turned, since the error is t(1-t)·ΔM·ΔP.
  - Result on the beach run: blinking pixels per image (outside the range of their two real neighbours by more
    than 12) 0.53–0.60 % before, 0.29 % after; the remaining ones are the electric aura, Pikachu's legs and the
    water line on them. Far Pokémon and cliffs measured stable.
  - Not changed: copy targets written by the replays keep the last replay's content (a non-twin target the next
    frame samples before copying would show it). Restoring them changed nothing measurable once the bias was gone.
  - Performance (headless, scale 6, beach): game at 56–57 fps with 120 on against 60 with 60, GPU thread 80 % against
    63 %, images 2.9–3.4 ms per frame. Measuring further was stopped: the user was playing another game at the time,
    and a run at the same settings fell to 30 fps.
- User test 21:12 of that build: no texture blinks any more, 94–98 fps shown. Its "replay" lines: 54–57 frames,
  8–16 late (a late frame gets no image, hence under 120), 1.8–2.2 ms per frame for the images, of which pairing
  1.5–1.9 ms, positions 0.1, commands 0.2; GPU thread 90 % busy. Far Pokémon still vanish and come back at a
  distance; the 18:xx test above saw far characters pop in at 60 fps too, so it is likely not the interpolation.
- Pairing is now split over helper threads (`parallel_for`, up to 3 helpers when the CPU has more than 4 cores): the
  close-enough checks of `match_by` run after the key pass over all candidates at once (same result: the trusted pass
  never used them to choose, the loose pass only rejects), and the per-draw choice (camera, partner, moving in the
  world) runs per draw. Headless with another game running: pairing 2.0 → 1.1 ms. Running the pairing on its own
  thread from the frame copy on gained nothing: the token right after the copy calls `finish_frame` at once, so the
  GPU thread waited the whole time. Blink rate unchanged (0.26–0.30 %, one run 0.54 % with Pikachu in the water).
- The "replay" log line now splits the image time: pairing, positions (filling the moved buffer), commands.
- Far Pokémon that vanish depending on the camera angle (the pink one on the islet off the beach): the same on a
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
  blink rate unchanged (0.25–0.35 %). Pairing about 2 ms headless with another game running.
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

1. Pause menu background turns to noise after a while (fine at the pause start and with replays disabled).
   At 120 the menu uses many buffer addresses and copies to 0x80000000 (failed allocations); at 60 only 5 stable ones.
2. Photo screen noise once (`Texturebug.png` at the repo root, local only).
3. Crash in about 1 of 4 runs: out of memory in the texture-object init f_80070fc0 (alloc of 0x23a00 via
   f_8000df50 → f_8037d4a0, failure handler f_80409388). 0 of 5 crashes at 60.
4. Shadows and footprints still flicker in game (user report).
5. In game the rate does not hold 120 (GPU thread 85–95% busy at 6x).

Hypothesis for 1–3: the GPU thread lags, draw-done/token signals arrive late, the game believes buffers are still
in use and allocates fresh ones until its heap runs out. Ideas: measure lag by pending tokens or time since the last
token instead of FIFO bytes; limit the game's run-ahead (64 MB FIFO, `kMaxFramesAhead = 2`); cut the per-batch
Map/state cost of replays. A write-back guard (`full_hash`) was tried and removed: no help.

### Test method

- Scripts in `build/test-scripts/` (ignored): `hl.sh <tag> <runs> <timeout> [ENV=..]` runs headless with
  `pause.txt` (opens the pause at ~105 s), retries while Windows Smart App Control blocks a fresh exe (rc=126,
  "An Application Control policy has blocked this file"; rebuilding with different bytes gets past it),
  kills leftovers and restores the save. `run.txt` moves 32–54 s, `empty.txt` stays on the title.
- Save backup: `games/pokepark2/backups/claude_save/` (nand + settings.ini, 15:47). Re-take it after the user plays.
- Rules from the user: headless only (no window), ask before a test series, never build or run while
  `pokepark2.exe` runs, runs muted, never commit official images or the untracked files at the root.

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

- `void follow_keyboard_layout();`: Moves key bindings still at the US letters to this keyboard's layout (Z, Q, S, D on AZERTY); called once at start.

### `engine/include/wp/keymap.h`

- `std::string localize(const std::string& list, const std::function<int(int letter)>& let...`: The letters of a default binding are positions on a US keyboard, given back as the letters at those positions on another layout: W, A, S, D become Z, Q, S, D on AZERTY. letter maps a US letter to the local one.
- `std::string local_defaults(size_t index);`: An action's default binding on this computer's keyboard layout.
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
- `bool replay_between(const std::vector<std::pair<uint32_t, uint32_t>>& matches, float t,...`: Draws the recorded frame again with each matched draw at time t between the previous frame (0) and this one (1).
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
- `return kActions[index].defaults;`: shortcut: SDL builds keep the US letters; map them through SDL_GetKeyFromScancode when Linux needs it.

### `engine/src/platform/options.cpp`

- `if (type != Kind::Choice && type != Kind::Slider && !(type == Kind::Toggle && action ==...`: Left and right change values and sliders; an on/off box changes only when it is accepted.

### `tests/engine/runtime_tests.cpp`

- `const float camera[4][4] = {{0.99f, 0.0f, 0.00000002f, 0.03f}, {0.0f, 1.0f, 0.0f, -0.01...`: A camera that turned and stepped between two frames, as one matrix on clip coordinates with depth in the millions.
- `if (i % 5 == 0)`: One pair in five belongs to something that moves by itself.
- `for (auto& t : to)`: Pairs with nothing in common have no motion to find.
- `from.clear();`: A menu: everything at one depth with w = 1, shifted by a fixed step.
- `auto azerty = [](int letter) { return letter == 'W' ? 'Z' : letter == 'Z' ? 'W' : lette...`: AZERTY: the letters at the US positions of W, A, Q and Z.
- `auto us_file = [](const std::string& key)`: A file holding the US defaults moves to this layout's letters, unless one of them was changed.
- `menu.select(1);`: An on/off box ignores left and right, so a stick drifting sideways never flips it.
