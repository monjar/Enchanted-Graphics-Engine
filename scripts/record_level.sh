#!/usr/bin/env bash
# Records the sandbox game being played and assembles docs/images/level.gif,
# plus a still beside it.
#
# What this shows is not the renderer and not the character: it is a *level*,
# and every rule in it comes from the sandbox module rather than from the
# engine. A coin is taken, a step is missed and a life with it, the player
# comes back at the start, the last coin opens the gate and the exit pad ends
# the level.
#
# Nothing is driving it by hand. The ScriptedRun behaviour walks the route by
# writing the same intent fields a player's hands would - and stands down the
# moment a human touches anything, which is why the same scene file is both
# this recording and the thing you play with --play.
#
# Needs a Vulkan device, ImageMagick, and - on a headless machine - Xvfb.
set -euo pipefail

BUILD="${BUILD:-build/default}"
FPS="${FPS:-8}"
# Long enough for the whole loop: a coin, a fall, the walk back, two more
# coins, the gate and the exit.
SECONDS_TO_RECORD="${SECONDS_TO_RECORD:-58}"
WINDOW_W="${WINDOW_W:-960}"
WINDOW_H="${WINDOW_H:-600}"
GIF_WIDTH="${GIF_WIDTH:-480}"
COLORS="${COLORS:-48}"
# Which frame becomes the still: the moment on the bridge, which is the one
# frame that has the whole level in it - where you came from, what you fall
# into, and where you are going.
STILL_FRAME_INDEX="${STILL_FRAME_INDEX:-165}"
OUT="${OUT:-docs/images/level.gif}"
STILL="${STILL:-docs/images/level.png}"

FRAMES="$(mktemp -d)"
trap 'rm -rf "$FRAMES"' EXIT

run() {
    if command -v xvfb-run >/dev/null && [ -z "${DISPLAY:-}" ]; then
        xvfb-run -a --server-args="-screen 0 $((WINDOW_W + 120))x$((WINDOW_H + 120))x24" "$@"
    else
        "$@"
    fi
}

run "${BUILD}/bin/EnchantedEngine" --scene assets/scenes/level.egescene --play \
    --size "$WINDOW_W" "$WINDOW_H" \
    --exit-after "$SECONDS_TO_RECORD" \
    --record "$FRAMES" --record-fps "$FPS"

mapfile -t frames < <(find "$FRAMES" -name 'frame_*.png' | sort)
if [ "${#frames[@]}" -eq 0 ]; then
    echo "no frames were recorded" >&2
    exit 1
fi

height=$((GIF_WIDTH * WINDOW_H / WINDOW_W))
# -dither None because the alternative scatters pixels that differ from their
# neighbours by a shade, which costs quality nothing anyone can see at this
# size and costs the frame differencing everything it had.
convert -delay "$(awk -v r="$FPS" 'BEGIN { printf "%d", 100 / r }')" -loop 0 \
    "${frames[@]}" -resize "${GIF_WIDTH}x${height}" \
    -colors "$COLORS" -dither None -layers OptimizeFrame "$OUT"

still_index="$STILL_FRAME_INDEX"
if [ "$still_index" -ge "${#frames[@]}" ]; then
    still_index=$((${#frames[@]} - 1))
fi
convert "${frames[$still_index]}" -resize "1024x$((1024 * WINDOW_H / WINDOW_W))" "$STILL"

echo "wrote $OUT from ${#frames[@]} frames ($(du -h "$OUT" | cut -f1))"
echo "wrote $STILL ($(du -h "$STILL" | cut -f1))"
