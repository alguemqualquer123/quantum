---
name: quantum-build-verify
description: Build, configure and verify the Quantum repo - CMake options, MSVC/MinGW toolchains, the standalone tool executables (quantum-reflector, quantum-resource-compiler, quantum-build-generator, quantum-engine), fast syntax checks when a full build is too slow, and CI parity. Use when compiling anything, debugging build errors, or before claiming an editor/engine change works.
---

# Build and verify

## One build tree: `QUANTUM_BUILD_DIR`

There is exactly one build tree per checkout and its folder name is the
`QUANTUM_BUILD_DIR` environment variable, defaulting to `build`. `scripts/*.bat`,
`.github/workflows/cmake.yml` and the generated game projects all read that same
value. Never invent another one (`build-debug/`, `build-release/`, `build-ninja/`,
`build-clean/` each carried their own CMakeCache, generated headers and object
files, and machine-specific paths from one of them leaked into committed template
projects). To move the tree, set the variable instead of adding a directory:

```bash
QUANTUM_BUILD_DIR=/d/quantum-build cmake -S . -B "$QUANTUM_BUILD_DIR"
```

The root `CMakeLists.txt` warns when it is configured into a sibling folder of the
checkout that is not the one the variable names.

## Configure and build

Local builds use whatever generator is cached; the default tree (`build`) is
**Visual Studio 17 2022 + MSVC**, and CI's authoritative job is MSVC
(`.github/workflows/cmake.yml`, MinGW is `continue-on-error`).

```bash
cmake -S . -B "$QUANTUM_BUILD_DIR"             # reconfigure after CMakeLists edits
cmake --build "$QUANTUM_BUILD_DIR" --target quantum-editor --config Debug
cmake --build "$QUANTUM_BUILD_DIR" --target quantum --config Debug   # engine lib only
ctest test -V                                # exists, but there are NO unit tests
```

Key cache options (root `CMakeLists.txt`): `API_BACKEND` (auto: `linux|win|mac`),
`GRAPHIC_BACKEND` (`glcore|vulkan` on win/linux, `metal` on mac), `QUANTUM_SHARED`
(force `ON` by the editor build), `NATIVE_MENU` (mac only), `QUANTUMEDITOR_VERSION`
(otherwise from `git describe --tags --always --dirty`).

Targets: `quantum` (engine), `quantum-editor`, `quantum-editor-cmd` (Windows console
twin, `EXCLUDE_FROM_ALL`), `copy_quantum_editor_engine` (copies `engine/` next to the
exe - the editor and `quantum-engine` resolve the engine dir from there),
`generate_shaders`, `generate_api_suggestions`, and the `tools/` suite
(`quantum-reflector`, `quantum-resource-compiler`, `quantum-build-generator`,
`quantum-engine`, `quantum-resource-server`).

## The suite executables

```bash
"$QUANTUM_BUILD_DIR/Debug/quantum-reflector.exe" reflect --bindings engine/core/script/binding \
    --out /tmp/s.hpp [--json api.json]
"$QUANTUM_BUILD_DIR/Debug/quantum-reflector.exe" check --bindings engine/core/script/binding \
    --against engine_api_suggestions.h          # CI staleness gate, exit 1 = stale
"$QUANTUM_BUILD_DIR/Debug/quantum-resource-compiler.exe" pak --project <dir> [--out x.pak]
"$QUANTUM_BUILD_DIR/Debug/quantum-resource-compiler.exe" shaders --out <dir> --shader <spec> [--backend opengl]
"$QUANTUM_BUILD_DIR/Debug/quantum-resource-compiler.exe" inspect --pack x.pak
"$QUANTUM_BUILD_DIR/Debug/quantum-engine.exe" --project <dir> [--scene main.lua] [--window 1280x720] \
    [--frames N] [--warmup M] [--name LABEL] [--stats-out report.json] \
    [--screenshot PATH] [--capture-frame N]
"$QUANTUM_BUILD_DIR/Debug/quantum-editor-cmd.exe" components list|install --project <dir> [--only A,B] [--lua <dir>] [--into <subdir>] [--no-demos] [--overwrite]
"$QUANTUM_BUILD_DIR/Debug/quantum-editor-cmd.exe" assets   list|find <text>|copy <engine path> --project <dir> [--root assets|components|shaderlib] [--engine-dir <dir>] [--lua <dir>] [--overwrite] [--json]
"$QUANTUM_BUILD_DIR/Debug/quantum-editor-cmd.exe" doctor  --project <dir>   # audits project.yaml + scenes off disk, no Project load; exit 0/1/2
"$QUANTUM_BUILD_DIR/Debug/quantum-resource-server.exe" --project <dir> [--port 0] [--once] [--parent <pid>] [--token <hex>] [--window|--no-window]
```

