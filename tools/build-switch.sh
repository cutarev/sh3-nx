#!/bin/sh
# Builds build/switch/sh3-nx.nro inside the devkitPro container (podman, no local install needed).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
podman run --rm -v "$ROOT:/src:Z" -w /src docker.io/devkitpro/devkita64:latest sh -c '
  cmake -S . -B build/switch -G Ninja -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake \
        -DCMAKE_BUILD_TYPE=Release >/dev/null &&
  cmake --build build/switch -j "$(nproc)"'
ls -la "$ROOT/build/switch/sh3-nx.nro"
