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

**Always use `git merge` — never squash, rebase, or `cherry-pick --reset-author`.** Each upstream commit keeps its original author, date, and message; only the merge commit itself is ours.

Before merging, preview what will conflict:
```bash
git fetch upstream --no-tags
git merge-tree --write-tree main upstream/main
```

Then merge and push:
```bash
git merge --no-ff upstream/main
# resolve any conflicts (see below)
git push origin main          # direct push — no PR for upstream syncs
```

After testing on the device, retag if the upstream version bumped:
```bash
git tag v<version>
git push origin v<version>    # triggers CI and GitHub release
```

### Recurring conflict spots

**`native/app/ui/dash.c` — `draw_button_glyph`**
Our `ui.swapab` A/B–X/Y face-button swap (`which ^ 1`, drawing `k_col[gi]` / `k_pad[gi]`) conflicts with upstream changes to the glyph draw logic. **Always combine both sides, never choose one:**
- Keep upstream's `which >= 4` wide-key branch for triggers/View button unchanged.
- In the `else if (D.in.pad)` face-button branch, apply our swap: `const int gi = cfg_bool(&D.global, "ui.swapab", false) ? (which ^ 1) : which;` and use `k_col[gi]` / `k_pad[gi]`.
- Keep the `static const char *const k_key[4]` declaration (upstream sometimes drops it; it's needed for the keyboard branch below).

**`tools/release/package_linux.sh`**
Keep our `ARCH=${ARCH:-$(uname -m)}` line and the `aarch64` references throughout; take upstream's `VERSION` default value. Upstream periodically hardens this to `x86_64` only — revert those changes.

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

**arm64 cross-compile** — run this **before every push**, including upstream syncs.
The x86-64 build does not exercise the arm64 cmake wiring, so arm64-only breakage
(e.g. a new upstream source that needs a dep only wired for x86-64) will silently
pass x86-64 but fail CI. Pattern to watch: new files added to `app_win.cmake` may
pull in headers that need a matching `add_subdirectory` in `linux_arm64.cmake`.
```bash
tools/build_local.sh          # full build → build-arm64/xbased
tools/build_local.sh build    # skip dep fetch
tools/build_local.sh clean    # wipe build-arm64/
```
Requires: `sudo pacman -S aarch64-linux-gnu-gcc aarch64-linux-gnu-glibc aarch64-linux-gnu-linux-api-headers`

## Test device

AYN Odin 3 running Armada OS: `ssh armada@192.168.68.30`
AppImage at `~/Applications/xbased.AppImage`

## Commit style

All commits must use conventional prefixes: `feat:`, `fix:`, `ci:`, `docs:`, `refactor:`, `chore:`.