`--frames N` turns the runner into a benchmark: it samples `Engine::FrameStats` per
presented frame, drops the first `--warmup` frames (default a quarter of `N`), prints one
JSON report and exits on its own, so a measured run needs neither a frame counter in Lua
nor `timeout` (see the gotcha below). `--stats-out` writes the same JSON to a file, which
makes a before/after diff one command. Numbers are only comparable within a build config -
a Debug exe pays sokol validation plus unoptimized MSVC and reports several times the
per-draw cost of Release.

`--screenshot PATH` is the only way to see what a headless run draws, and no graphics change is
done without it. The read happens in `Engine::runFrame` between `SystemRender::commit()` and the
platform swap (`engine/core/util/FrameCapture.cpp`, `Engine.cpp` runFrame), which is the single
point where sokol's immediate GL backend has the frame in the window's default framebuffer;
`Engine::onPostDraw` fires *before* the commit and cannot see it. `SystemWin::readBackFramebuffer`
resolves `glReadPixels`/`glPixelStorei` by name (`wglGetProcAddress` + an `opengl32.dll` fallback,
because no engine TU includes a GL header) and flips the rows, so the PNG is upright. Sample count
is 1, so no MSAA resolve is needed. `--capture-frame N` (default 60) counts finished frames;
`--screenshot` alone also sets `--frames N` and clears `--warmup`, so the run exits on the captured
frame. Same surface from Lua: `Engine.screenshot(path)` / `Engine.screenshotAfter(path, frames)`.
Vulkan has no read path yet and logs the reason. Relative paths are resolved against the *calling
shell's* cwd, not the project (the runner chdirs before the engine starts).

`quantum-engine --project <dir>` accepts a `project.yaml` or its directory and **chdirs into it**
(`tools/engine/main.cpp:173-204`), then boots `lua://<scene>.lua` - which `FileData.cpp:157-160`
resolves against `System::getLuaPath()` -> `<cwd>/lua/`. So the runner's scene root is
`<project>/lua/` whatever `luaDir:` says: it does **not** read `luaDir` (the editor does), and a
project authored with `luaDir: .` needs its scripts under `lua/` to be reachable from the CLI.
Asset paths inside a script or a `.scene` resolve against `System::getAssetPath()`, which is the
literal `"assets"` (`engine/platform/win/SystemWin.cpp:113-119`, applied at
`engine/core/io/MeshSystem.cpp:160`), so `loadModel("cube.obj")` and `materials/default/x.png` are
right and `loadModel("assets/cube.obj")` fails with `(ERROR): Model file not found: assets/cube.obj`.
It also exits 1 immediately with `Quantum is missing precompiled shaders` when the `.sdat` variants
are absent from `assets/shaders` - a scratch fixture needs them copied in, not just a `project.yaml`.
Generate exactly the variants the run asks for with
`quantum-resource-compiler shaders --out <project>/assets/shaders --backend opengl --shader <spec>`;
that path does **not** compile on demand, it only reports what the engine then refuses to draw.

`quantum-editor-cmd` is the `EXCLUDE_FROM_ALL` console twin of the editor; `components install` seeds
`engine/components/` (59 ready-made Lua gameplay scripts + `demos/`) into a project - see
`quantum-scripting`.

