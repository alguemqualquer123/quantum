#!/bin/sh
# UILab vendor sync (libs/uilab).
#
# The vendored copy is pristine upstream sources + the patch series under
# libs/uilab/doriax-patches. Upstream is https://github.com/alguemqualquer123/
# UniversalUIComponentsLab and the scratch clone used as the diff base lives in
# build/uilab-upstream (git-ignored, never committed).
#
#   scripts/sync-uilab.sh regen                 # after editing libs/uilab: rewrite the patches
#   scripts/sync-uilab.sh sync <sha|branch>     # take a new upstream revision, re-apply the patches
#
# third_party/ (Dear ImGui + GLFW) is not vendored: doriax builds ImGui itself and
# hands it to the library through the doriax_imgui target.
set -e
cd "$(dirname "$0")/.."

VENDOR=libs/uilab
UPSTREAM=build/uilab-upstream
PATCH_DIR=$VENDOR/doriax-patches
REPO=https://github.com/alguemqualquer123/UniversalUIComponentsLab

if [ ! -d "$UPSTREAM/.git" ]; then
    echo "error: $UPSTREAM is not a clone of $REPO"
    echo "       git clone $REPO $UPSTREAM"
    exit 1
fi

case "$1" in
regen)
    rm -f "$PATCH_DIR"/*.patch
    mkdir -p "$PATCH_DIR"
    i=0
    for f in $(cd "$UPSTREAM" && find . -type f \
            \( -name '*.cpp' -o -name '*.h' -o -name '*.txt' -o -name '*.cmake' -o -name '*.md' \) \
            | grep -v '^./third_party' | grep -v '^./examples/host-integration/out' | sed 's|^\./||' | sort); do
        if ! diff -q "$UPSTREAM/$f" "$VENDOR/$f" >/dev/null 2>&1; then
            i=$((i + 1))
            name=$(printf '%04d-%s.patch' "$i" "$(echo "$f" | tr '/ .' '---')")
            diff -u --label "a/$f" "$UPSTREAM/$f" --label "b/$f" "$VENDOR/$f" \
                > "$PATCH_DIR/$name" || true
            echo "wrote $PATCH_DIR/$name"
        fi
    done
    if [ "$i" = 0 ]; then echo "no differences against $UPSTREAM"; fi

    # Files doriax added to the library (a new seam header, say). sync copies the
    # pristine src/ tree over the vendored one, so these must come back as patches
    # that create the file.
    for f in $(cd "$VENDOR" && find src cmake -type f 2>/dev/null | sort); do
        if [ ! -f "$UPSTREAM/$f" ]; then
            i=$((i + 1))
            name=$(printf '%04d-%s-new.patch' "$i" "$(echo "$f" | tr '/ .' '---')")
            lines=$(wc -l < "$VENDOR/$f")
            {
                printf -- '--- /dev/null\n+++ b/%s\n@@ -0,0 +1,%s @@\n' "$f" "$lines"
                sed 's/^/+/' "$VENDOR/$f"
            } > "$PATCH_DIR/$name"
            echo "wrote $PATCH_DIR/$name"
        fi
    done
    ;;

sync)
    [ -n "$2" ] || { echo "usage: $0 sync <sha|branch>"; exit 1; }
    (cd "$UPSTREAM" && git fetch --all --tags && git checkout --detach "$2")
    for d in src cmake examples; do
        rm -rf "$VENDOR/$d"
        cp -r "$UPSTREAM/$d" "$VENDOR/$d"
    done
    cp "$UPSTREAM/CMakeLists.txt" "$VENDOR/CMakeLists.txt"
    cp "$UPSTREAM/README.md" "$VENDOR/README.md"
    rm -rf "$VENDOR/examples/host-integration/out"
    for p in "$PATCH_DIR"/*.patch; do
        [ -e "$p" ] || continue
        echo "applying $(basename "$p")"
        (cd "$VENDOR" && git apply --verbose "$p")
    done
    echo "vendored $2 - review with: git diff -- $VENDOR"
    ;;

*)
    echo "usage: $0 regen | sync <sha|branch>"
    exit 1
    ;;
esac
