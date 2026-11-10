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
- F1 opens the options menu (tabs Display, Keyboard, Gamepad, Sound, Game). F10 no longer does anything.
- The title screen and cutscenes are pre-rendered 30 fps movies, so they stay at 30 fps (see "60 fps").
- Discord shows the game when `system.discord` is on and `discord_application` in the game description holds an
  Application ID; it talks to the local client over its IPC pipe and retries every 15 s.

## Indirect maps left unbound

`render::draw` bound only the maps named by TEV stage orders. A map read only by an indirect stage, such as the
normal map of the dash shockwave (64x64 IA8), stayed unbound and sampled as zero, the largest offset: the ring
showed the scene shifted by 128 texels. `used_maps` now adds the maps of the indirect stages (BP 0x27).
