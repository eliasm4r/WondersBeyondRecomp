# WondersBeyondRecomp — PokéPark 2 Recomp for PC

**PokéPark 2: Wonders Beyond** (PokePark 2 Recomp) running natively on PC through static recompilation.

> [!WARNING]
> **Project status: early development.** The game's code is translated and builds, but the game does not run yet. There is nothing to play for now.

## Overview

WondersBeyondRecomp translates the game's PowerPC code into C++ ahead of time and compiles it into a native PC program. The game and the Nintendo SDK code run translated; the console hardware (GPU, DSP audio, Bluetooth, IOS, disc and NAND) is reimplemented underneath.

**No game files are distributed.** You need your own copy of PokéPark 2: Wonders Beyond; the build reads your disc image.

Supported disc: Europe (`S2LP01`). Other regions are planned.

### Goals

- Stable, faithful gameplay
- Higher internal resolution and widescreen
- High frame rates (120 FPS and more) through frame interpolation
- Custom and upscaled textures
- Keyboard, mouse and modern gamepads
- Windows first, then Linux and macOS

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

The function list in `games/pokepark2/analysis/` was produced with [decomp-toolkit](https://github.com/encounter/decomp-toolkit).

## Credits

WondersBeyondRecomp is built on [Wii Party Recomp](https://github.com/arelkair/wiiparty-recomp) by arelkair, whose recompiler and Wii runtime do most of the heavy lifting. Third-party code and libraries are listed in `THIRD_PARTY_NOTICES.md`.

This project was developed with the help of [Claude](https://claude.com/claude-code), Anthropic's AI assistant.

## Legal & Licensing

Independent fan project, not affiliated with or endorsed by Nintendo, Creatures Inc., Game Freak or The Pokémon Company. Pokémon and PokéPark are trademarks of their respective owners. Users must supply their own legally obtained game files.

Licensed under the GPL-3.0-or-later.
