# Changelog

All notable changes to WondersBeyondRecomp. Dates are when the change landed on `main`.

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

### Fixed
- Gamepad buttons given to A, B, 1, 2, +, − or HOME in the menu were ignored while the Wii Remote is held sideways,
  which is always the case in PokéPark 2.
- The game's frame alarms wait for the draw token the GPU thread still owes (100 ms at most): with the GPU thread
  a little late the game took fresh buffers every frame and ran out of heap in the pause menu.
- Flickering shadows, footprints, far scenery and sea at 120 fps, and thin lines of sea showing through the ground
  during camera turns.

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
