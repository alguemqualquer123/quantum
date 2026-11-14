#!/bin/sh
# Cheap pre-check for the Slate module: MinGW syntax-only pass over given files.
# MSVC stays authoritative for what actually ships; this catches missing
# declarations, typos and header breakage in seconds without touching the shared
# build tree (no PDB, no lock, no object files).
#
#   scripts/slate-syntax-check.sh engine/slate/core/Widget.cpp ...
# With no arguments it checks every .cpp in the module and the demo.
#
# Include dirs are the ones CMake exports (harvested from
# build/quantum-editor.vcxproj AdditionalIncludeDirectories) plus the module root.
set -e
cd "$(dirname "$0")/.."

INC="-Iengine -Iengine/core -Iengine/core/action -Iengine/core/buffer -Iengine/core/component -Iengine/core/ecs -Iengine/core/io -Iengine/core/manager -Iengine/core/math -Iengine/core/object -Iengine/core/object/ui -Iengine/core/pool -Iengine/core/registry -Iengine/core/render -Iengine/core/script -Iengine/core/shader -Iengine/core/subsystem -Iengine/core/texture -Iengine/core/util -Iengine/platform/common -Iengine/platform/win -Iengine/renders -Iengine/libs/sokol -Iengine/libs/stb -Iengine/libs/yoga -Iengine/libs/harfbuzz -Iengine/libs/lua -Iengine/libs/luabridge3 -Iengine/libs/sheenbidi/Headers -Iengine/libs/json -Iengine/libs/box2d/include -Iengine/libs/joltphysics -Iengine/libs/soloud/include -Iengine/libs/tinygltf -Iengine/libs/tinyobjloader -Ilibs/doctest -Ibuild/generated"

if [ $# -gt 0 ]; then
    FILES="$*"
else
    FILES=$(find engine/slate engine/slate_demo tests/slate -name '*.cpp' 2>/dev/null)
fi

if [ -z "$FILES" ]; then
    echo "no Slate sources found"
    exit 0
fi

fail=0
for f in $FILES; do
    if g++ -fsyntax-only -std=c++17 $INC "$f" 2>/tmp/slate_syntax_err.txt; then
        echo "ok   $f"
    else
        echo "FAIL $f"
        head -20 /tmp/slate_syntax_err.txt
        fail=1
    fi
done
exit $fail
