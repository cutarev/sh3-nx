#!/bin/sh
# Waits for the Switch's netloader (hbmenu, Y) and sends build/switch/sh3-nx.nro with nxlink, keeping
# the stdio server up so the game's log streams back.  Usage: tools/nxlink-wait.sh <switch ip> [log]
IP=${1:?usage: tools/nxlink-wait.sh <switch ip> [log]}
LOG=${2:-build/switch/nxlink.log}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
for i in $(seq 1 400); do
  if podman run --rm --network host -v "$ROOT/build/switch:/b:Z" docker.io/devkitpro/devkita64:latest \
       /opt/devkitpro/tools/bin/nxlink -s -a "$IP" /b/sh3-nx.nro > "$LOG" 2>&1; then
    grep -q "failed" "$LOG" || exit 0
  fi
  sleep 3
done
