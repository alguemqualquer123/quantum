---
name: doriax-build-verify
description: Build, configure and verify the doriax repo - CMake options, MSVC/MinGW toolchains, the standalone tool executables (doriax-reflector, doriax-resource-compiler, doriax-build-generator, doriax-engine), fast syntax checks when a full build is too slow, and CI parity. Use when compiling anything, debugging build errors, or before claiming an editor/engine change works.
---

# Build and verify

## One build tree: `DORIAX_BUILD_DIR`

There is exactly one build tree per checkout and its folder name is the
`DORIAX_BUILD_DIR` environment variable, defaulting to `build`. `scripts/*.bat`,
`.github/workflows/cmake.yml` and the generated game projects all read that same
value. Never invent another one (`build-debug/`, `build-release/`, `build-ninja/`,
`build-clean/` each carried their own CMakeCache, generated headers and object
files, and machine-specific paths from one of them leaked into committed template
projects). To move the tree, set the variable instead of adding a directory:

```bash
DORIAX_BUILD_DIR=/d/doriax-build cmake -S . -B "$DORIAX_BUILD_DIR"
```

The root `CMakeLists.txt` warns when it is configured into a sibling folder of the
checkout that is not the one the variable names.

## Configure and build

Local builds use whatever generator is cached; the default tree (`build`) is
**Visual Studio 17 2022 + MSVC**, and CI's authoritative job is MSVC
(`.github/workflows/cmake.yml`, MinGW is `continue-on-error`).

```bash
cmake -S . -B "$DORIAX_BUILD_DIR"             # reconfigure after CMakeLists edits
cmake --build "$DORIAX_BUILD_DIR" --target doriax-editor --config Debug
cmake --build "$DORIAX_BUILD_DIR" --target doriax --config Debug   # engine lib only
ctest test -V                                # exists, but there are NO unit tests
```

Key cache options (root `CMakeLists.txt`): `API_BACKEND` (auto: `linux|win|mac`),
`GRAPHIC_BACKEND` (`glcore|vulkan` on win/linux, `metal` on mac), `DORIAX_SHARED`
(force `ON` by the editor build), `NATIVE_MENU` (mac only), `DORIAXEDITOR_VERSION`
(otherwise from `git describe --tags --always --dirty`).

Targets: `doriax` (engine), `doriax-editor`, `doriax-editor-cmd` (Windows console
twin, `EXCLUDE_FROM_ALL`), `copy_doriax_editor_engine` (copies `engine/` next to the
exe - the editor and `doriax-engine` resolve the engine dir from there),
`generate_shaders`, `generate_api_suggestions`, and the `tools/` suite
(`doriax-reflector`, `doriax-resource-compiler`, `doriax-build-generator`,
`doriax-engine`, `doriax-resource-server`).

## The suite executables

```bash
"$DORIAX_BUILD_DIR/Debug/doriax-reflector.exe" reflect --bindings engine/core/script/binding \
    --out /tmp/s.hpp [--json api.json]
"$DORIAX_BUILD_DIR/Debug/doriax-reflector.exe" check --bindings engine/core/script/binding \
    --against engine_api_suggestions.h          # CI staleness gate, exit 1 = stale
"$DORIAX_BUILD_DIR/Debug/doriax-resource-compiler.exe" pak --project <dir> [--out x.pak]
"$DORIAX_BUILD_DIR/Debug/doriax-resource-compiler.exe" shaders --project <dir> [--force]
"$DORIAX_BUILD_DIR/Debug/doriax-resource-compiler.exe" inspect --pack x.pak
"$DORIAX_BUILD_DIR/Debug/doriax-engine.exe" --project <dir> [--scene main.lua] [--window 1280x720]
"$DORIAX_BUILD_DIR/Debug/doriax-resource-server.exe" --project <dir> [--port 0] [--once] [--parent <pid>]
```

