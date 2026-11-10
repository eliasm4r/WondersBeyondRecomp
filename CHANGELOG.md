# Changelog

All notable changes to WondersBeyondRecomp. Dates are when the change landed on `main`.

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
- `Jouer PokePark 2.bat` starts the game without a console window and keeps the last two logs.

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
