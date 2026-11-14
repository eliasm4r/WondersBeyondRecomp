<div align="center">

# Changelog

Every change to WondersBeyondRecomp, newest first.

**[Latest release](https://github.com/eliasm4r/WondersBeyondRecomp/releases/latest)** · **[All releases](https://github.com/eliasm4r/WondersBeyondRecomp/releases)** · **[README](README.md)**

</div>

<br>

## v0.2.0-alpha · 2026-11-14

> **Smoother and faster.** Up to 240 fps, shorter freezes when a zone appears, a faster start, an update check and controls made for the four attractions. [Download it here.](https://github.com/eliasm4r/WondersBeyondRecomp/releases/tag/v0.2.0-alpha)

### Added
- **Shader preloading.** Shaders compiled while playing are saved on the disk and loaded again at the next start, so arriving in a zone no longer makes the processor compile them again (about 10 s of processor time saved over a play session, more on slower processors).
- **144, 165 and 240 fps.** The frame-rate limit goes up to 240 with in-between frames, never above the screen's refresh rate (240 on a 144 Hz screen shows 144).
- **Faster start.** The hidden start-up screens run four times faster once the park data is loaded: the title appears about 2 seconds sooner.
- **Update check.** Options > Advanced shows the version and whether a newer release is out on GitHub; confirming it opens the download page. It can be turned off.
- **Controls for the four attractions.** Each attraction now plays with the keyboard, the mouse or a gamepad, without hunting for the Wii Remote motion keys.
  - **Cake Contraption:** the mouse aims by itself and left click throws. On a gamepad, the left stick aims and A or RT throws; whichever of the mouse and the gamepad moved last aims.
  - **Dance Inferno:** one key per move, drawn on the move's icon (arrows, E for the kick, a space bar for the poses, or the gamepad's buttons and PlayStation shapes when you play with a gamepad). Press it as the icon reaches the diamond, and hold it for moves that last several beats. Poses pressed on time now score GREAT.
  - **Power Bash:** each press of Q or E (LB or RB on a gamepad) strikes, and shaking the gamepad strikes for as long as you shake it, from the first jolt. Space drops the bomb, and the A on the bombs now shows the key or button that drops them (a space bar, or the gamepad's button).
  - **Flight of Fancy:** the movement keys or the left stick tilt gradually (up climbs, down dives), and Space flies faster; tilting the gamepad no longer steers.

### Changed
- **Lighter on the processor.** The game's own thread does about 15 % less work at 60 fps: the clock is read through the processor's counter instead of a slow Windows call, and the paired-single loads and stores of plain floats are compiled into the game code.
- **Smoother zone arrivals.** The longest freeze when a zone starts, about 0.4 s while its scripts looked up their resources one by one, is down to about 0.1 s, and textures decode several times faster, which removes most of the next freeze (0.18 s).
- **Compressed textures.** The game's compressed textures stay compressed on the graphics card (8 times less memory and upload), which halves the stutter when a zone appears.
- **The mouse cursor is hidden over the game by default.** Turn it back on with **Hide the mouse cursor** in the options. Settings saved by an earlier build keep their choice.

### Fixed
- Above 60 fps, the images in between were sometimes shown in bunches and some were dropped, mostly in dialogues and other light scenes: 240 fps fell to about 210 there. They now come at an even pace.
- Above 60 fps, a Pokémon's eyes and mouth slid across its face for a few images when its expression changed, as when it blinks.
- A crash in the game's memory allocator, about once in eight starts at 240 fps: the release of the graphics buffers could land in the middle of an allocation.

## v0.1.0-alpha · 2026-11-13

> **First public release.** PokéPark 2: Wonders Beyond on Windows at up to 120 fps, installed by a setup that builds the game from your own disc image in about 5 minutes. [Download it here.](https://github.com/eliasm4r/WondersBeyondRecomp/releases/tag/v0.1.0-alpha)

### Added
- **RecompLauncher support.** RecompLauncher can now install the game from your disc image without the setup window, start it and change its main settings (resolution, frame rate, window, picture shape, V-Sync, smoothing, texture filtering, volume, game language).

### Changed
- **Much faster on laptops and slower processors.** The graphics card now places the vertices, keeps the models that do not change in its own memory and redraws the images in between at 90 and 120 fps, and the game thread rests while the game waits. A Ryzen 5 5600H laptop went from 25 to 42 fps to a **steady 60 fps** in the heaviest zone.
- **Gentler mouse camera.** At 100 % the mouse turns the free camera 2.5 times slower than before. The setting still goes up to 300 %.
- **Quieter default volume.** 100 % is now a third of the game's own level, which was too loud, and the slider goes up to 300 %. If you set the volume in an earlier build, the game now sounds three times quieter: raise it in the **Sound** tab.
- **Setup.** `Setup.exe` carries the game icon, and the window says the install takes about 5 minutes and that the disc image is the ROM file of the game. If Smart App Control refuses `Setup.exe`, start `python/Setup (if blocked).exe`.
- **Pikachu icon** for the game and the setup.
- **Game files in a `game` folder.** The saves, settings and extracted disc now sit in `game` next to the program. Installs from the test builds move them there by themselves on the next start or update.
- **Setup zip renamed** `WondersBeyondRecomp-Setup-Windows.zip`, since a macOS version is on its way.
- Each section of `settings.ini` is now written once.

### Fixed
- At 90 and 120 fps, the shadows of the Pokémon grew and turned for one image whenever the player changed direction.
- A rare crash a few minutes into play, in the check made before the game takes temporary memory.

<br>

---

<div align="center">

## Before the first release

These builds were shared for testing only.

</div>

### 2026-11-12

#### Added
- **A setup for other PCs.** A zip to unpack, with one window that asks for the disc image and a folder, downloads the build tools, builds the game there and adds a shortcut. It refuses any disc other than the European version and works with Smart App Control on.
- **A log file on every start**, `logs/pokepark2.log`, with the one before kept as `logs/pokepark2.previous.log`. It names the processor, the power source, the graphics card and its driver.

#### Changed
- **Higher frame rate in heavy zones.** Up to three helper threads prepare the vertices (processors with more than four cores). In the measured spot at 120 fps the game went from about 50 to its full 60 frames a second.
- **Performance overlay.** "Game" and "GPU thr." now show how much of each second the game and the graphics thread were really busy.

#### Removed
- The previous installer and its AppImage and offline packs; the setup replaces them.

#### Fixed
- Laptops with two graphics cards started the game on the slow one built into the processor.
- With a Wii Remote connected, the game read the keyboard, mouse and controllers tens of thousands of times a second; it now does so at the remote's own rate.
- Graphics cards that receive vertices slowly now get only the data each draw uses. A Radeon RX 6500M laptop went from 25-30 to 30-54 fps in the main zone.
- The free camera jumped up and down near walls, rocks and ledges.
- At 90 and 120 fps the screen trembled and black lines showed when the minimap slid away.
- Photos taken in the game were saved black with pink streaks.
- 90 and 120 fps stayed around 80 and 105 in heavy zones because images in between were dropped when a frame came a little late.
- Some Direct3D 11 sessions ran at half speed from start to end.

### 2026-11-11

#### Added
- **120 fps mode** (Direct3D 11): each frame is recorded and drawn again at evenly spaced times in between, with moving models blended and camera cuts kept sharp.
- **45 and 90 fps limits.** At 45 fps the game logic still runs 60 steps a second.
- **Free camera**, on by default: the mouse or the right stick turns the view around Pikachu, who walks toward the screen's direction. **C** (Wii B) brings the camera back behind him. Separate mouse and stick speeds and an up/down invert.
- **Performance overlay on F2**: frame rate, processor, graphics card and memory, in three levels of detail and any corner.
- **Play time**, counted across sessions and shown in the Game tab.
- **Color settings**: brightness, contrast, saturation and gamma, with an "Original colors" button.
- **Controller button names**: the Controls tab names the buttons of the connected Xbox, PlayStation or Switch controller.
- **A help line** under the options menu describes the selected setting.
- Default keys follow the keyboard layout (`Z Q S D` on AZERTY).
- Holding Ctrl turns a mouse button bound to 1 into A, like pointing with a Wii Remote.
- **Bug recorder** (Advanced tab): the screenshot key saves the last three seconds of images.
- A report of every game thread in the log when the game draws nothing for 15 seconds.

#### Changed
- **The options menu is drawn as the game's dialog bubble**, with six tabs: Screen, Game, Sound, Controls, Language and Advanced.
- **New default keys**: Space jumps and talks, left click dashes, right click shakes, E calls a partner, C turns the view, Tab opens the PokéPark Pad and P takes a photo. Keys you changed yourself stay.
- Keys are shown as written on your keyboard.
- Controllers keep working while the window is in the background.
- Higher frame rate in heavy zones on Direct3D 11: frame buffer copies no longer wait on the graphics driver.

#### Fixed
- Controller buttons given to Wii buttons in the menu were ignored.
- A crash, noise in the pause menu and black bars after leaving it at 90 and 120 fps.
- Endless loading after a long session.
- Flickering shadows, footprints, far scenery and sea at 120 fps.
- Pikachu torn apart on loading screens at 120 fps.
- Thin triangles sometimes vanished.

### 2026-11-10

#### Added
- **60 fps mode**, on by default; movies and cutscenes stay at their native 30 fps.
- **Options menu on F1** in the game's style, usable with keyboard, mouse and controller.
- Vsync, supersampled smoothing, anisotropic filtering, 16:9 or 4:3, rumble, stick deadzone, volume and a French menu.
- A shake of a controller with a motion sensor counts as a Wii Remote shake.
- **Discord Rich Presence**, a window title and a new icon.

#### Changed
- One pixel shader is compiled per texture setup, in the background.
- The first open zone went from about 52 to 60 fps.

#### Fixed
- The dash shockwave showed the scene shifted.
- An occasional black screen at boot.
- Phases running at 15 fps.
- The game was held to 50 fps.

### 2026-11-09

#### Added
- **First playable build**: boots straight to the title screen.

#### Removed
- Features inherited from Wii Party Recomp that do not apply to this game.
