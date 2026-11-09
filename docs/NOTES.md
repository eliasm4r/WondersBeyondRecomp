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

## Skipping the Wii Remote strap notice

- The strap notice is the scene `nsNile::nsScene::SceneSpecializerStrap`. While it is shown, 0x80210d90 runs every frame; it stops once A is pressed.
- Change: `analysis/observed_functions.csv` lists 0x80210d90, so the recompiler adds `wp::observe()` at its entry. While it ran in the last 300 ms, player 1's input holds A for 100 ms every 500 ms (`engine/src/input/input.cpp`). Scripted input (`WP_INPUT_SCRIPT`) turns this off.
- Verified: headless runs of 20 s. With the skip, the workload changes after about 5 s (batches per frame drop from about 125 to 80), as when A is pressed by hand. Without it (scripted input), it stays on the notice (about 120 batches) for the whole run.

## Removed Wii Party features

- Mii loading, the controls hint window, the sideways grip per minigame and the instruction-screen watch are gone, with their settings (`input.auto_grip`, `input.controls_hint`, `system.skip_notices`) and launcher pages. The grip is now a single per-game value (`sideways_grip` in `game::Description`).
