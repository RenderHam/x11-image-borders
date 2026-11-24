#!/usr/bin/env bash
# refresh-hud.sh - send SIGHUP to a running xib when the HUD assets changed.
#
# Usage: tools/refresh-hud.sh [assets-dir]
# The assets (assets/*.png) are hashed; on a change, SIGHUP the running daemon
# (it reloads the PNGs without restarting). First run just records the hash.

set -u

DIR="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
cd "$DIR"

STAMP="${XDG_STATE_HOME:-$HOME/.local/state}/xib/assets.md5"
mkdir -p "$(dirname "$STAMP")"

hash=$(md5sum assets/*.png 2>/dev/null | awk '{print $1}' | sort | md5sum | awk '{print $1}')

if [ "$hash" = "$(cat "$STAMP" 2>/dev/null)" ]; then
    exit 0
fi

pid=$(pgrep -x xib 2>/dev/null | head -1)
if [ -n "$pid" ]; then
    kill -HUP "$pid"
    echo "refreshed xib (pid $pid)"
fi
echo "$hash" > "$STAMP"
