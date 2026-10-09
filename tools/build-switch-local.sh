#!/bin/sh
# Builds build/switch/sh3-nx.nro with a local devkitPro install (Windows msys2 or Linux), no container.
# Needs: devkitA64, libnx, switch-sdl2, switch-mesa (dkp-pacman).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
: "${DEVKITPRO:=/opt/devkitpro}"
[ -d "$DEVKITPRO" ] || DEVKITPRO=/c/devkitPro
export DEVKITPRO
export PATH="$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:$DEVKITPRO/portlibs/switch/bin:$PATH"
cd "$ROOT"
cmake -S . -B build/switch -G Ninja -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build/switch -j "${JOBS:-$(nproc 2>/dev/null || echo 8)}"
ls -la build/switch/sh3-nx.nro
