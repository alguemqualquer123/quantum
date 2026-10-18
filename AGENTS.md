# Doriax — guide for coding agents

C++17 game engine (`CXX_STANDARD 17`, `CMakeLists.txt:749`) + Dear ImGui editor, MIT,
by Eduardo Doria.
Layout: `engine/` (runtime lib, target `doriax`), `editor/` (the editor app),
`templates/` (sample game projects), `tools/` (headless executables),
`engine/libs/` + `libs/` (vendored deps: sokol, imgui, Lua/LuaBridge, box2d, Jolt,
glslang/SPIRV-Cross, yaml-cpp, tinygltf, stb, sheenbidi, harfbuzz), `scripts/`,
`.github/workflows/cmake.yml` (authoritative CI job is MSVC).

Every file needs the `// (c) Eduardo Doria` + `SPDX-License-Identifier: MIT` header.
`namespace doriax`; classic include guards in `engine/`, `#pragma once` in `editor/`;
4-space indent; comments in English and only where the *why* is non-obvious.

## Hard rules (violating these caused real damage here)

1. **Never launch `doriax-editor.exe`** (or any build of it) to verify a change, and
   never kill the user's running editor without asking. Its autosave rewrote a template
   scene into ~18,870 bad lines. Verify headless; if only the GUI can prove it, say the
   change is visually unverified and ask the user to look.
2. **One build tree**: `$DORIAX_BUILD_DIR`, default `build`. Do not create
   `build-debug/`, `build-ninja/`, etc. to dodge a lock; two concurrent
   `cmake --build` runs in one tree fail with misleading `C1041`/PDB errors.
3. **Never `git add -A` / `git add .`.** This tree routinely holds concurrent agents'
   in-flight edits. Stage explicit file lists, review `git status` after staging, and do
   not commit work you did not write. Never commit run artifacts (`stderr.txt`,
   `stdout.txt`, `build_editor.log`, `templates/*/{editor.log,settings.yaml,editor_layout.ini}`).
4. **Never hand-edit generated output**: `build/generated/shaders.h`,
   `engine_api_suggestions.h`, a project's `CMakeLists.txt` and `.doriax/`, and the
   per-project `AGENTS.md` (emitted by `editor/Generator.cpp`). Project build settings go
   in `ProjectBuild.cmake`.
5. **No unit tests exist** (`ctest` is empty). A change is not done until the target
   builds with MSVC *and* an executable actually runs and prints something sane.
6. No new third-party dependency without a strong reason; deps are vendored on purpose.

## Skills

A 21-skill set is mirrored, byte-identical, into `.qoder/skills/`, `.opencode/skills/`,
`.agents/skills/` (Codex) and `.claude/skills/` (Claude Code). Open the matching
`SKILL.md` before working in an area, and if you correct a fact in one, update all four.

| Area | Skill |
|---|---|
| start here / any new capability | `doriax-feature-workflow`, `doriax-safety-rules` |
| compiling, CI parity, fast syntax checks | `doriax-build-verify` |
| rendering overview / GLSL / variants / cache | `doriax-rendering`, `doriax-shaders` |
| lights, shadows, PBR | `doriax-lighting` |
| screen effects, SSAO/SSR, adding a pass | `doriax-postprocess` |
| bloom / fog, smoke, god rays / water | `doriax-bloom`, `doriax-volumetrics`, `doriax-water` |
| heightmaps, elevation, splat / Perlin, fBm | `doriax-terrain`, `doriax-procedural-noise` |
| Box2D+Jolt, stepping, CCD, buoyancy | `doriax-physics` |
| Lua + C++ DPROPERTY scripts, input | `doriax-scripting` |
| adding a component/property, reflector | `doriax-api` |
| threads, pools, `MAX_*` caps, profiling | `doriax-performance` |
| in-game UI / ImGui editor panels | `doriax-ui`, `doriax-editor-dev` |
| retained UI: Slate/UMG/CommonUI (`engine/slate`) | `doriax-slate-ui` |
| game projects, scene YAML, templates | `doriax-game-projects`, `doriax-engine-core` |