`quantum-editor-cmd assets list|find|copy` (`editor/cli/EngineAssetsCli.cpp`) is the headless twin of
the Resources window's Engine origin: it reads the same three roots through
`editor/util/EngineAssets.{h,cpp}`, which resolves `<exe>/engine` (the `copy_quantum_editor_engine`
output) and otherwise walks up to a checkout's `engine/`. Measured here: `assets list` reports
160 files / 3.5 MiB - `assets` 4, `components` 81, `shaderlib` 75. `--engine-dir <dir>` replaces that
resolution, and because the resolver caches its answer for the process the override must arrive
before the first query (`EngineAssets::overrideEngineDir` ignores a late one). Only `copy` writes, and
only into the project it names: a `.lua` lands in `<luaDir>/Components` where the *New component*
dialog looks, everything else under `<project>/assets/engine/<root>`; a file the project already owns
is skipped unless `--overwrite`, and a source that is not engine content is refused (exit 2, same as
a bad argument; 1 means nothing was found).

`quantum-editor-cmd doctor --project <dir>` (`editor/cli/ProjectDoctor.cpp`) reads `project.yaml` and the
`.scene` files straight off disk - no `Project::loadProject`, no window, no `Engine::systemInit` - so it
also audits projects the editor cannot open. One row per issue (`<severity> <check> <file>:<line>`), with
the real YAML line from `YAML::Mark`, and exit 0 clean / 1 warnings / 2 errors. It is the cheapest
headless health check in the repo and the one to run before blaming the generator. Its `missing-shader-file`
check resolves a `customShader` the way the editor does - a stem needs `<stem>.vert` **and** `<stem>.frag`,
the `vert|frag` form needs both named files (`Util::resolveCustomShaderPaths`) - so a forked shader is not
reported as missing just because no single file carries that exact name.

