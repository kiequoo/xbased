<p align="center">
  <img src="docs/media/banner.jpg" alt="xbased" width="820">
</p>

<p align="center">
  <b>The original Xbox, on your PC.</b><br>
  xbased is an original Xbox emulator for Windows and Linux, developed by <b>crazyriddler</b>.<br>
  Free and open source, with a console-style dashboard. Based on <a href="https://github.com/rfandango/hakuX">hakuX</a>.
</p>

<p align="center">
  <a href="https://github.com/crazyriddler/xbased/releases/latest"><img src="https://img.shields.io/github/v/release/crazyriddler/xbased?style=for-the-badge&color=6fd12a&labelColor=0b1a06&label=download" alt="Download"></a>
  <img src="https://img.shields.io/badge/Windows-x64-6fd12a?style=for-the-badge&labelColor=0b1a06&logo=windows&logoColor=white" alt="Windows x64">
  <img src="https://img.shields.io/badge/Linux-AppImage-6fd12a?style=for-the-badge&labelColor=0b1a06&logo=linux&logoColor=white" alt="Linux AppImage">
  <img src="https://img.shields.io/badge/Vulkan-renderer-6fd12a?style=for-the-badge&labelColor=0b1a06&logo=vulkan&logoColor=white" alt="Vulkan">
  <a href="COPYING"><img src="https://img.shields.io/badge/license-GPL--3.0-6fd12a?style=for-the-badge&labelColor=0b1a06" alt="GPL-3.0"></a>
</p>

<p align="center">
  <a href="#download">Download</a> ·
  <a href="#features">Features</a> ·
  <a href="#screenshots">Screenshots</a> ·
  <a href="#getting-started">Getting started</a> ·
  <a href="#building-from-source">Building</a> ·
  <a href="#credits">Credits</a>
</p>

---