## Build and verify

```bash
cmake -S . -B "$DORIAX_BUILD_DIR"
cmake --build "$DORIAX_BUILD_DIR" --target doriax-editor --config Debug
cmake --build "$DORIAX_BUILD_DIR" --target generate_shaders          # after ANY shaderlib edit
"$DORIAX_BUILD_DIR/Debug/doriax-resource-compiler.exe" shaders --project <dir> --backend opengl
"$DORIAX_BUILD_DIR/Debug/doriax-engine.exe" --project <dir> --window 1280x720
"$DORIAX_BUILD_DIR/Debug/doriax-build-generator.exe" generate --project <dir>
"$DORIAX_BUILD_DIR/Debug/doriax-reflector.exe" reflect --bindings engine/core/script/binding --out <hpp>
```

Key cache options: `API_BACKEND` (auto), `GRAPHIC_BACKEND` (`glcore|vulkan|metal`),
`DORIAX_SHARED`, `NATIVE_MENU`, `DORIAX_EDITOR_VERSION`. MSVC is authoritative — MinGW
accepts code MSVC rejects; `cl /Zs` with the include dirs harvested from
`build/doriax-editor.vcxproj` is the cheap pre-check (there is no `compile_commands.json`).

## Verified facts worth knowing before you answer a question

- **Lighting is forward.** `mesh.frag` lights the scene. The deferred classes exist but
  are never instantiated and load `deferred_lighting.{vert,frag}`, which do not exist.
- **"Declared" is not "implemented".** Bloom, tonemap, exposure, colour grading, DOF,
  TAA, FXAA, motion blur, volumetric/height fog, light shafts, VSM and SSGI have `.frag`
  files, enums and header declarations with **zero definitions and zero call sites**.
  Before saying a feature exists, look for the `RenderSystem::renderX` definition and its
  call in `draw()`. What does run: SSAO, SSR, depth pre-pass, per-pixel fog, fixed-res
  blit, the user post-process chain, CDLOD terrain, particles, PBR + shadows + probes.
- **No water system and no Perlin/simplex anywhere.** Terrain, fog and value noise exist
  (the last only editor-side, `editor/ai/AiTerrainCapability.cpp`).
- **There is no reflection registry.** Engine, editor, YAML serializer and code generator
  each keep hand-maintained parallel lists; `DORIAX_EDITOR_SOURCES` and
  `engine/CMakeLists.txt` are edited by hand. New `ComponentType` values must be
  *appended* (the ordinal is a mask bit) and `Signature` caps component types at 64.
- **`_HAS_EXCEPTIONS=0`** propagates PUBLIC from `engine/libs/joltphysics/CMakeLists.txt`
  into the engine and editor: nothing may throw across a task/future boundary or the
  process aborts. `HybridArray` is a fixed array in engine builds but resizes under
  `DORIAX_EDITOR`, so caps can pass in the editor and break a shipped game.
- Lua scripts dispatch **only** `init` automatically; `update`/`onKeyDown`/contacts must
  be subscribed (`RegisterEngineEvent`, or direct `Engine.onX = ...` assignment).
  `DPROPERTY` expands to `public:` plus a marker comment and is regex-parsed from your
  header, so it is not type-checked until code generation. Base class for C++ scripts is
  `doriax::ScriptBase` (`engine/core/script/ScriptBase.h:12`).

## Plans and open work

`TODO_ENGINE.md`, `RENDERING_UPGRADE_PLAN.md`, `UI_SYSTEM_PLAN.md`,
`VISUAL_SCRIPTING_PLAN.md` (Blueprint-style visual scripting spec, phases 0-9, not
started) and `roadmap/`. Read them before designing anything new.

The user writes in **pt-BR**; reply in Portuguese while keeping code, comments and
identifiers in English.
