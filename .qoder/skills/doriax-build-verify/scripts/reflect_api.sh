#!/usr/bin/env bash
# Regenerate the engine API header with doriax-reflector and report drift against
# the header the build is using (build/generated/engine_api_suggestions.h).
# Usage: reflect_api.sh [build-dir] [header-to-compare]
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
BUILD="${1:-$REPO/build}"
BINDINGS="$REPO/engine/core/script/binding"
CURRENT="${2:-$BUILD/generated/engine_api_suggestions.h}"

REFLECTOR=""
for cand in \
    "$BUILD/Debug/doriax-reflector.exe" "$BUILD/Release/doriax-reflector.exe" \
    "$BUILD/doriax-reflector.exe" "$BUILD/Debug/doriax-reflector" "$BUILD/doriax-reflector"; do
    [ -x "$cand" ] && REFLECTOR="$cand" && break
done
if [ -z "$REFLECTOR" ]; then
    echo "FAIL: doriax-reflector not built. Run: cmake --build \"$BUILD\" --target doriax-reflector"
    exit 1
fi

OUT="$(mktemp -t doriax_api_XXXXXX.hpp)"

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
