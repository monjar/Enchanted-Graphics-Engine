#!/usr/bin/env bash
# Runs a command for N seconds, then stops it and reports success.
#
# The engine's main loop only exits when its window is closed, which cannot
# happen unattended, so CI needs to let it render for a while and then take the
# absence of a crash as the result.
set -euo pipefail

seconds="$1"; shift

"$@" &
pid=$!

for _ in $(seq "$seconds"); do
    sleep 1
    if ! kill -0 "$pid" 2>/dev/null; then
        wait "$pid" && status=0 || status=$?
        echo "Process exited early with status ${status}" >&2
        exit "${status}"
    fi
done

kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true
echo "Ran for ${seconds}s without crashing."
