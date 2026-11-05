# CLAUDE.md

## What this project is

**xbased-arm64** is a fork of [crazyriddler/xbased](https://github.com/crazyriddler/xbased) that adds an arm64 Linux build target for Linux handhelds (AYN Odin 3, etc. running Armada OS or similar full desktop Linux). The local clone is at `~/dev/xbased-arm64`.

## What this fork adds over upstream

- arm64 Linux build target (FEX x86→arm64 CPU backend + SDL3 desktop frontend)
- SIGSEGV/SIGBUS fault handler for arm64 Linux (`native/core/fault_linux_arm64.c`)
- Cross-compile toolchain for local arm64 builds on x86_64 (`tools/build_local.sh`, `native/cmake/toolchain-cross-aarch64.cmake`)
- CI: arm64-only AppImage, caching, short SHA as version on PRs/branches, GitHub release on version tags
- A/B–X/Y glyph swap toggle (Settings > Interface; relabels hints, does not change button actions)
- Controller button icons shown immediately when a controller is connected
- EmuDeck-style Select-modifier combos: Select+Start=quit, +R3=menu, +R1=save, +L1=load, +Y=screenshot, +L3=pause, +R2=fast-forward

## Keeping up with upstream

Upstream is `crazyriddler/xbased` (remote: `upstream`).

```bash
git fetch upstream --no-tags
git merge upstream/main
git push origin main          # direct push — no PR for upstream syncs
```

Resolve any conflicts (recurring one: `tools/release/package_linux.sh` — keep our `ARCH=${ARCH:-$(uname -m)}` line and take upstream's new `VERSION`), then push directly to `main`. No PR needed for upstream syncs.

After syncing, retag if the upstream version bumped:

```bash
git tag v<version>
git push origin v<version>    # triggers CI and GitHub release
```

## PR workflow

**Own changes only** use PRs:

```bash
git checkout -b feat/my-change main
# … make changes …
git push origin feat/my-change
gh pr create --repo kiequoo/xbased-arm64
# wait for CI to pass, then merge
```

Never push own feature/fix work directly to `main`.

## Building

One-time:
```bash
git submodule update --init --recursive   # FEX submodule
tools/fetch_deps.sh ../deps
```

**x86_64 local build** (fastest dev loop — catches shared code issues without the device):
```bash
cmake -S native -B ../build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build ../build/linux --target hakux
```

**arm64 cross-compile** (catches arm64 build errors without CI):
```bash
tools/build_local.sh          # full build → build-arm64/xbased
tools/build_local.sh build    # skip dep fetch
tools/build_local.sh clean    # wipe build-arm64/
```
Requires: `sudo pacman -S aarch64-linux-gnu-gcc aarch64-linux-gnu-glibc aarch64-linux-gnu-linux-api-headers`

## Test device

AYN Odin 3 running Armada OS: `ssh armada@192.168.68.36`
AppImage at `~/Applications/xbased.AppImage`

## Commit style

All commits must use conventional prefixes: `feat:`, `fix:`, `ci:`, `docs:`, `refactor:`, `chore:`.
