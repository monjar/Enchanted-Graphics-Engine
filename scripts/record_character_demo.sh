#!/usr/bin/env bash
# Records the character being played and assembles docs/images/character.gif,
# plus a still beside it.
#
# Different from record_demo.sh, which flies the camera round the scene on
# rails to show what the renderer produces. This one puts the camera behind
# the character and leaves it there, because what it is showing is not a
# picture - it is a rigged body walking on physics geometry, turning to face
# where it is going, jumping, and shouldering a crate out of the way.
#
# Nothing is driving it by hand: the character is walked by the Patrol
# behaviour, which writes the same four intent fields a player's hands would.
# That is why this recording is reproducible rather than a thing someone did
# once with a controller - and it is the same reason the demo tour is.
#
# Needs a Vulkan device, ImageMagick, and - on a headless machine - Xvfb.
set -euo pipefail

BUILD="${BUILD:-build/default}"
FPS="${FPS:-12}"
SECONDS_TO_RECORD="${SECONDS_TO_RECORD:-14}"
WINDOW_W="${WINDOW_W:-960}"
WINDOW_H="${WINDOW_H:-600}"
GIF_WIDTH="${GIF_WIDTH:-560}"
COLORS="${COLORS:-64}"
# Which frame becomes the still. Far enough in that the camera has settled
# behind the character and the walk cycle is running.
STILL_FRAME_INDEX="${STILL_FRAME_INDEX:-30}"
OUT="${OUT:-docs/images/character.gif}"
STILL="${STILL:-docs/images/character.png}"

FRAMES="$(mktemp -d)"
trap 'rm -rf "$FRAMES"' EXIT

run() {
    if command -v xvfb-run >/dev/null && [ -z "${DISPLAY:-}" ]; then
        xvfb-run -a --server-args="-screen 0 $((WINDOW_W + 120))x$((WINDOW_H + 120))x24" "$@"
    else
        "$@"
    fi
}

run "${BUILD}/bin/EnchantedEngine" --demo --follow \
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