> **This is a fork of [crazyriddler/xbased](https://github.com/crazyriddler/xbased) that adds arm64 Linux support** for Linux handhelds (AYN Odin, etc. running a full Wayland/X11 desktop). All emulator credit goes to the upstream project.
>
> **What this fork adds:**
> - arm64 Linux build target (FEX x86→arm64 CPU backend + SDL3 desktop frontend)
> - A/B–X/Y glyph swap toggle — relabels on-screen button hints without changing button actions
> - Controller button icons shown immediately when a controller is detected
> - EmuDeck-style Select-modifier combos: Select+Start=quit, +R3=menu, +R1=save, +L1=load, +Y=screenshot, +L3=pause, +R2=fast-forward
>
> **[Download arm64 Linux AppImage](https://github.com/kiequoo/xbased-arm64/releases/latest)** · [Build from source](BUILDING.md)

---

<!-- trailer -->
https://github.com/user-attachments/assets/bf62adff-002a-4dc6-9b28-eee775181646

<p align="center"><sub><a href="https://youtu.be/0Kq3bW7XKxI">Watch on YouTube</a> · <a href="https://github.com/crazyriddler/xbased/releases/download/v1.0/xbased-trailer-1080p60.mp4">Download in 1080p60</a></sub></p>
<!-- /trailer -->

---

**xbased** is an original Xbox emulator for Windows and Linux, developed by
**[crazyriddler](https://github.com/crazyriddler)**. Based on
[hakuX](https://github.com/rfandango/hakuX).

It runs original Xbox games on a PC with a reimplemented Xbox kernel, a
high-level NV2A renderer on Vulkan and an accurate MCPX audio core, behind a
console-style front end of its own.
No BIOS, MCPX ROM or hard-disk image is needed.

xbased is created and maintained by **[crazyriddler](https://github.com/crazyriddler)**:
the emulator, the dashboard, the game fixes and every release.

## Download

| Platform | File | |
|---|---|---|
| **Windows** 10/11 (x64) | `xbased-1.0.21-windows-x64.zip` | Unzip anywhere and run `xbased.exe`. Portable: everything stays in its folder. |
| **Linux** (x86-64) | `xbased-1.0.21-x86_64.AppImage` | `chmod +x` it and run. Ubuntu 22.04 or newer, or any distribution of the same age. |

Get them from **[Releases](https://github.com/crazyriddler/xbased/releases/latest)**.

**Requirements:** a GPU with Vulkan 1.1 or newer (1.3 recommended), a CPU with
SSE4.1, and your own game images (ISO / XISO) dumped from discs you own.
xbased doesn't include any copyrighted Xbox software.

## Features

<table>
<tr>
<td width="50%" valign="top">

### Like a console
- **Console-style dashboard**: an animated green orb, drifting hexagons, 3D
  transitions and navigation sounds.
- **Gamepad, keyboard or mouse** everywhere; button prompts follow the device
  you're using.
- **Borderless full screen by default**, any resolution, ultrawide or portrait,
  any DPI.
- **Exit from the gamepad**, with a confirmation.
- **Automatic updates**: a new release is offered as xbased starts and
  installs itself (Settings › Interface turns the check off and on).

</td>
<td width="50%" valign="top">

### Your library
- **Automatic box art** and the discs' own title icons.
- **A page per game**: play, load a state, settings, patches, controls.
- **Memory manager**: your games' saves with their icons; export, import and
  delete.
- **Import from xemu**: xemu's saves copied in and re-signed so games accept
  them (Memory, Y).
- **Broad compatibility** with many games, inherited from hakuX.

</td>
</tr>
<tr>
<td valign="top">

### Picture
- **Internal resolution** up to 8x, aspect ratio, widescreen and video modes.
- **FXAA** and **anisotropic filtering** up to 16x.
- **Upscaling**: bilinear or AMD FSR 1.
- **CRT filters**: soft scanlines, a classic TV, an arcade monitor.
- Every setting **global or per game**.

</td>
<td valign="top">

### Patches & mods
- **Per-game patches**: 60/120 fps, widescreen, fixes and cheats, toggled per
  game.
- **Frame-rate unlock per game**, where the game allows it.
- **Add your own**: drop the IPS patches shared online into
  `mods/<TITLE ID>/` and they show up in the game's menu
  ([MODS.md](mods/README.md)).

</td>
</tr>
<tr>
<td valign="top">

### Save states & in-game menu
- **10 save-state slots per game**, with pictures.
- **In-game menu** with F1, or **Back + Start** / the **Guide** button on a
  gamepad: states, settings, controls, restart, back to the dashboard.
- **Shortcuts**: F5 quick save, F8 quick load, Tab fast forward, F2 pause,
  F12 screenshot, F11 full screen; all configurable.

</td>
<td valign="top">

### Controls, online, languages
- **4 controller ports**: Automatic, any gamepad or the keyboard; remap every
  button, per game too; dead zone and vibration.
- **Sticks like the Xbox controller's**: a modern stick's circle reaches the
  Duke's full square, so diagonals register. Stick range and shape are set
  on a screen that shows both sticks live, the range games expect and the
  range yours reaches.
- **PAL games at 60 Hz**, as with the console's PAL-60 setting on; 50 Hz is
  an option, for all games or one.
- **Stereo or 5.1 surround**: the games' Dolby Digital mix, six channels
  out (Settings › Audio; stereo by default).
- **Online**: Insignia (the community's Xbox Live replacement), XLink Kai and
  System Link on your network.
- **11 languages**, your system's by default. Games start in your language,
  or in English when the disc doesn't have it.

</td>
</tr>
</table>

## Screenshots

<table>
<tr>
<td width="50%"><img src="docs/media/dashboard.jpg" alt="Dashboard"><p align="center"><sub>The dashboard</sub></p></td>
<td width="50%"><img src="docs/media/library.jpg" alt="Library"><p align="center"><sub>Your library, with box art</sub></p></td>
</tr>
<tr>
<td><img src="docs/media/game.jpg" alt="Game page"><p align="center"><sub>A game's page</sub></p></td>
<td><img src="docs/media/game-settings.jpg" alt="Per-game settings"><p align="center"><sub>Per-game settings</sub></p></td>
</tr>
<tr>
<td><img src="docs/media/patches.jpg" alt="Patches and mods"><p align="center"><sub>Patches & mods</sub></p></td>
<td><img src="docs/media/states.jpg" alt="Save states"><p align="center"><sub>Save states with pictures</sub></p></td>
</tr>
<tr>
<td><img src="docs/media/ingame-menu.jpg" alt="In-game menu"><p align="center"><sub>The in-game menu</sub></p></td>
<td><img src="docs/media/crt.jpg" alt="CRT filter"><p align="center"><sub>A CRT filter</sub></p></td>
</tr>
<tr>
<td><img src="docs/media/controls.jpg" alt="Controller ports"><p align="center"><sub>4 controller ports</sub></p></td>
<td><img src="docs/media/shortcuts.jpg" alt="Shortcuts"><p align="center"><sub>Keyboard shortcuts</sub></p></td>
</tr>
<tr>
<td><img src="docs/media/memory.jpg" alt="Memory manager"><p align="center"><sub>Memory manager</sub></p></td>
<td><img src="docs/media/online.jpg" alt="Online"><p align="center"><sub>Online: Insignia, XLink Kai, System Link</sub></p></td>
</tr>
</table>

<p align="center"><img src="docs/media/ultrawide.jpg" alt="Ultrawide" width="100%"><br><sub>Any screen: the dashboard at 21:9</sub></p>

## Getting started

1. Download xbased from [Releases](https://github.com/crazyriddler/xbased/releases/latest):
   unzip it anywhere (Windows) or run the AppImage (Linux).
2. Start it: the dashboard opens. **Settings › Interface › Add folder** and pick
   the folder with your disc images (or **Add a game file** for a single
   image).
3. Choose a game in **Games**. Box art downloads automatically (it can be
   turned off).

Command line: `xbased <image.iso> [options]` starts a game directly; options
are the same as the settings files' lines (`scale=2`, `uncap`, `crt=2`,
`fullscreen=0`, `mod=<patch>`...). Settings are kept in `xbased.ini` (global)
and `games/<TITLE ID>.<version>.ini` (per game) beside the executable on
Windows, in `~/.local/share/xbased/` on Linux.

### Default keyboard controls

| Xbox | Key | Xbox | Key |
|---|---|---|---|
| A / B / X / Y | Space / Left Shift / Q / E | Left stick | W A S D |
| Black / White | C / Z | Right stick | I J K L |
| Left / right trigger | 1 / 3 | D-pad | Arrow keys |
| Start / Back | Enter / Backspace | Stick clicks | F / H |

**Shortcuts:** F1 menu, F5 quick save, F8 quick load (Shift: slot 2), Tab fast
forward (hold), F2 pause, F12 screenshot, F11 full screen. On a gamepad,
Back + Start or the Guide button opens the menu.

## Building from source

xbased builds with clang and CMake. The third-party sources are fetched at
pinned versions by `tools/fetch_deps.sh` into a `deps` folder beside the
repository:

```
<work>/
  xbased/      this repository
  deps/        third-party sources (tools/fetch_deps.sh)
  build/       build trees
```

The executable is built from the `hakux` target and named `xbased`.

### Windows (x64)

1. Install [MSYS2](https://www.msys2.org/) and open the **MSYS2 CLANG64** shell.
2. Install the toolchain:

   ```bash
   pacman -S --needed git mingw-w64-clang-x86_64-clang mingw-w64-clang-x86_64-lld \
     mingw-w64-clang-x86_64-cmake mingw-w64-clang-x86_64-ninja mingw-w64-clang-x86_64-python
   ```

3. Get the source, fetch the dependencies and build:

   ```bash
   git clone https://github.com/crazyriddler/xbased
   cd xbased
   tools/fetch_deps.sh
   cmake -S native -B ../build/win -G Ninja -DCMAKE_BUILD_TYPE=Release \
     -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
   cmake --build ../build/win --target hakux
   ```

`../build/win/xbased.exe` runs from the build folder (its fonts and mods are
copied beside it). It imports only system DLLs, so it runs on any Windows
10/11 x64 with a Vulkan driver. To make the release zip in `../dist`
(PowerShell): `tools\release\package_windows.ps1`

### Linux (x86-64)

1. Install the packages (Ubuntu/Debian; on other distributions, the same
   libraries' development packages):

   ```bash
   sudo apt install git clang lld cmake ninja-build pkg-config python3 zlib1g-dev \
     libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxtst-dev libxfixes-dev \
     libxkbcommon-dev libwayland-dev wayland-protocols libegl-dev libgl-dev libdecor-0-dev \
     libdrm-dev libgbm-dev libasound2-dev libpulse-dev libpipewire-0.3-dev libudev-dev \
     libdbus-1-dev libibus-1.0-dev
   ```

2. Get the source, fetch the dependencies and build:

   ```bash
   git clone https://github.com/crazyriddler/xbased
   cd xbased
   tools/fetch_deps.sh
   cmake -S native -B ../build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
     -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
   cmake --build ../build/linux --target hakux
   ```

`../build/linux/xbased` runs from the build folder. Settings, saves and states
go to `~/.local/share/xbased` (or beside the executable when a `portable.txt`
is there). SDL3 is linked statically; at run time the system needs glibc 2.35
or newer and a Vulkan driver (Mesa or the GPU vendor's). To make the AppImage
in `../dist`: `tools/release/package_linux.sh ../build/linux` (the release
one is built on Ubuntu 22.04, so it runs there and on newer distributions).

More options (link-time optimization, the dependencies' location) are in
[BUILDING.md](BUILDING.md).

## Credits

- **xbased** by **[crazyriddler](https://github.com/crazyriddler)**, its creator
  and maintainer: the emulator for x86-64 Windows and Linux, the
  console-style dashboard and front end, the game fixes (Fable, The Thing,
  BloodRayne 2, NFL Fever 2004, Panzer Dragoon Orta and more), internal
  resolution up to 8x, 5.1 audio, the automatic updates and every release
  ([CHANGELOG.md](CHANGELOG.md)).
- Based on **[hakuX](https://github.com/rfandango/hakuX)**. Its README and
  changelog are in [docs/hakuX](docs/hakuX/).
- Box art from the [libretro-thumbnails](https://github.com/libretro-thumbnails)
  project. Interface font: Play (SIL Open Font License).
- SDL3, Zydis, libslirp, xxHash, volk, Vulkan Memory Allocator, glslang,
  SPIRV-Reflect and the other components listed in [LICENSE](LICENSE).

## License

xbased, like hakuX, is free software under the GNU General Public License,
version 3 or later ([COPYING](COPYING)). The third-party components keep their
own licenses ([LICENSE](LICENSE), [licenses/](licenses/)).

<sub>xbased is not affiliated with or endorsed by Microsoft. Xbox is a trademark
of Microsoft Corporation. Game names and box art shown belong to their owners.</sub>
