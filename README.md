# WondersBeyondRecomp — PokéPark 2 Recomp for PC

**PokéPark 2: Wonders Beyond** running natively on PC through static recompilation.

> [!NOTE]
> **Project status: playable, in development.** The game boots, plays and saves on Windows at up to 120 fps and high internal resolutions. Expect rough edges; see [CHANGELOG.md](CHANGELOG.md) for what changed.

## Overview

WondersBeyondRecomp translates the game's PowerPC code into C++ ahead of time and compiles it into a native PC program. The game and the Nintendo SDK code run translated; the console hardware (GPU, DSP audio, Bluetooth, IOS, disc and NAND) is reimplemented underneath.

**No game files are distributed.** You need your own copy of PokéPark 2: Wonders Beyond; the build reads your disc image.

Supported disc: Europe (`S2LP01`). Other regions are planned.

## Features

- **30, 45 or 60 fps** gameplay (`video.fps`, 60 by default), with movies and cutscenes kept at their native 30 fps
- **90 or 120 fps** through frame interpolation (Direct3D 11): images are drawn between the game's frames
- **Internal resolution** up to 6x, 16:9 or 4:3, optional supersampled smoothing, anisotropic filtering, vsync
- **Color settings**: brightness, contrast, saturation and gamma, applied to the game picture and screenshots
- **Free camera**: turn the camera around Pikachu with the mouse or the right stick, with its own speed for each and an up/down invert; **C** (Wii B) brings it back behind Pikachu; it can be turned off to keep the game's camera
- **Options menu (F1)** drawn as the game's dialog bubble, with six tabs (Screen, Game, Sound, Controls, Language, Advanced) and a help line for each setting, usable with keyboard, mouse or gamepad, without pausing the game
- **Performance overlay (F2)**: frame rate, processor, graphics card and memory use, in three levels of detail and in any corner
- **Play time** counted across sessions and shown in the options menu
- **Keyboard and mouse**: **Space** jumps and talks, **left click** dashes (hold it for Thunderbolt), **right click** shakes, **E** calls a partner, **C** turns the view, **Tab** opens the PokéPark Pad, **P** takes a photo; every action can be rebound, and keys are shown as written on your keyboard; hold **Ctrl** to aim the Wii Remote pointer with the mouse (left click then acts as A)
- **Gamepads** (XInput and SDL): the options menu names the buttons of the connected Xbox, PlayStation or Switch controller; rebinding, stick deadzone, rumble; hold **L3** to aim with the right stick; a sharp jolt of a gamepad with a motion sensor counts as a Wii Remote shake; gamepads keep working while the window is in the background
- **Sound**: volume, mute, Wii Remote speaker
- **Languages**: game and menu language (English, French, Spanish, Galician for the menu)
- **Discord Rich Presence** while the game runs (can be turned off)
- **Custom textures** loaded from `games/pokepark2/textures`
- Save backups, crash and hang reports, and a bug recorder that saves the last three seconds of images with the screenshot key

## Playing

After building (below), start the game with `build/out/pokepark2.exe`. Press **F1** for the options menu and **F2** for the performance overlay. Settings are stored in `games/pokepark2/settings.ini`, the play time in `games/pokepark2/stats.ini` and saves in `games/pokepark2/nand`.

## Roadmap

- A steady 120 fps on more PCs
- Shader cache on disk to remove first-time stutters
- A released installer, then Linux and macOS builds

## Building from Source

Requirements: Python 3.11+, CMake 3.20+, Ninja, a MinGW-w64 GCC 13+ and [nodtool](https://github.com/encounter/nod).

Put your disc image at `games/pokepark2/disc/pokepark2.rvz` (`.iso` and `.wbfs` also work with nodtool), then:

```bash
nodtool extract games/pokepark2/disc/pokepark2.rvz games/pokepark2/extracted
python tools/fetch_sdl.py
python recompiler/recomp.py
python recompiler/dsp/recomp_dsp.py
cmake -S . -B build/out -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/out --target pokepark2
./build/out/pokepark2
```

Run the tests with `ctest` in `build/out`. Technical notes on the fixes so far are in [docs/NOTES.md](docs/NOTES.md).

### Installer

```bash
python tools/make_setup.py
```

`build/dist/WondersBeyondRecomp-Setup.zip` is for a PC without any of the tools above. It holds the official Python with its window toolkit, the scripts of `installer/` and the sources of this repository, and nothing of the game. Unzip it and start `Setup.exe`: it asks for a disc image of the European version and a folder, downloads the build tools, translates and compiles the game on that PC and adds a shortcut. It only installs; every setting is in the game (F1). It is at the test stage and Windows only.

The function list in `games/pokepark2/analysis/` was produced with [decomp-toolkit](https://github.com/encounter/decomp-toolkit).

## Credits

WondersBeyondRecomp is built on [Wii Party Recomp](https://github.com/arelkair/wiiparty-recomp) by arelkair, whose recompiler and Wii runtime do most of the heavy lifting. Third-party code and libraries are listed in `THIRD_PARTY_NOTICES.md`.

This project was developed with the help of [Claude](https://claude.com/claude-code), Anthropic's AI assistant.

## Legal & Licensing

Independent fan project, not affiliated with or endorsed by Nintendo, Creatures Inc., Game Freak or The Pokémon Company. Pokémon and PokéPark are trademarks of their respective owners. Users must supply their own legally obtained game files.

Licensed under the GPL-3.0-or-later.
