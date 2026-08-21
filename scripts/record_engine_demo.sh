#!/usr/bin/env bash
# Records the engine being used, and assembles docs/images/engine-demo.gif.
#
# Different from record_demo.sh, which records the camera tour with the editor
# hidden - that shows what the renderer produces. This one leaves the editor
# up, so what is recorded is the hierarchy, the inspector, the stats and the
# console alongside the scene, and then rebuilds a script module underneath
# the running engine so the recording contains a hot reload actually
# happening.
#
# The edit it makes is to a constant rather than to a reflected field, and
# that is the point: a field's value is carried across a reload from the
# instance being replaced, so changing a default would prove nothing. Changing
# code is what a reload has to demonstrate, and the sphere's breathing
# visibly deepens when the new module lands.
#
# The source file is restored on the way out, whatever happens.
#
# On the frame rate. Recording pins the simulation to a fixed step of 1/FPS,
# so the tour comes out identical on a fast machine and a slow one - which
# means FPS is how smooth the result looks and the frame count is how much of
# the tour it covers. A GIF of a 1280x800 editor costs roughly 45 kB a frame
# after quantisation however hard it is squeezed, because the viewport is
# moving and frame differencing has little to hold on to, so the two trade
# directly against file size. 12 fps for nine seconds is the compromise:
# smooth enough to read as running software, small enough to sit in a README.
# The still is there for anyone who wants to read the panels.
#
# Which nine seconds is not a free choice either. The tour's pose is a pure
# function of elapsed time, so reaching its last third means recording
# everything before it and throwing most of it away - hence recording one
# number of frames and keeping another. The window is picked so the pulsing
# sphere is on screen and large across it, because the sphere is where the
# reload becomes visible, and so the last frame is the wide shot of what
# physics left behind, because that frame is also the still.
#
# Needs a Vulkan device, ImageMagick, and - on a headless machine - Xvfb.
set -euo pipefail

BUILD="${BUILD:-build/default}"
FPS="${FPS:-12}"
FRAMES_TO_RECORD="${FRAMES_TO_RECORD:-276}"
# The window kept for the GIF: 14.0s to 23.0s of a 24.1s tour.
GIF_FIRST_FRAME="${GIF_FIRST_FRAME:-168}"
GIF_FRAMES="${GIF_FRAMES:-108}"
# Which frame the rebuild is kicked off at, so the reload always lands in the
# same part of the recording. Counting frames rather than sleeping for a wall
# time, because how long a frame takes depends entirely on the machine - the
# software rasteriser in CI is two orders of magnitude off a real GPU, and a
# sleep tuned for one puts the reload off the end of the other. Early in the
# window rather than in the middle of it, because the rebuild itself takes a
# few seconds of wall clock and the recording keeps going through them.
RELOAD_AFTER_FRAMES="${RELOAD_AFTER_FRAMES:-200}"
# Which frame of the kept window becomes the still: far enough past the
# reload to be showing its result, and while the sphere showing it is still
# in shot, which the window's own last frame is not.
STILL_FRAME_INDEX="${STILL_FRAME_INDEX:-60}"
WINDOW_W="${WINDOW_W:-1280}"
WINDOW_H="${WINDOW_H:-800}"
GIF_WIDTH="${GIF_WIDTH:-720}"
COLORS="${COLORS:-64}"
OUT="${OUT:-docs/images/engine-demo.gif}"
STILL="${STILL:-docs/images/editor.png}"

SOURCE="sandbox/SandboxBehaviors.cpp"
BACKUP="$(mktemp)"
FRAMES="$(mktemp -d)"
cp "$SOURCE" "$BACKUP"

restore() {
    cp "$BACKUP" "$SOURCE"
    rm -f "$BACKUP"
    rm -rf "$FRAMES"
    # Leave the built module matching the source in the tree, so the next run
    # of anything is not quietly running the demo's edit.
    cmake --build "$BUILD" --target EnchantedSandbox >/dev/null 2>&1 || true
}
trap restore EXIT

run() {
    if command -v xvfb-run >/dev/null && [ -z "${DISPLAY:-}" ]; then
        xvfb-run -a --server-args="-screen 0 $((WINDOW_W + 120))x$((WINDOW_H + 120))x24" "$@"
    else
        "$@"
    fi
}

# The edit and the rebuild, once the recording has got far enough in.
(
    while [ "$(find "$FRAMES" -name 'frame_*.png' | wc -l)" -lt "$RELOAD_AFTER_FRAMES" ]; do
        sleep 0.2
    done
    sed -i 's/exaggeration = 1.f/exaggeration = 3.f/' "$SOURCE"
    cmake --build "$BUILD" --target EnchantedSandbox >/dev/null 2>&1
) &
reloader=$!

# --exit-after counts the simulated seconds the fixed step adds up to, so the
# frame count is what is actually being asked for here.
seconds=$(awk -v f="$FRAMES_TO_RECORD" -v r="$FPS" 'BEGIN { printf "%.3f", f / r }')
run "${BUILD}/bin/EnchantedEngine" --demo --editor \
    --size "$WINDOW_W" "$WINDOW_H" \
    --exit-after "$seconds" \
    --record "$FRAMES" --record-fps "$FPS"

wait "$reloader" 2>/dev/null || true

count=$(find "$FRAMES" -name 'frame_*.png' | wc -l)
if [ "$count" -eq 0 ]; then
    echo "no frames were recorded" >&2
    exit 1
fi

mapfile -t kept < <(find "$FRAMES" -name 'frame_*.png' | sort |
    tail -n "+$((GIF_FIRST_FRAME + 1))" | head -n "$GIF_FRAMES")
if [ "${#kept[@]}" -eq 0 ]; then
    echo "the kept window starts past the end of the recording" >&2
    exit 1
fi

height=$((GIF_WIDTH * WINDOW_H / WINDOW_W))
# -dither None because the alternative scatters pixels that differ from their
# neighbours by a shade, which costs quality nothing anyone can see at this
# size and costs the frame differencing below everything it had.
convert -delay "$(awk -v r="$FPS" 'BEGIN { printf "%d", 100 / r }')" -loop 0 \
    "${kept[@]}" -resize "${GIF_WIDTH}x${height}" \
    -colors "$COLORS" -dither None -layers OptimizeFrame "$OUT"

# A frame out of the GIF's own window as the still, at a width the panels are
# readable at - so the two pictures are the same recording rather than two
# near misses.
still_index="$STILL_FRAME_INDEX"
if [ "$still_index" -ge "${#kept[@]}" ]; then
    still_index=$((${#kept[@]} - 1))
fi
convert "${kept[$still_index]}" -resize "1024x$((1024 * WINDOW_H / WINDOW_W))" "$STILL"

echo "wrote $OUT from ${#kept[@]} of ${count} frames ($(du -h "$OUT" | cut -f1))"
echo "wrote $STILL ($(du -h "$STILL" | cut -f1))"
