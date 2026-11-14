# Quantum Engine

![Quantum](https://alguemqualquer123.github.io/quantum-docs/assets/logo/quantum_wordmark.png)

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Latest release](https://img.shields.io/github/v/release/alguemqualquer123/quantum)](https://github.com/alguemqualquer123/quantum/releases)
[![CI](https://img.shields.io/github/actions/workflow/status/alguemqualquer123/quantum/cmake.yml?label=CI)](https://github.com/alguemqualquer123/quantum/actions/workflows/cmake.yml)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C.svg)](https://en.cppreference.com/w/cpp/17)
[![Docs](https://img.shields.io/badge/docs-online-2563eb.svg)](https://alguemqualquer123.github.io/quantum-docs/)

Quantum is an MIT-licensed C++17 game engine and visual editor for 2D and 3D
games, maintained by SR VINIX. It ships as one repository: the runtime, the
editor, the headless tooling and a library of ready-made gameplay scripts, so a
project goes from an empty scene to a packaged build without leaving the tree.

![The Quantum editor with a 3D scene open](https://alguemqualquer123.github.io/quantum-docs/assets/screenshots/editor-overview.png)

## What is in the box

**Rendering.** Forward PBR with directional, point and spot lights, shadow
cascades and reflection probes; SSAO, SSR and depth-of-field; an HDR chain with
bloom, eye adaptation, tone mapping and colour grading; FXAA closing every
frame. Shaders are GLSL, compiled offline and cached on disk.

**Worlds.** CDLOD terrain with a deterministic heightmap generator (fBm plus
hydraulic and thermal erosion, identical bytes for the same seed from Lua, the
CLI or the editor brush), blend-map splatting, instanced foliage, and a
shallow-water simulation that runs downhill, pools, takes rain, evaporates and
carries floating bodies.

**Gameplay.** An ECS runtime with Lua and C++ scripting (hot reload included),
Box2D and Jolt physics behind one API, 2D and 3D skeletons and animation,
particles, audio, input and networking. `engine/components/` ships 59
ready-made Lua gameplay scripts — controllers, camera rigs, rope, fire that
spreads to fuel, day-night with a real moon, weather, HUD — that new projects
are seeded with automatically.

**Editor.** A Dear ImGui editor with docking, an animation timeline, sprite and
tileset slicers, a terrain painter, a script debugger, a code editor, play mode
and network play. It also runs an MCP server, so an AI agent can drive scenes
and builds, and every validation it can do in the GUI it can do headless.

**Shipping.** Native export to Windows, Linux, macOS, Android, iOS and HTML5,
with asset bundles, asset validation and a command-line release pipeline.

## Quick start

```powershell
cmake -S . -B build
cmake --build build --config Release --target quantum-editor
```

On Windows the authoritative CI toolchain is MSVC. Open the editor, create a
project from a template, press play. Step-by-step builds for Linux, macOS,
Android, iOS and HTML5, including the optional Vulkan editor, are in
[Building from Source](https://alguemqualquer123.github.io/quantum-docs/building/overview/).

Everything the GUI validates, the CLI validates without a window:

```powershell
build\Release\quantum-editor-cmd.exe doctor --project templates\daynight
build\Release\quantum-editor-cmd.exe export --project templates\daynight
build\Release\quantum-engine.exe --project templates\daynight --screenshot frame.png --capture-frame 120
```

A Lua script is one file with no boilerplate:

```lua
function init(self)
    self.speed = 6.5
end

RegisterEngineEvent("update", function(self, dt)
    local move = Vector3(Input.axis("horizontal"), 0, Input.axis("vertical"))
    Transform.translate(self.entity, move * self.speed * dt)
end)
```

## Documentation

The full manual, tutorials and API reference live at
[alguemqualquer123.github.io/quantum-docs](https://alguemqualquer123.github.io/quantum-docs/).
Start with [Your First Project](https://alguemqualquer123.github.io/quantum-docs/getting-started/first-project/).

AI agents: the site publishes [`llms.txt`](https://alguemqualquer123.github.io/quantum-docs/llms.txt)
and [`llms-full.txt`](https://alguemqualquer123.github.io/quantum-docs/llms-full.txt),
and the repository carries its own `AGENTS.md` plus a 25-skill set describing
the architecture, the hand-maintained registration lists and the build rules.

## Community

- Discord: <https://discord.gg/gSHUVHXNwP>
- Issues and discussions: <https://github.com/alguemqualquer123/quantum/issues>

## License and credits

Quantum is distributed under the [MIT License](LICENSE). It is a fork of the
Doriax Engine, (c) Eduardo Doria — MIT; the upstream copyright headers and the
LICENSE are preserved verbatim. Vendored dependencies and third-party assets
retain their own licenses.
