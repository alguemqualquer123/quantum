<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/doriaxengine/doriax-site/main/logo/doriax_logo_transparent.png">
    <source media="(prefers-color-scheme: light)" srcset="https://raw.githubusercontent.com/doriaxengine/doriax-site/main/logo/doriax_logo_dark.png">
    <img src="https://raw.githubusercontent.com/doriaxengine/doriax-site/main/logo/doriax_logo_dark.png" alt="Doriax Engine" width="300">
  </picture>
</p>

<p align="center">
  <strong>A lightweight native C++ engine for 2D and 3D games, with a visual editor and Lua scripting.</strong>
</p>

<p align="center">
  <strong>Your game. Your source. Your build.</strong>
</p>

<p align="center">
  <a href="https://doriax.org/#download"><strong>Download</strong></a> ·
  <a href="https://docs.doriax.org"><strong>Documentation</strong></a> ·
  <a href="https://github.com/doriaxengine/doriax/releases"><strong>Releases</strong></a> ·
  <a href="https://discord.gg/yXXDyJf3gT"><strong>Discord</strong></a>
</p>

<p align="center">
  <a href="https://www.youtube.com/watch?v=3eqhaAZBNss">
    <img src="https://raw.githubusercontent.com/doriaxengine/doriax-site/main/screenshots/editor-lighting.png" alt="Watch Doriax Engine in action">
  </a>
</p>

<p align="center">
  <a href="https://github.com/doriaxengine/doriax/actions/workflows/cmake.yml"><img src="https://github.com/doriaxengine/doriax/actions/workflows/cmake.yml/badge.svg?branch=main" alt="Editor Desktop build status"></a>
  <a href="https://github.com/doriaxengine/doriax/actions/workflows/engine-cmake.yaml"><img src="https://github.com/doriaxengine/doriax/actions/workflows/engine-cmake.yaml/badge.svg?branch=main" alt="Engine Desktop build status"></a>
  <a href="https://github.com/doriaxengine/doriax/actions/workflows/engine-android.yml"><img src="https://github.com/doriaxengine/doriax/actions/workflows/engine-android.yml/badge.svg?branch=main" alt="Engine Android build status"></a>
  <a href="https://github.com/doriaxengine/doriax/actions/workflows/engine-emscripten.yaml"><img src="https://github.com/doriaxengine/doriax/actions/workflows/engine-emscripten.yaml/badge.svg?branch=main" alt="Engine Emscripten build status"></a>
  <a href="https://github.com/doriaxengine/doriax/actions/workflows/engine-xcode.yaml"><img src="https://github.com/doriaxengine/doriax/actions/workflows/engine-xcode.yaml/badge.svg?branch=main" alt="Engine iOS and macOS build status"></a>
  <a href="https://discord.gg/yXXDyJf3gT"><img src="https://img.shields.io/discord/1356958061880934480?label=Discord&logo=discord&style=flat&color=5865F2" alt="Join the Doriax Discord"></a>
</p>

## Why Doriax?

Doriax is for developers who want a smaller, native, code-transparent engine without giving up a visual editor. It combines scene authoring, scripting, shader tooling, builds, and exports in one open-source workflow.

- **Lightweight** — a lean, data-oriented runtime that keeps abstractions and engine overhead under control
- **Native C++** — native platform and graphics backends, with C++ available throughout the stack
- **Lua and C++ together** — prototype quickly in Lua, write native gameplay code in C++, or mix both
- **Visual workflow** — edit 2D, 3D, and UI scenes with a hierarchy, inspector, resources, animation timeline, and play mode
- **ECS and data-oriented** — shared 2D/3D runtime designed around cache-friendly component data
- **Open and hackable** — MIT-licensed engine code and readable exported projects, with no closed build pipeline
- **Cross-platform** — target Windows, Linux, macOS, Android, iOS, and HTML5

## Native projects you control

**Source Code** export produces a standalone native CMake project—not an opaque package or a build that depends on a closed service. The project contains the generated C++ scene code, your Lua and C++ scripts, assets, platform backends, compiled shaders, and the engine source it needs.

The exported project is yours: inspect the result, add native integrations, change the engine, and compile the game yourself outside the editor with standard native toolchains.

When you want a finished artifact instead, use **Desktop** export to build a ready-to-run native binary for the current computer, or **Web** export to build HTML and WebAssembly with Emscripten.

## Editor workflow

