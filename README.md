<p align="center">
  <img src="docs/images/logo.jpg" alt="Metroid Fusion Recomp logo" width="80%">
</p>

<h1 align="center">metroid-fs-recomp</h1>

<p align="center">
  <b>Metroid Fusion, running natively on Android and PC.</b><br>
  No emulator: the game's own C code, compiled for your device, with save states, touch controls and video options.
</p>

<p align="center">
  <a href="https://github.com/kalasyan/metroid-fs-recomp/actions/workflows/build.yml"><img src="https://github.com/kalasyan/metroid-fs-recomp/actions/workflows/build.yml/badge.svg" alt="Build"></a>
  <img src="https://img.shields.io/badge/platform-Android%20%7C%20Linux%20%7C%20Windows%20%7C%20macOS-blue" alt="Platforms">
  <img src="https://img.shields.io/badge/region-EU%20%7C%20US-green" alt="Regions">
  <img src="https://img.shields.io/badge/built%20with-C%20%2B%20SDL3-orange" alt="C + SDL3">
  <img src="https://img.shields.io/badge/license-MIT%20%2F%20GPL--3.0-lightgrey" alt="License">
</p>

---

## ✨ What is this?

**metroid-fs-recomp** turns *Metroid Fusion* (GBA) into a native app for
Android and PC. It is built from the
[metroidret/mf](https://github.com/metroidret/mf) decompilation: the game code
is compiled as C for the device, with an SDL3 platform layer shared with its
sister project, [metroid-zm-recomp](https://github.com/kalasyan/metroid-zm-recomp).

The graphics, sound and text are read from **your own ROM** at startup, so the
repository and the APKs contain no game data.

> 🤖 **AI notice:** this project was developed with the help of AI tools
> (Anthropic's Claude), which wrote most of the code and documentation under
> my direction and review.

## 📸 Screenshots

<p align="center">
  <img src="docs/images/gameplay-1.png" width="49%" alt="Gameplay">
  <img src="docs/images/gameplay-2.png" width="49%" alt="Gameplay">
  <img src="docs/images/gameplay-3.png" width="49%" alt="Gameplay">
  <img src="docs/images/gameplay-4.png" width="49%" alt="Gameplay">
</p>

## 🎮 Features

- 📱 **Native Android app**, with a touch gamepad you can rearrange and resize.
- 💾 **Quick saves:** slots with thumbnail and date (`F5` / `F8`), on top of the normal in-game save.
- 🖥️ **Widescreen:** 16:10, 16:9, 2:1 or match your display. Menus, map and cutscenes stay at 3:2 in the center.
- 🎨 **Video options:** fullscreen, integer scaling, smooth filter, LCD grid or CRT scanlines, VSync.
- 🌊 **Smoothness options:** match the game speed to 60/120 Hz displays, or frame blending for 90/144 Hz.
- 🗺️ **Second screen:** the sector map or Samus's status on the bottom screen of dual-screen devices (AYN Thor) or in a second window on PC.
- ⏩ **Fast-forward** (x2 to x10, hold or toggle).
- 🎮 **Keyboard and gamepad** with remappable buttons and rumble.
- 🌍 **Europe and USA** builds. The European version keeps its five languages.
- 🔊 **The original sound engine**, translated to C.
- 🚫 **No BIOS and no emulator needed.**

## 📊 Status

- Everything the decompilation still had in assembly has been translated to C
  (`port/decomp/`), and every function has been compared against the original.
- Tested on PC with both ROMs: all 381 rooms of the station and the six
  sectors, the cutscenes and the conversations with the computer, the menus,
  saving and loading, game over, the ending and the credits.
- 🚧 It still needs thorough testing on Android and a full playthrough by
  hand. Details in [docs/ROADMAP.md](docs/ROADMAP.md).

## 💽 You need your own ROM

Each build only accepts its own region's ROM:

| Region | File | SHA-1 |
|---|---|---|
| EU | Metroid Fusion (Europe) | `cc46d54b70c1ee38c856ee6e58ec712136763389` |
| US | Metroid Fusion (USA) | `ca33f4348c2c05dd330d37b97e2c5a69531dfe87` |

## 🚀 Installation (Android)

Download `Metroid_Fusion-eu.apk` or `Metroid_Fusion-us.apk` from the
**[latest release](https://github.com/kalasyan/metroid-fs-recomp/releases/latest)**,
install the one for your region and pick your ROM when you open it. ▶️

Builds of every change to `main` are also in
**[Actions → build](https://github.com/kalasyan/metroid-fs-recomp/actions/workflows/build.yml)**
(the `mf-android-apk` artifact of the latest run).

## 🕹️ Controls

> 💡 The **options menu** pauses the game and opens with **Esc**. From there
> you can remap every button, manage quick saves, change the video filter,
> set the fast-forward speed and edit the touch controls.

| GBA | Keyboard |
| --- | --- |
| A / B | X / Z |
| L / R | A / S |
| Start / Select | Enter / Backspace |
| D-pad | arrow keys |
| Fast-forward | Tab |
| Quick save / load | F5 / F8 |

## 🔨 Building from source

### Android

You need JDK 17 and the Android SDK with NDK 27.2.12479018:

```sh
cd android
./gradlew assembleEuRelease   # or assembleUsRelease
```

### PC (Linux, Windows, macOS)

You need CMake, Ninja, a C compiler (clang or gcc) and Python 3. SDL3 is
downloaded automatically if it is not installed.

```sh
cmake -B build -G Ninja -DMF_REGION=eu    # or us
ninja -C build
./build/mf [path/to/rom.gba]
```

Without a path, the program looks for `mf_eu.gba` / `mf_us.gba` next to the
executable or opens a file picker. `-DMF_HOST=headless` builds `mf_headless`,
a windowless version for testing that dumps screenshots.

### Testing

[docs/PORT.md](docs/PORT.md) explains the `MF_WARP` mode. It jumps to any
room, cutscene or menu with all equipment, and that is how the whole game has
been tested without playing it.

### GBA ROM (decompilation)

The decompilation tree still builds the original US ROM, byte-for-byte
identical, with [agbcc](https://github.com/pret/agbcc) and
`binutils-arm-none-eabi`:

```sh
cp your_us_rom.gba mf_us_baserom.gba
python3 tools/extractor.py
make -j
sha1sum mf_us.gba   # ca33f4348c2c05dd330d37b97e2c5a69531dfe87
```

The port's changes are under `#ifdef PLATFORM_PC`, so they do not affect that
build. Full agbcc installation instructions are in the
[metroidret/mf README](https://github.com/metroidret/mf#readme).

## 🙏 Credits and licenses

- Decompilation: [metroidret/mf](https://github.com/metroidret/mf)
  ([Discord](https://discord.gg/2MGB9Xbr9y)), MIT ([LICENSE](LICENSE)).
- Port layer: GPL-3.0 ([port/LICENSE](port/LICENSE)).
- Software PPU: [Project Picori](https://github.com/999sian/tmc).
- Sister project: [metroid-zm-recomp](https://github.com/kalasyan/metroid-zm-recomp) (Metroid: Zero Mission).

Metroid Fusion is a trademark of Nintendo. This project is not affiliated with
Nintendo and does not distribute any game data.
