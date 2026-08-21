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
# Needs a Vulkan device, ImageMagick, and - on a headless machine - Xvfb.
set -euo pipefail

BUILD="${BUILD:-build/default}"
FPS="${FPS:-4}"
SECONDS_TO_RUN="${SECONDS_TO_RUN:-22}"
RELOAD_AT="${RELOAD_AT:-11}"
WINDOW_W="${WINDOW_W:-1280}"
WINDOW_H="${WINDOW_H:-800}"
GIF_WIDTH="${GIF_WIDTH:-800}"
COLORS="${COLORS:-96}"
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

# The edit and the rebuild, on a timer, while the engine runs.
(
    sleep "$RELOAD_AT"
    sed -i 's/exaggeration = 1.f/exaggeration = 3.f/' "$SOURCE"
    cmake --build "$BUILD" --target EnchantedSandbox >/dev/null 2>&1
) &
reloader=$!

run "${BUILD}/bin/EnchantedEngine" --demo --editor \
    --size "$WINDOW_W" "$WINDOW_H" \
    --exit-after "$SECONDS_TO_RUN" \
    --record "$FRAMES" --record-fps "$FPS"

wait "$reloader" 2>/dev/null || true

count=$(find "$FRAMES" -name 'frame_*.png' | wc -l)
if [ "$count" -eq 0 ]; then
    echo "no frames were recorded" >&2
    exit 1
fi

height=$((GIF_WIDTH * WINDOW_H / WINDOW_W))
convert -delay "$((100 / FPS))" -loop 0 "$FRAMES"/frame_*.png \
    -resize "${GIF_WIDTH}x${height}" -colors "$COLORS" -layers OptimizeTransparency "$OUT"

# A still from after the reload, for anywhere a moving picture is too much.
last=$(find "$FRAMES" -name 'frame_*.png' | sort | tail -3 | head -1)
convert "$last" -resize "${GIF_WIDTH}x${height}" "$STILL"

echo "wrote $OUT from ${count} frames ($(du -h "$OUT" | cut -f1))"
echo "wrote $STILL ($(du -h "$STILL" | cut -f1))"