- Build 2D and 3D scenes with sprites, tilemaps, models, cameras, lights, and reusable entities
- Edit component properties, project resources, animations, bones, and terrain without leaving the editor
- Write Lua and C++ in the integrated code editor with engine API completion
- Fork the built-in Mesh, UI, Points, Lines, and Sky shaders, plus custom shaders for ordered post-process passes, with live viewport recompilation
- Run the game in play mode, inspect build output, and export scenes, assets, scripts, engine files, and compiled shaders
- Optionally call on the built-in AI assistant to inspect the project, create entities, draft Lua and C++ scripts, and invoke builds, with preview-then-approve, auto-run-read-only, and full-agent modes deciding how much it may do on its own

<p align="center">
  <img src="https://raw.githubusercontent.com/doriaxengine/doriax-site/main/screenshots/readme/editor-office-scene.png" alt="Office scene in the Doriax 3D editor" width="48%">
  <img src="https://raw.githubusercontent.com/doriaxengine/doriax-site/main/screenshots/editor-2d-sprite.png" alt="Sprite and tilemap tools in the Doriax 2D editor" width="48%">
</p>
<p align="center">
  <img src="https://raw.githubusercontent.com/doriaxengine/doriax-site/main/screenshots/readme/editor-code.png" alt="Integrated code editor in Doriax" width="48%">
  <img src="https://raw.githubusercontent.com/doriaxengine/doriax-site/main/screenshots/readme/runtime-first-ui-scene.png" alt="UI scene in Doriax" width="48%">
</p>
<p align="center">
  <img src="https://raw.githubusercontent.com/doriaxengine/doriax-site/main/screenshots/readme/editor-ai-chat.png" alt="Doriax AI assistant creating entities and scripts" width="48%">
  <img src="https://raw.githubusercontent.com/doriaxengine/doriax-site/main/screenshots/editor-bones.png" alt="Bone animation tools in Doriax" width="48%">
</p>

## Engine features

- Shared ECS runtime for 2D and 3D scenes, with scene layers and serialization
- Sprites, tilemaps, and polygons, plus 2D lights, normal maps, and occluder shadows
- GLTF and OBJ models with skeletal animation, morph targets, and mesh instancing
- PBR materials with dynamic lights and cascaded shadows, sky-driven IBL, SSAO, fog, and planar mirrors
- Keyframe tracks, runtime actions with easing, crossfade blending, and sprite-sheet animation
- Particle systems and heightmap terrain with clipmap LOD and sculpting tools
- UI toolkit with anchors, containers, text, images, buttons, scrollbars, and other widgets
- Integrated Box2D and Jolt Physics support for 2D/3D bodies, shapes, and joints
- Spatial 3D audio, texture and shader pools, and multithreaded asynchronous resource loading

## Platform support

| Area | Support |
| --- | --- |
| Editor | Windows, Linux, macOS |
| Export targets | Windows, Linux, macOS, Android, iOS, HTML5 |
| Rendering backends | OpenGL/OpenGL ES, Vulkan, Metal, Direct3D 11 |
| Scripting | Lua, C++ |

Backend availability depends on the target platform. OpenGL is the default editor backend on Windows and Linux, while macOS uses Metal; Vulkan can be selected on Windows or Linux at configure time.

## Get started

