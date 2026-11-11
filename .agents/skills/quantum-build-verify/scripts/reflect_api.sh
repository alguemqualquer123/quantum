#!/usr/bin/env bash
# Regenerate the engine API header with quantum-reflector and report drift against
# the header the build is using ($QUANTUM_BUILD_DIR/generated/engine_api_suggestions.h).
# Usage: reflect_api.sh [build-dir] [header-to-compare]
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
BUILD="${1:-${QUANTUM_BUILD_DIR:-build}}"
case "$BUILD" in /*|?:*) ;; *) BUILD="$REPO/$BUILD";; esac
BINDINGS="$REPO/engine/core/script/binding"
CURRENT="${2:-$BUILD/generated/engine_api_suggestions.h}"

REFLECTOR=""
for cand in \
    "$BUILD/Debug/quantum-reflector.exe" "$BUILD/Release/quantum-reflector.exe" \
    "$BUILD/quantum-reflector.exe" "$BUILD/Debug/quantum-reflector" "$BUILD/quantum-reflector"; do
    [ -x "$cand" ] && REFLECTOR="$cand" && break
done
if [ -z "$REFLECTOR" ]; then
    echo "FAIL: quantum-reflector not built. Run: cmake --build \"$BUILD\" --target quantum-reflector"
    exit 1
fi

OUT="$(mktemp -t quantum_api_XXXXXX.hpp)"

if ! "$REFLECTOR" reflect --bindings "$BINDINGS" --out "$OUT" >/dev/null; then
    echo "FAIL: reflector exited non-zero"
    rm -f "$OUT"
    exit 1
fi

if [ ! -f "$CURRENT" ]; then
    echo "MISSING: $CURRENT - build the generate_api_suggestions target first, or copy:"
    echo "  cp \"$OUT\" \"$CURRENT\""
    exit 1
fi

# The banner line differs by design between the Python generator and the reflector.
if diff -q <(grep -v '^//' "$CURRENT" | sort) <(grep -v '^//' "$OUT" | sort) >/dev/null; then
    rm -f "$OUT"
    echo "OK: $CURRENT matches the current bindings"
    exit 0
fi

echo "STALE: $CURRENT differs from $BINDINGS"
diff <(grep -v '^//' "$CURRENT" | sort) <(grep -v '^//' "$OUT" | sort) | head -40
exit 1
