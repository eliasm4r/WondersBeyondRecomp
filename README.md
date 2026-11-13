<div align="center">

<img src="games/pokepark2/res/pokepark2.png" width="140" alt="WondersBeyondRecomp icon">

# WondersBeyondRecomp

### PokéPark 2: Wonders Beyond, running natively on PC

A static recompilation of the Wii game: its code is translated to C++ ahead of time and built into a real PC program.<br>
No emulator, up to 120 fps, high resolutions, keyboard, mouse and modern controllers.

[![Latest release](https://img.shields.io/github/v/release/eliasm4r/WondersBeyondRecomp?include_prereleases&style=for-the-badge&label=release&color=f08a00)](https://github.com/eliasm4r/WondersBeyondRecomp/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/eliasm4r/WondersBeyondRecomp/total?style=for-the-badge&color=2e7d32)](https://github.com/eliasm4r/WondersBeyondRecomp/releases)
[![Platform](https://img.shields.io/badge/platform-Windows-0078d4?style=for-the-badge)](#requirements)
[![License](https://img.shields.io/badge/license-GPL--3.0-6a3d1b?style=for-the-badge)](LICENSE)

**[Download](https://github.com/eliasm4r/WondersBeyondRecomp/releases/latest)** ·
**[Install](#install)** ·
**[Controls](#controls)** ·
**[FAQ](#faq)** ·
**[Changelog](CHANGELOG.md)**

</div>

> [!IMPORTANT]
> **No game files are included.** You need your own copy of PokéPark 2: Wonders Beyond. The setup reads your disc image and builds the game on your PC.
>
> **Only the European version works** (`S2LP01`). The American and Japanese versions are refused by the setup.

> [!NOTE]
> **Alpha.** The whole game boots, plays and saves, but expect rough edges. Please [report what you find](https://github.com/eliasm4r/WondersBeyondRecomp/issues).

<br>

## Highlights

<table>
<tr>
<td width="50%" valign="top">

### Smooth
- **30, 45 or 60 fps** gameplay, 60 by default
- **90 or 120 fps** with images drawn between the game's own frames
- Movies and cutscenes kept at their native 30 fps
- Most of the work done by the graphics card, so laptops hold 60 fps too

</td>
<td width="50%" valign="top">

### Sharp
- **Internal resolution up to 6x**, in 16:9 or 4:3
- Supersampled smoothing and anisotropic filtering
- Brightness, contrast, saturation and gamma
- **Custom texture packs**

</td>
</tr>
<tr>
<td valign="top">

### Played your way
- **Keyboard and mouse** with every key rebindable
- **Xbox, PlayStation and Switch controllers**, with their own button names in the menus
- **Real Wii Remotes** and their speaker
- **Free camera**: turn the view around Pikachu with the mouse or the right stick

</td>
<td valign="top">

### Made for PC
- **Options menu on F1**, drawn as the game's own dialog bubble, without pausing
- **Performance overlay on F2**
- Automatic save backups and a play time counter
- **Discord Rich Presence**
- Menu in English, French, Spanish and Galician

</td>
</tr>
</table>

<br>

## Install

| | |
|:---:|---|
| **1** | Download **`WondersBeyondRecomp-Setup.zip`** from the [latest release](https://github.com/eliasm4r/WondersBeyondRecomp/releases/latest) and unzip it anywhere. |
| **2** | Start **`Setup.exe`**. |
| **3** | Pick your **disc image**: the ROM file of the **European** version (`S2LP01`), `.iso`, `.rvz` or `.wbfs`. |
| **4** | Pick an install folder and press **Install**. |
| **5** | Wait about **5 minutes** while the setup builds the game, then press **Play**. |

The setup downloads the build tools, translates the game code from your disc image, compiles it for your PC and adds a desktop shortcut. You only run it once: every setting lives in the game itself.

> [!TIP]
> **Windows refuses to open `Setup.exe`?** Smart App Control blocks programs that carry no signature. Open the `python` folder and start **`Setup (if blocked).exe`** instead: it is the same setup, run by the signed official Python.

<details>
<summary><b>Updating to a new version</b></summary>
<br>

Download the new setup and point it at the same folder. It sees the game already there, updates it without asking for the disc image again, and keeps your saves and settings.
</details>

### Requirements

| | |
|---|---|
| **System** | Windows 10 or 11, 64-bit |
| **Graphics** | A Direct3D 11 graphics card |
| **Disk space** | About 4 GB during the install |
| **Internet** | Needed by the setup to download the build tools |
| **Game** | Your own disc image of the **European** version (`S2LP01`) |

Other regions, Linux and macOS are planned.

<br>

## Controls

The defaults below can all be changed in the **Controls** tab of the options menu (**F1**). Keys follow your keyboard layout, so `W A S D` becomes `Z Q S D` on AZERTY.

<table>
<tr>
<th>Action</th>
<th>Keyboard and mouse</th>
<th>Controller (Xbox names)</th>
</tr>
<tr><td>Move</td><td><kbd>W</kbd> <kbd>A</kbd> <kbd>S</kbd> <kbd>D</kbd> or arrows</td><td>Left stick</td></tr>
<tr><td>Jump, talk, confirm</td><td><kbd>Space</kbd> or <kbd>Enter</kbd></td><td><kbd>A</kbd></td></tr>
<tr><td>Dash, attack (hold for Thunderbolt)</td><td><kbd>Left click</kbd> or <kbd>Backspace</kbd></td><td><kbd>B</kbd> or <kbd>X</kbd></td></tr>
<tr><td>Call a partner</td><td><kbd>E</kbd></td><td><kbd>Y</kbd> or <kbd>RB</kbd></td></tr>
<tr><td>Turn the camera</td><td>Mouse</td><td>Right stick</td></tr>
<tr><td>Camera back behind Pikachu</td><td><kbd>C</kbd></td><td><kbd>RT</kbd></td></tr>
<tr><td>Shake the Wii Remote</td><td><kbd>Right click</kbd></td><td><kbd>LT</kbd>, or jolt a controller with a motion sensor</td></tr>
<tr><td>PokéPark Pad</td><td><kbd>Tab</kbd></td><td><kbd>Start</kbd></td></tr>
<tr><td>Take a photo</td><td><kbd>P</kbd></td><td><kbd>Back</kbd></td></tr>
<tr><td>Aim the Wii Remote pointer</td><td>Hold <kbd>Ctrl</kbd> and move the mouse</td><td>Hold <kbd>L3</kbd> and move the right stick</td></tr>
<tr><td>Swing up and down</td><td>Mouse wheel, <kbd>T</kbd> <kbd>G</kbd></td><td><kbd>LB</kbd> + D-pad</td></tr>
<tr><td>Tilt</td><td><kbd>I</kbd> <kbd>J</kbd> <kbd>K</kbd> <kbd>L</kbd></td><td><kbd>LB</kbd> + right stick</td></tr>
</table>

| Key | |
|---|---|
| <kbd>F1</kbd> | Options menu |
| <kbd>F2</kbd> | Performance overlay |
| <kbd>F9</kbd> | Screenshot |
| <kbd>F11</kbd> | Full screen |

<br>

## Performance

The game spends most of its time drawing, so the graphics card does the heavy part: it places the vertices, keeps the models that do not change in its own memory and redraws the scene for the images in between at 90 and 120 fps.

| PC | Before | Now |
|---|:---:|:---:|
| Laptop, Ryzen 5 5600H and Radeon RX 6500M, heaviest zone | 25 to 42 fps | **steady 60 fps** |

Press **F2** at any time to see the frame rate and the load on the processor and the graphics card.

<br>

## FAQ

<details>
<summary><b>Where do I get the game?</b></summary>
<br>

From your own Wii disc of PokéPark 2: Wonders Beyond. CleanRip, run on a modded Wii, turns a disc you own into a disc image. This project does not provide one and does not help to find one.
</details>

<details>
<summary><b>Why does the setup build the game instead of shipping it?</b></summary>
<br>

The finished program is made from the game's code, which belongs to its owners and cannot be shared. So the setup ships only this project's own code and turns your disc image into the game on your PC.
</details>

<details>
<summary><b>Which version of the game works?</b></summary>
<br>

The European version (`S2LP01`), the one sold in Europe and Australia. The setup checks the disc image and says so if it is another one. Trimmed `.wbfs` copies are fine.
</details>

<details>
<summary><b>Where are my saves and settings?</b></summary>
<br>

In the install folder, under `games/pokepark2`: `settings.ini` for the settings, `stats.ini` for the play time and `nand` for the saves. Backups of the saves are made automatically.
</details>

<details>
<summary><b>The game crashed or froze. What do I send?</b></summary>
<br>

The file `logs/pokepark2.log` in the install folder, and `logs/pokepark2.previous.log` if you already started the game again. Open an [issue](https://github.com/eliasm4r/WondersBeyondRecomp/issues) with it and a few words on what you were doing.
</details>

<details>
<summary><b>Is it an emulator?</b></summary>
<br>

No. An emulator reads the game's PowerPC instructions one by one while you play. Here they are translated to C++ once, during the install, and compiled into a normal Windows program. Only the console hardware underneath (graphics, sound, Wii Remotes, storage) is recreated.
</details>

<br>

## Building from source

You need Python 3.11+, CMake 3.20+, Ninja, MinGW-w64 GCC 13+ and [nodtool](https://github.com/encounter/nod). Put your disc image at `games/pokepark2/disc/pokepark2.rvz` (`.iso` and `.wbfs` work too), then:

```bash
nodtool extract games/pokepark2/disc/pokepark2.rvz games/pokepark2/extracted
python tools/fetch_sdl.py
python recompiler/recomp.py
python recompiler/dsp/recomp_dsp.py
cmake -S . -B build/out -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/out --target pokepark2
./build/out/pokepark2
```

Run the tests with `ctest` in `build/out`. Technical notes on every fix so far are in [`docs/NOTES.md`](docs/NOTES.md).

To make the setup zip, run `python tools/make_setup.py`. It writes `build/dist/WondersBeyondRecomp-Setup.zip`, which holds the official Python with its window toolkit, the scripts of `installer/` and the sources of this repository, and nothing of the game.

<br>

## Credits

- Built on **[Wii Party Recomp](https://github.com/arelkair/wiiparty-recomp)** by arelkair, whose recompiler and Wii runtime do most of the heavy lifting.
- Function list made with **[decomp-toolkit](https://github.com/encounter/decomp-toolkit)**, disc reading by **[nod](https://github.com/encounter/nod)**.
- Third-party code and libraries are listed in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
- Developed with the help of [Claude](https://claude.com/claude-code), Anthropic's AI assistant.

## Legal

Independent fan project, not affiliated with or endorsed by Nintendo, Creatures Inc., Game Freak or The Pokémon Company. Pokémon and PokéPark are trademarks of their respective owners. You must supply your own legally obtained copy of the game.

Licensed under the [GPL-3.0-or-later](LICENSE).