Download the latest tagged release from the [Doriax website](https://doriax.org/#download) or the [GitHub releases page](https://github.com/doriaxengine/doriax/releases), then follow [Your First Project](https://docs.doriax.org/getting-started/first-project/) to create a scene, attach a script, and run it in the editor.

> [!IMPORTANT]
> **Tagged release:** recommended for projects. **`main` build:** the newest development version, which may contain regressions, incomplete work, or breaking changes.

### Build from source

All platforms require:

- A C++17 compiler: MSVC, GCC, or Clang
- CMake 3.27 or newer (required by the bundled shader compiler)
- Python 3

Clone the repository before following the instructions for your platform:

```bash
git clone https://github.com/doriaxengine/doriax.git
cd doriax
```

Every example below configures a single build tree whose folder name comes from the
`DORIAX_BUILD_DIR` environment variable, defaulting to `build`. Export it once (in your
shell profile, or per command as `DORIAX_BUILD_DIR=... cmake -S . -B "$DORIAX_BUILD_DIR"`)
to keep the tree on another disk; `scripts/doriax-build-dir.bat` and the CI workflows
read that same value. One checkout, one build tree: switching toolchain or graphics
backend means reconfiguring it, not adding a second directory beside it.

#### Linux (Ubuntu 26.04)

Ubuntu 26.04 includes a recent enough CMake in its official repositories, so no third-party package source is required:

```bash
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  build-essential cmake git ninja-build pkg-config python3 \
  libx11-dev libxcursor-dev libxi-dev libxrandr-dev \
  libgl1-mesa-dev libwayland-dev wayland-protocols \
  libdbus-1-dev libcurl4-openssl-dev

export DORIAX_BUILD_DIR=${DORIAX_BUILD_DIR:-build}
cmake -S . -B "$DORIAX_BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$DORIAX_BUILD_DIR" --target doriax-editor
"./$DORIAX_BUILD_DIR/doriax-editor"
```

To create a redistributable install tree under `dist/`:

```bash
cmake --install "$DORIAX_BUILD_DIR" --prefix dist
```

#### macOS

Install the Xcode Command Line Tools, CMake, and Ninja:

```bash
xcode-select --install
brew install cmake ninja
export DORIAX_BUILD_DIR=${DORIAX_BUILD_DIR:-build}

cmake -S . -B "$DORIAX_BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$DORIAX_BUILD_DIR" --target doriax-editor
open "$DORIAX_BUILD_DIR/Doriax.app"
```

To create a redistributable app under `dist/bin/`:

```bash
cmake --install "$DORIAX_BUILD_DIR" --prefix dist
```

#### Windows (Visual Studio)

Install Python 3 and Visual Studio 2022 or newer with the **Desktop development with C++** workload and **C++ CMake tools for Windows** component. Run these commands from a Developer PowerShell or Developer Command Prompt:

```powershell
if (-not $env:DORIAX_BUILD_DIR) { $env:DORIAX_BUILD_DIR = 'build' }
cmake -S . -B $env:DORIAX_BUILD_DIR -G "Visual Studio 17 2022" -A x64
cmake --build $env:DORIAX_BUILD_DIR --config Release --target doriax-editor doriax-editor-cmd
& "$env:DORIAX_BUILD_DIR\Release\doriax-editor.exe"
```

If you use Visual Studio 2026, replace the generator with `"Visual Studio 18 2026"`. The explicit `doriax-editor-cmd` target is required because the console executable is excluded from the default build.

To install the editor, CLI executable, runtime library, and engine files under `dist/bin/`:

```powershell
cmake --install $env:DORIAX_BUILD_DIR --prefix dist --config Release
```

#### Optional Vulkan editor

The editor defaults to OpenGL on Windows and Linux and to Metal on macOS. To use Vulkan on Windows or Linux, install the Vulkan SDK and add `-DGRAPHIC_BACKEND=vulkan` to that platform's configure command, in the same build tree:

```bash
cmake -S . -B "$DORIAX_BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DGRAPHIC_BACKEND=vulkan
cmake --build "$DORIAX_BUILD_DIR" --target doriax-editor
```

Switch back with `-DGRAPHIC_BACKEND=glcore` rather than keeping a second directory for
each backend: a parallel tree duplicates the cache, the generated headers and every
object file. On Windows, add `-DGRAPHIC_BACKEND=vulkan` to the Visual Studio configure
command instead.

## Command-line tools

The editor executable also exposes automation commands for project export and standalone shader generation:

```bash
doriax-editor export --help
doriax-editor shaders --help
```

Windows release packages use `doriax-editor-cmd.exe` for console automation; the Windows build instructions above create it alongside the GUI editor.

## Try it in your browser

These small projects demonstrate real Doriax builds running on the web. Play them, then inspect the source:

- **Character Control** — [Play](https://doriaxengine.github.io/charactercontrol/) · [Source](https://github.com/doriaxengine/charactercontrol)
- **Simple Physics Control** — [Play](https://doriaxengine.github.io/simplephysicscontrol/) · [Source](https://github.com/doriaxengine/simplephysicscontrol)

## Repository layout

- `editor/` — desktop editor, AI assistant, project tools, build orchestration, and export flow
- `engine/` — ECS runtime, platform layers, rendering, scripting, and project templates
- `shadercompiler/` — shader compilation and cross-platform translation
- `libs/` — bundled third-party dependencies

## Project links

- [Website](https://doriax.org)
- [Documentation](https://docs.doriax.org)
- [Your First Project](https://docs.doriax.org/getting-started/first-project/)
- [Releases](https://github.com/doriaxengine/doriax/releases)
- [Issues](https://github.com/doriaxengine/doriax/issues)
- [Discord](https://discord.gg/yXXDyJf3gT)

Issues and pull requests are welcome. For substantial changes, start with an issue or discuss the proposal with the community on Discord.

## From Supernova to Doriax

Doriax Engine is the next phase of Supernova Engine. Version `0.5.5` was the final release under the previous name, and `0.6` is the first Doriax release. Some internal folders and older external references may still use the Supernova name while the transition continues.

## License

Doriax Engine is available under the [MIT License](LICENSE) and can be used in personal and commercial projects. Bundled third-party libraries retain their respective licenses.