`quantum-resource-server` answers newline-delimited JSON over 127.0.0.1 (`ping|status|index|entry|changes|connections`),
keeps running after the editor closes and lives in the Windows notification area; `--port 0`
prints `quantum-resource-server: listening on 127.0.0.1:<port>`, `--once` scans and exits.
`connections` reports who asked for what, with the peer's real process name and PID read from
the OS TCP table (a client's own `"client"` tag is only a label). Started by hand it also opens a
plain-Win32 window with the two lists and Clear/Hide/Quit buttons; the editor-spawned one
(`--parent`) stays in the tray and the tray menu's "Show connections window" opens it later.
Closing that window only hides it - the service keeps serving.

`scripts/reflect_api.sh [build-dir]` (in this skill's `scripts/`) regenerates the header
and diffs it against `$QUANTUM_BUILD_DIR/generated/engine_api_suggestions.h`
(exit 1 = drift). Verified working.

## Fast check when a full build is not feasible

Shader sources are embedded, not read from disk: after any change under
`engine/shaderlib/` run `cmake --build "$QUANTUM_BUILD_DIR" --target
generate_shaders` before recompiling, or the edit is invisible
(`quantum-shaders`). Compiled variants are also cached per-user in
`%LOCALAPPDATA%/quantum/shaders/v40/*.sdat` (`editor/App.cpp:3792-3863`), but each entry
now signs the source graph it was compiled from (`<cache>.sources.sha1`,
`ShaderBuilder::isCacheStale`), so a `shaderlib` edit invalidates only the variants built
from that text - the version directory is a blunt fallback for a `.sdat` format change or
a C++-side `#define`, and deleting it is how you force the absolute truth.

A full editor build is minutes-long and the repo frequently has unrelated breakage.
Rank checks by cost:

1. `g++ -fsyntax-only -std=c++17 -I libs/imgui -I engine/core <file.cpp>` for pure
   logic files. For editor files harvest the real include set from
   `"$QUANTUM_BUILD_DIR/quantum-editor.vcxproj"` (`AdditionalIncludeDirectories`) - the
   tree has no `compile_commands.json`.
2. `cl /Zs` with those same `/I` paths is the closest thing to the authoritative
   compiler check (MSVC rejects things MinGW accepts).
3. Build only the affected target, never `--target ALL_BUILD`.
4. To prove an editor *panel* without launching it, drive it from a scratch MSBuild
   console project outside the repo: compile the window `.cpp` with the real
   `libs/imgui/imgui*.cpp` and stub only what it calls (`App::getCodeFont`,
   `App::requestRedraw`, `Backend::getApp`, the `AppSettings` getters it reads,
   `Project::getProjectPath`). `CreateContext` + `io.Fonts->GetTexDataAsRGBA32` +
   `NewFrame` / `window->show()` / `Render` draws a frame with no renderer, and
   `io.AddMousePosEvent` / `AddMouseButtonEvent` / `AddInputCharacter` reach the widgets.
   Set `_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE)` (plus `_CRTDBG_FILE_STDERR`)
   before anything else: a Debug CRT otherwise raises a dialog nobody can read, and with
   that set an `IM_ASSERT` prints its expression - which is how a real `abort()` on
   opening a panel was located. Read the widget's own state back with
   `#define private public` around the header include, `_ALLOW_KEYWORD_MACROS` on the
   command line, and that same `.cpp` compiled with `/Dprivate=public` so the members
   mangle alike.

## Localising a native crash, cheaply

`quantum-engine.exe` dying with exit 139 and no message is a segfault, and this tree cannot name the
function: the DLL is MSVC-built, and neither route works - gdb prints every frame as
`quantum!?AddForce@BodyInterface@JPH@...+21923445` (nearest export, useless), and dbghelp cannot read
the PDB from a MinGW-compiled helper either (`SymFromAddr` gives err=87; mingw's `SYMBOL_INFO` layout
differs from dbghelp's, so hand-offset field reads are needed just to get *something* back). Two days
of that were wasted; do not retry it. What does work:

1. `gdb --batch -x cmds.gdb --args ./build/Debug/quantum-engine.exe <args>` with `run`, `bt 120`,
   `frame 0`, `info registers rip rsp rbp`, `x/6i $rip`. A *complete* stack (ending in
   `KERNEL32!BaseThreadInitThunk`) rules out recursion, and the faulting instruction plus its
   neighbour names the code: `mov %cl,0x79(%rax)` followed by `call ...ResetSleepTimer` was
   `Body::SetAllowSleeping` writing through a null `mMotionProperties`.
2. Otherwise instrument the candidate engine path with `Log::warn(...)` and rebuild only `quantum`
   (~4 min), because `quantum-editor.exe`/`quantum-engine.exe` load `quantum.dll` at run time - a
   relink is not needed for an engine-internal change. **Put `fflush(nullptr)` after each probe**:
   stdout is CRT-full-buffered when redirected, so a segfault discards the tail and the last line you
   need is the one that never landed.

Bisect with headless scenes, not with the debugger: one probe per hypothesis in
`%TEMP%/KitLive/lua/`, run `--scene <bare base name> --frames N`, and compare which ones reach an
exit 0.

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
  `windows.h` and `winsock2.h` before `ws2tcpip.h` (the `quantum` target defines
  `WIN32_LEAN_AND_MEAN` on Windows; new targets must too, or `sockaddr` redefinition
  errors cascade ~100 deep).
- **`editor/window/Properties.cpp` is at MSVC's object-section limit**: adding template-heavy code
  there fails with `C1128: number of sections exceeded object file format limit: compile with
  /bigobj` (it is a TU-size limit, not a syntax error, and it reports at `(1,1)`). Neither editor
  target passes `/bigobj` - only `engine/CMakeLists.txt:720` and
  `tools/engine/quantum_engine_exe.cmake:199` do - so give new logic its own file
  (`editor/util/ReadyMadeComponents.cpp` is the precedent) instead of growing that one or flipping
  the flag.
- **Concurrent MSBuild**: two `cmake --build` runs in the same tree fail with
  `C1041` / PDB-lock errors that look like compile errors. One build at a time in
  the tree `QUANTUM_BUILD_DIR` names - do not spawn a second tree to dodge it.
- **A piped build hides its own failure**: `cmake --build ... > log 2>&1; echo "EXIT=$?"`
  reports the *echo*'s status to the caller, and `cmake --build ... | tail` reports the
  pipeline's. Write the real status into the log (`echo "EXIT=$?" >> log`) and grep it out,
  otherwise a build that died at the link reads as a pass.
- **LNK1168 means the target exe is running**: the editor spawns `quantum-resource-server.exe`
  and that service outlives the editor by design, so stale tray instances are what block the
  link. Closing them is not enough if an editor session starts again in between - re-check
  immediately before retrying, and by path rather than by image name
  (`powershell.exe -NoProfile -Command "Get-Process | Where-Object { $_.Path -like '*quantum*' }"`),
  which also catches a process under another name. Ask before closing anything the user may
  have open. `taskkill //PID <pid> //F` prints nothing and can leave the process alive from this
  shell; `powershell Stop-Process -Id <pid> -Force` does kill it.
- **`timeout N quantum-engine.exe` does not reliably kill the child on Git Bash/Windows.** The
  windowed process survives, keeps `quantum.dll` open (next link: LNK1168) *and* its `stdout` dies
  with the kill, so the log you redirect comes back with zero lines and you conclude "it hung
  silently" when it had actually printed errors. Give a headless run its own exit: `--frames N`
  (preferred, it quits by itself), or a frame counter that calls `System.quit()`
  (`engine/components/demos` use `KIT_DEMO_FRAMES`). Note that
  `System.quit()` inside `QUANTUM_INIT init()` does not end the runner - the frame loop starts
  after it - so quit from an `onUpdate`. The same is true of a bare `System.quit()` at the end of a
  scene script's top-level body: it runs before the loop exists, so the window opens and never
  closes. Two such leftovers sat unnoticed for 70 minutes and came back as
  `LNK1168: cannot open quantum.dll for writing` in someone else's build, so when a link fails on
  the dll, list the holders by path *first* - a probe that never quit is a process you started
  yourself and may kill.
- **A harness that hangs with no output is usually a modal CRT dialog, not a deadlock**:
  `Debug Assertion Failed!` blocks at ~0 CPU and flushes nothing, and `-fsyntax-only` cannot
  see it (observed from walking a `std::vector` backwards while erasing, and from giving
  `_CrtSetReportMode` the OR of `_CRT_ASSERT|_CRT_ERROR|_CRT_WARN` instead of one type). Find
  it with `(Get-Process -Id <pid>).MainWindowTitle`, read the text by enumerating that window's
  children (`EnumChildWindows` + `GetWindowTextW`, keeping a reference to the delegate or the
  callback is collected and returns nothing), and make headless runs non-blocking by routing
  reports to stderr: `_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE)` then
  `_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR)`.
- **Generated game projects** resolve the engine import library in this order:
  `-DQUANTUM_LIB_DIR=...`, `$QUANTUM_LIB_DIR` (env), the editor directory baked at
  generate time. `quantum-build-generator configure` deliberately does not pass
  `-DQUANTUM_LIB_DIR`, because that also skips the `QUANTUM_SHARED QUANTUM_EDITOR`
  macros the import library needs (the failure looks like unresolved
  `quantum::Engine::*` / `System::*` symbols). Use the env variable to point a
  project at another tree.
- **`QUANTUM_EDITOR_SOURCES` is hand-maintained**: a new `.cpp` not added there builds
  in the IDE and vanishes from the product.
- **A source-list edit landing mid-build gives a one-target `LNK2019`.** Each suite exe
  compiles the whole editor list into its own `build/<target>.dir/Debug`, so if an entry
  is added to `CMakeLists.txt` after the configure that drives a compile, the caller's
  `.obj` links against a `.obj` that was never built - `quantum-editor` is fine,
  `quantum-build-generator` fails on symbols that plainly exist. Confirm by comparing
  `<ClCompile Include=` in the on-disk vcxproj against the obj dir; the next build
  reconfigures and the failure disappears. Never "fix" it by editing the source.
- **A relinked `quantum.dll` does not relink its consumers.** MSBuild keys the exe on
  `quantum.lib`, and the linker leaves that file's timestamp alone when the export list
  is unchanged, so a header-only change (a struct gaining a field) ships a new dll with
  exes compiled against the old offsets - the crash is a null/garbage member read on the
  first call across the boundary, e.g. `Project::loadScene` -> `createSceneRender` ->
  `Scene::getSystem<T>`. `engine/CMakeLists.txt` now `touch`es
  `$<TARGET_LINKER_FILE:quantum>` POST_BUILD under the VS generator; if the symptom
  returns, compare `ls -la build/Debug/*.{exe,dll,lib}` before debugging the code, and
  rebuild the stale exes rather than theorising about the call stack.

## Before claiming a change works

No test suite exists, so: (a) the target builds with MSVC, (b) the executable actually
runs and prints something sane (`--help` is a real smoke test), (c) for editor UI,
state explicitly that it was not visually verified if it was not launched.
