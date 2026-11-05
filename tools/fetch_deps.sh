#!/bin/bash
# The third-party sources xbased builds with, at the versions it's tested
# with, cloned into ../deps beside the repository (or the folder given).
#   tools/fetch_deps.sh [deps dir]
# Runs on Linux and in MSYS2 on Windows; needs git.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
DEPS=${1:-$REPO/../deps}
mkdir -p "$DEPS"

# name  url  commit  (subdirectory)
fetch() {
  local dir="$DEPS/$1" url=$2 rev=$3
  if [ -d "$dir/.git" ] && [ "$(git -C "$dir" rev-parse HEAD)" = "$rev" ]; then
    echo "$1: up to date"
    return
  fi
  echo "$1: $url @ ${rev:0:12}"
  rm -rf "$dir"
  mkdir -p "$dir"
  git -C "$dir" init -q
  git -C "$dir" remote add origin "$url"
  git -C "$dir" fetch -q --depth 1 origin "$rev"
  git -C "$dir" checkout -q FETCH_HEAD
}

fetch SDL3          https://github.com/libsdl-org/SDL.git                                    829a65d769d935c4852f8159e964312c0957260a
fetch zydis         https://github.com/zyantific/zydis.git                                   a2278f1d254e492f6a6b39f6cb5d1f5d515659dc
fetch libslirp      https://gitlab.freedesktop.org/slirp/libslirp.git                        26be815b86e8d49add8c9a8b320239b9594ff03d
fetch xxhash        https://github.com/Cyan4973/xxHash.git                                   e626a72bc2321cd320e953a0ccf1584cad60f363
fetch volk          https://github.com/zeux/volk.git                                         0b17a763ba5643e32da1b2152f8140461b3b7345
fetch vma           https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git    1d8f600fd424278486eade7ed3e877c99f0846b1
fetch glslang       https://github.com/KhronosGroup/glslang.git                              b5782e52ee2f7b3e40bb9c80d15b47016e008bc9
fetch spirv_reflect https://github.com/KhronosGroup/SPIRV-Reflect.git                        ef913b3ab3da1becca3cf46b15a10667c67bebe5
fetch Vulkan-Headers https://github.com/KhronosGroup/Vulkan-Headers.git                       9a0f3099c8a9607a7c0f3127d8abfdc19a93e8c5
fetch root/subprojects/nv2a_vsh_cpu https://github.com/xemu-project/nv2a_vsh_cpu.git         1115255708c10c4841b65dcd2223262e7a316598
fetch libchdr       https://github.com/rtissera/libchdr.git                                  47d794151b2b64f6aba60304e6480f4f5a2bc24b

# Zydis' own dependency
if [ ! -f "$DEPS/zydis/dependencies/zycore/CMakeLists.txt" ]; then
  rm -rf "$DEPS/zydis/dependencies/zycore"
  mkdir -p "$DEPS/zydis/dependencies/zycore"
  git -C "$DEPS/zydis/dependencies/zycore" init -q
  git -C "$DEPS/zydis/dependencies/zycore" remote add origin https://github.com/zyantific/zycore-c.git
  git -C "$DEPS/zydis/dependencies/zycore" fetch -q --depth 1 origin 0b2432ced0884fd152b471d97ecf0258ff4d859f
  git -C "$DEPS/zydis/dependencies/zycore" checkout -q FETCH_HEAD
fi
echo "deps ready in $DEPS"
