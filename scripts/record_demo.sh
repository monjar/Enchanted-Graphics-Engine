#!/usr/bin/env bash
# Records the demo tour and assembles docs/images/demo-tour.gif.
#
# The engine writes every frame itself rather than being screen-grabbed, so
# the result is the exact pixels the GPU produced and is the same however fast
# the machine renders it - the tour advances by a fixed step per frame while
# recording. That is what makes this reproducible rather than a thing someone
# did once.
#
# Needs a Vulkan device, ImageMagick, and - on a headless machine - Xvfb.
set -euo pipefail

BUILD="${BUILD:-build/default}"
FPS="${FPS:-10}"
WIDTH="${WIDTH:-400}"
COLORS="${COLORS:-64}"
OUT="${OUT:-docs/images/demo-tour.gif}"
FRAMES="$(mktemp -d)"
trap 'rm -rf "$FRAMES"' EXIT

run() {
    if command -v xvfb-run >/dev/null && [ -z "${DISPLAY:-}" ]; then
        xvfb-run -a --server-args="-screen 0 800x600x24" "$@"
    else
        "$@"
    fi
}

run "${BUILD}/bin/EnchantedEngine" --demo --record "$FRAMES" --record-fps "$FPS"

count=$(find "$FRAMES" -name 'frame_*.png' | wc -l)
if [ "$count" -eq 0 ]; then
    echo "no frames were recorded" >&2
    exit 1
fi

height=$((WIDTH * 3 / 4))
convert -delay "$((100 / FPS))" -loop 0 "$FRAMES"/frame_*.png \
    -resize "${WIDTH}x${height}" -colors "$COLORS" -layers OptimizeTransparency "$OUT"

echo "wrote $OUT from ${count} frames ($(du -h "$OUT" | cut -f1))"