`doriax-resource-server` answers newline-delimited JSON over 127.0.0.1 (`ping|status|index|entry|changes`),
keeps running after the editor closes and lives in the Windows notification area; `--port 0`
prints `doriax-resource-server: listening on 127.0.0.1:<port>`, `--once` scans and exits.

`scripts/reflect_api.sh [build-dir]` (in this skill's `scripts/`) regenerates the header
and diffs it against `$DORIAX_BUILD_DIR/generated/engine_api_suggestions.h`
(exit 1 = drift). Verified working.

## Fast check when a full build is not feasible

Shader sources are embedded, not read from disk: after any change under
`engine/tools/shaderlib/` run `cmake --build "$DORIAX_BUILD_DIR" --target
generate_shaders` before recompiling, or the edit is invisible
(`doriax-shaders`). Compiled variants are also cached per-user in
`%LOCALAPPDATA%/doriax/shaders/v23/*.sdat`, which can hide a broken shader.

A full editor build is minutes-long and the repo frequently has unrelated breakage.
Rank checks by cost:

1. `g++ -fsyntax-only -std=c++17 -I libs/imgui -I engine/core <file.cpp>` for pure
   logic files. For editor files harvest the real include set from
   `"$DORIAX_BUILD_DIR/doriax-editor.vcxproj"` (`AdditionalIncludeDirectories`) - the
   tree has no `compile_commands.json`.
2. `cl /Zs` with those same `/I` paths is the closest thing to the authoritative
   compiler check (MSVC rejects things MinGW accepts).
3. Build only the affected target, never `--target ALL_BUILD`.

## Gotchas that have burned time here

- **MinGW PATH collision**: `git` ships its own `mingw64/bin`. If
  `C:\Program Files\Git\mingw64\bin` resolves before `C:\mingw64\bin`, binaries built
  with GCC 15.2 load a mismatched `libstdc++-6.dll` and segfault inside the standard
  library (observed at `-O1`, gone at `-O0`/`-Os` - not a code bug). Prefix
  `PATH=/c/mingw64/bin:$PATH` when running MinGW-built tools from this shell.
- **MSVC catches what g++ waves through**: `tools/engine/main.cpp` passed
  `-fsyntax-only` and failed `cl` on a stringizing macro. Prefer `cl` for the final
  word, and put string defines in `target_compile_definitions(TGT PRIVATE X="${p}")`
  - `set_source_files_properties(COMPILE_DEFINITIONS ...)` needs manual `\"` escaping
  that MSVC and Ninja handle differently.
- **Winsock**: any engine TU touching Winsock2 needs `WIN32_LEAN_AND_MEAN` before
  `windows.h` and `winsock2.h` before `ws2tcpip.h` (the `doriax` target defines
  `WIN32_LEAN_AND_MEAN` on Windows; new targets must too, or `sockaddr` redefinition
  errors cascade ~100 deep).
- **Concurrent MSBuild**: two `cmake --build` runs in the same tree fail with
  `C1041` / PDB-lock errors that look like compile errors. One build at a time in
  the tree `DORIAX_BUILD_DIR` names - do not spawn a second tree to dodge it.
- **Generated game projects** resolve the engine import library in this order:
  `-DDORIAX_LIB_DIR=...`, `$DORIAX_LIB_DIR` (env), the editor directory baked at
  generate time. `doriax-build-generator configure` deliberately does not pass
  `-DDORIAX_LIB_DIR`, because that also skips the `DORIAX_SHARED DORIAX_EDITOR`
  macros the import library needs (the failure looks like unresolved
  `doriax::Engine::*` / `System::*` symbols). Use the env variable to point a
  project at another tree.
- **`DORIAX_EDITOR_SOURCES` is hand-maintained**: a new `.cpp` not added there builds
  in the IDE and vanishes from the product.

## Before claiming a change works

No test suite exists, so: (a) the target builds with MSVC, (b) the executable actually
runs and prints something sane (`--help` is a real smoke test), (c) for editor UI,
state explicitly that it was not visually verified if it was not launched.
