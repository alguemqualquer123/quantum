#!/bin/bash
# (c) SR VINIX
# SPDX-License-Identifier: MIT
#
# Scale curve: runs the probe in tests/scale/probe at every N given on the command line and prints
# one line of numbers per point, so a before/after comparison is one command.
#
#   tests/scale/run_curve.sh 200 500 1000 2000
#
# --stats-out must be absolute: the runner chdirs into the project directory before the engine
# starts, so a relative path lands inside the fixture and the write fails.
set -u
cd "$(dirname "$0")/../.." || exit 1
BUILD="${QUANTUM_BUILD_DIR:-build}"
# Release for honest numbers, Debug to match what the editor itself pays.
CONFIG="${SCALE_CONFIG:-Release}"
OUT="${SCALE_OUT_DIR:-/tmp/quantum-scale}"
mkdir -p "$OUT" || exit 1
EXE="$BUILD/$CONFIG/quantum-engine.exe"
if [ ! -f "$EXE" ]; then
  echo "no such executable: $EXE" >&2
  exit 1
fi

if [ "$#" -eq 0 ]; then
  set -- 200 500 1000
fi

for N in "$@"; do
  echo "=== N=$N ==="
  SCALE_N="$N" SCALE_WARMUP="${SCALE_WARMUP:-20}" SCALE_SAMPLES="${SCALE_SAMPLES:-50}" \
    SCALE_KIND="${SCALE_KIND:-model}" SCALE_SPACING="${SCALE_SPACING:-6}" \
    "$EXE" --project tests/scale/probe --window 1280x720 \
      --frames 110 --warmup 30 --name "n${N}" --stats-out "$(cygpath -w "$OUT")/n${N}.json" \
      > "$OUT/n${N}.log" 2>&1
  echo "exit=$?"
  grep -E "\[scale\]" "$OUT/n${N}.log"
  grep -E '"wallFps"|"avgMs"|"avgDrawCalls"|"avgInstances"|"drawSceneMs"|"updateMs"|"fixedUpdateMs"|"outsideLoopMs"|"meshLoadsPerFrame"' \
      "$OUT/n${N}.json" 2>/dev/null | tr -d ' '
done
