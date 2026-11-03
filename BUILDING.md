# Building xbased

xbased builds with clang and CMake on Windows (MSYS2) and Linux. The third-party
sources are fetched at pinned versions by `tools/fetch_deps.sh` into a `deps`
folder beside the repository:

```
<work>/
  xbased/      this repository
  deps/        third-party sources (tools/fetch_deps.sh)
  build/       build trees
```

The executable is built from the `hakux` target and named `xbased`.

## Windows (x64)

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

`../build/win/xbased.exe` runs from the build folder (its fonts and mods are copied
beside it). It imports only system DLLs, so it runs on any Windows 10/11 x64 with a
Vulkan 1.1 driver.

To make the release folder and zip in `../dist` (PowerShell):

```powershell
tools\release\package_windows.ps1
```

## Linux (x86-64)

Ubuntu/Debian packages (other distributions: the same libraries' development packages):

```bash
sudo apt install git clang lld cmake ninja-build pkg-config python3 zlib1g-dev \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxtst-dev libxfixes-dev \
  libxkbcommon-dev libwayland-dev wayland-protocols libegl-dev libgl-dev libdecor-0-dev \
  libdrm-dev libgbm-dev libasound2-dev libpulse-dev libpipewire-0.3-dev libudev-dev \
  libdbus-1-dev libibus-1.0-dev
```

Then:

```bash
git clone https://github.com/crazyriddler/xbased
cd xbased
tools/fetch_deps.sh
cmake -S native -B ../build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build ../build/linux --target hakux
```

`../build/linux/xbased` runs from the build folder. Settings, saves and states go to
`~/.local/share/xbased` (or beside the executable when a `portable.txt` is there).
SDL3 is linked statically; at run time the system needs glibc 2.35 or newer and a
Vulkan driver (Mesa or the GPU vendor's).

To make the AppImage in `../dist`:

```bash
tools/release/package_linux.sh ../build/linux
```

The release AppImage is built on Ubuntu 22.04, so it runs on that and newer
distributions.

## Linux (arm64)

Tested on devices running a full Linux desktop (Wayland/X11, glibc 2.35+, Mesa
Vulkan). On Adreno hardware the system Vulkan driver must support Vulkan 1.1.

Ubuntu/Debian packages (same set as Linux x86-64):

```bash
sudo apt install git clang lld cmake ninja-build pkg-config python3 zlib1g-dev \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxtst-dev libxfixes-dev \
  libxkbcommon-dev libwayland-dev wayland-protocols libegl-dev libgl-dev libdecor-0-dev \
  libdrm-dev libgbm-dev libasound2-dev libpulse-dev libpipewire-0.3-dev libudev-dev \
  libdbus-1-dev libibus-1.0-dev
```

Then:

```bash
git clone https://github.com/crazyriddler/xbased
cd xbased
# Initialise the FEX fork (the arm64 CPU backend) and its required submodules:
git submodule update --init external/fex
git -C external/fex submodule update --init \
  External/fmt External/range-v3 External/rpmalloc \
  External/unordered_dense External/xxhash Source/Common/cpp-optparse
# Fetch the remaining third-party sources (SDL3, Vulkan libs, etc.):
tools/fetch_deps.sh
cmake -S native -B ../build/linux-arm64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build ../build/linux-arm64 --target hakux
```

Pass `-DHAKUX_ARCH=armv8.2-a+fp16+dotprod+rcpc` (or a higher `-march` your
device supports) to enable CPU-specific code generation in FEX and the core.
Snapdragon 8 Elite (Oryon, ARMv9.2-a) can take `-DHAKUX_ARCH=armv9-a`.

`../build/linux-arm64/xbased` runs from the build folder.  Settings, saves and
states go to `~/.local/share/xbased` (or beside the executable when a
`portable.txt` is there).

## Options

| CMake option | Default | Meaning |
| --- | --- | --- |
| `NC_DEPS` | `../deps` | where the third-party sources are |
| `HAKUX_LTO` | `OFF` | link-time optimization |
