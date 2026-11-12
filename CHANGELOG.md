# Changelog

All notable changes to WondersBeyondRecomp. Dates are when the change landed on `main`.

## 2026-11-12

### Changed
- Higher frame rate in heavy zones: the vertices of consecutive draws are prepared on up to three helper threads
  (processors with more than four cores). In the measured spot at 120 fps the game went from about 50 to its full 60
  frames a second, each with its image in between.

### Fixed
- Photos taken in the game were saved black with pink streaks; they now hold the picture.
- 90 and 120 fps stayed around 80 and 105 in heavy zones although the game ran at its full speed: images in between
  were dropped whenever the game finished a frame a little late.
- Direct3D 11: some sessions ran at half speed from start to end because every batch waited on the graphics driver.

## 2026-11-11

### Added
- 120 fps mode (`video.fps=120`, Direct3D 11 only): each frame is recorded, every draw is paired with the one it was
  in the previous frame, and the frame is drawn again at evenly spaced times in between. Draws that stand still follow
  the camera's fitted motion, movers are blended in the world, and camera cuts are shown as they are.
- Default key bindings follow the keyboard layout (Z, Q, S, D on AZERTY); bindings files still holding the US letters
  move to the local ones once.
- Performance overlay on F2 (or in the Advanced tab): frame rate, processor, graphics card and memory figures, in three
  levels of detail and in any corner of the window.
- Play time, counted in `games/pokepark2/stats.ini` and shown in the Game tab.
- 45 and 90 fps limits. At 45 fps the game's logic still runs 60 steps a second; 90 fps uses the frame interpolation
  (Direct3D 11, on a screen of 85 Hz or more) and shows every other game frame.
- A help line under the options menu's rows describes the selected setting.
- The Controls tab names the buttons of the connected controller (Xbox, PlayStation or Switch, after the last one
  used; Xbox names without one) and never shows "Automatic". Each key and button row says what it does in the game.
- While the pointer key (Ctrl) is held, a mouse button bound to 1 clicks A instead, like pointing with a Wii Remote.
- Color settings in the Screen tab: brightness, contrast, saturation and gamma, 100% being the original picture, and
  an "Original colors" button. They change the game picture, cutscenes and F9 screenshots, not the F1 menu or the F2
  overlay, and apply while the menu is open.
- Free camera (on by default, Controls tab): the mouse or the right stick turns the camera around Pikachu and Pikachu
  walks toward the screen's direction. The camera stays where it was put; the game's "turn the view" button (C, Wii
  B) brings it back behind Pikachu. Mouse and stick speeds and an up/down invert are separate settings. While playing,
  the cursor is hidden and held in the window; holding Ctrl frees it to aim the pointer. Left bumper + right stick
  still tilts the Wii Remote and L3 + right stick still aims. The camera stays above the ground and in front
  of the walls the game knows about.

- Bug recorder (Advanced tab, Direct3D 11): keeps the last three seconds of images shown, and the screenshot key then saves
  them all to `screenshots/<name>_recent`, the interpolated ones marked `between`.
- When the game draws no frame for 15 seconds, the log gets a report of every game thread and of the IOS requests.

### Changed
- The options menu is drawn as the game's dialog bubble (orange edge, Nunito font) with six tabs in a column: Screen,
  Game, Sound, Controls (keyboard or gamepad), Language and Advanced.
- Gamepads keep working while the window is in the background.
- The Windows timer runs at 1 ms, so images 8 ms apart are shown on time.
- In the options menu the stick moves only along its main axis, and on/off boxes change only when accepted.
- New default keys: Space and Enter jump and confirm (2), left click and Backspace dash and attack (1), right click
  shakes, E calls a partner (A), C turns the view (B), Tab opens the PokéPark Pad (+), P takes a photo (−), and the
  tilts move to I, J, K, L. Shift and the 1 and 2 keys are free. Files still holding the old defaults move to the new
  ones once (`input.bindings_version`); keys a player changed stay.
- Keys are shown as written on the keyboard (`&` rather than 1 on AZERTY, "Left click", "Space" in the menu
  language).
- Gamepad bindings are stored as real buttons (`pad.one=east,west`, `pad.swing_up=left_shoulder+dpad_up`); `auto`
  is still read and replaced by the default. Giving a button to an action in the menu takes it off the others.
- Higher frame rate in heavy zones on Direct3D 11: the frame buffer copies the game makes about 50 times a frame no
  longer wait on the graphics driver (38 to about 50 frames a second in the measured spot at 120 fps).

### Fixed
- Gamepad buttons given to A, B, 1, 2, +, − or HOME in the menu were ignored while the Wii Remote is held sideways,
  which is always the case in PokéPark 2.
- The game's frame alarms wait for the draw token the GPU thread still owes (100 ms at most): with the GPU thread
  a little late the game took fresh buffers every frame and ran out of heap in the pause menu.
- Crash, noise in the pause menu and black bars after leaving it at 90 and 120 fps: with the GPU thread a frame late
  the game dropped the frees of its temporary buffers and ran out of memory. Such frees are now kept and the game
  waits for the GPU thread when a buffer does not fit.
- Face culling is computed in double precision: thin triangles were sometimes taken for back faces.
- Play time is no longer counted in headless runs.
- Flickering shadows, footprints, far scenery and sea at 120 fps, and thin lines of sea showing through the ground
  during camera turns.
- Endless loading after a long session: every opened file took a new descriptor number, and the 228th overwrote a
  timer of the game. Numbers are reused as on the console.
- Pikachu torn apart on loading screens at 120 fps.
- The log file kept the hang report only until the next line was written.

## 2026-11-10

### Added
- 60 fps mode (`video.fps=60`, on by default); movies and cutscenes stay at their native 30 fps.
- EURGB60 video mode, so the game is no longer held to 50 fps.
- Options menu on F1, in the game's style, with Display, Keyboard, Gamepad, Sound and Game tabs, usable with keyboard, mouse and gamepad.
- New settings: vsync, fps counter in the title bar, supersampled smoothing, anisotropic filtering, 16:9 or 4:3, gamepad rumble and deadzone, volume, menu language (French added).
- The pointer follows the mouse only while Ctrl is held, and the right stick only while L3 is held; both are rebindable.
- A sharp jolt of a gamepad with a motion sensor simulates a Wii Remote shake.
- Discord Rich Presence.
- Window titled "WondersBeyondRecomp" and a new icon.

### Changed
- One pixel shader is compiled per TEV configuration, in the background.
- EFB copies made again every frame stay on the GPU instead of being read back to RAM.
- Draws that change no BP, konst or XF control register join the pending batch directly: the first open zone went from about 52 to 60 fps.

### Fixed
- The dash shockwave showed the scene shifted by 128 texels: texture maps read only by indirect stages were never bound.
- Intermittent black screen at boot (`__AI_SRC_INIT`).
- Phases running at 15 fps: back-to-back decrementers are delivered promptly and EFB read-backs are batched.
- A released texture could stay bound to a draw; textures get exact cache keys.
- EFB copies to null buffers no longer reach RAM; save file attributes.

## 2026-11-09

### Added
- First playable build: boots straight to the title, IOS requests answered faster.

### Removed
- Features inherited from Wii Party Recomp that do not apply to this game, including the Tab grip toggle.

### Fixed
- GX draw-sync tokens and IOS `/dev/fs` ReadDir.
