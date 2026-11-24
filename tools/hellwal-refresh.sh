#!/usr/bin/env bash
# hellwal-refresh.sh - recolor the focused HUD to the new palette and reload
# the running xib (SIGHUP).
#
# Usage: tools/hellwal-refresh.sh        # run after hellwal

set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$DIR"

source ~/.cache/hellwal/colors.sh

magick assets/hud-512-focused-white.png -fuzz 10% -fill "#${color6}" -opaque "#FFFFFF" assets/hud-512-focused.png
magick assets/hud-512-unfocused-black.png -fuzz 10% -fill "#${color6}" -opaque "#FFFFFF" assets/hud-512-unfocused.png

if pid=$(pgrep -x xib 2>/dev/null | head -1); then
    kill -HUP "$pid"
fi
