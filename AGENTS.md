# Quantum — guide for coding agents

C++17 game engine (`QUANTUM_CXX_STANDARD 17`, `engine/CMakeLists.txt:5`) + Dear ImGui editor, MIT.
Quantum is maintained by SR VINIX; it is a fork of the Quantum engine by SR VINIX, and the
`quantum` identifiers below are the upstream names the code rename has not replaced yet.
Layout: `engine/` (runtime lib, target `quantum`), `editor/` (the editor app),
`templates/` (sample game projects), `tools/` (headless executables),
`engine/libs/` + `libs/` (vendored deps: sokol, imgui, Lua/LuaBridge, box2d, Jolt,
glslang/SPIRV-Cross, yaml-cpp, tinygltf, stb, sheenbidi, harfbuzz, and `libs/uilab` —
the editor's own UI component library, pristine upstream + `quantum-patches/`, so edit it
only through `scripts/sync-uilab.sh regen`), `scripts/`,
`.github/workflows/cmake.yml` (authoritative CI job is MSVC).

Every file needs the `// (c) SR VINIX` + `SPDX-License-Identifier: MIT` header.
`namespace quantum`; classic include guards in `engine/`, `#pragma once` in `editor/`;
4-space indent; comments in English and only where the *why* is non-obvious.

## Hard rules (violating these caused real damage here)

1. GUI validation with `quantum-editor.exe` is allowed when the user explicitly requests
   it. Never kill the user's running editor without asking. Its autosave previously
   rewrote a template scene into ~18,870 bad lines, so prefer headless checks when they
   are sufficient and avoid saving or mutating user scenes during validation. A render
   change still requires a real frame: `quantum-engine.exe --screenshot PATH
   --capture-frame N` (or `Engine.screenshot` from Lua) writes a PNG of the presented
   frame.
2. **One build tree**: `$QUANTUM_BUILD_DIR`, default `build`. Do not create
   `build-debug/`, `build-ninja/`, etc. to dodge a lock; two concurrent
   `cmake --build` runs in one tree fail with misleading `C1041`/PDB errors.
3. **Never `git add -A` / `git add .`.** This tree routinely holds concurrent agents'
   in-flight edits. Stage explicit file lists, review `git status` after staging, and do
   not commit work you did not write. Never commit run artifacts (`stderr.txt`,
   `stdout.txt`, `build_editor.log`, `templates/*/{editor.log,settings.yaml,editor_layout.ini}`).
4. **Never hand-edit generated output**: `build/generated/shaders.h`,
   `engine_api_suggestions.h`, a project's `CMakeLists.txt` and `.quantum/`, and the
   per-project `AGENTS.md` (emitted by `editor/Generator.cpp`). Project build settings go
   in `ProjectBuild.cmake`.
5. **No unit tests exist** (`ctest` is empty). A change is not done until the target
   builds with MSVC *and* an executable actually runs and prints something sane.
6. No new third-party dependency without a strong reason; deps are vendored on purpose.
7. **`templates/voxelworld-main/minecraft-assets-26.3/` is Mojang's copyright.** It is a local
   input the asset tools read, never a committed file and never a derived asset either (a stitched
   atlas is the same copyright as its source). The project's own procedural generators stay the
   shipped art. See `quantum-voxel-assets`.

## Skills

A 25-skill set is mirrored, byte-identical, into `.qoder/skills/`, `.opencode/skills/`,
`.agents/skills/` (Codex) and `.claude/skills/` (Claude Code). Open the matching
`SKILL.md` before working in an area, and if you correct a fact in one, update all four.

| Area | Skill |
|---|---|
| start here / any new capability | `quantum-feature-workflow`, `quantum-safety-rules` |
| compiling, CI parity, fast syntax checks | `quantum-build-verify` |
| rendering overview / GLSL / variants / cache | `quantum-rendering`, `quantum-shaders` |
| lights, shadows, PBR | `quantum-lighting` |
| screen effects, SSAO/SSR, adding a pass | `quantum-postprocess` |
| bloom / fog, smoke, god rays / water | `quantum-bloom`, `quantum-volumetrics`, `quantum-water` |
| heightmaps, elevation, splat / Perlin, fBm | `quantum-terrain`, `quantum-procedural-noise` |
| Box2D+Jolt, stepping, CCD, buoyancy | `quantum-physics` |
| Lua + C++ DPROPERTY scripts, input | `quantum-scripting` |
| adding a component/property, reflector | `quantum-api` |
| generating/verifying the Lua API reference | `quantum-lua-api` |
| threads, pools, `MAX_*` caps, profiling | `quantum-performance` |
| in-game widgets: HUD, modal, button, text field, font | `quantum-game-ui` (`engine/core/object/ui`) |
| editor ImGui panels, docking, UILab chrome | `quantum-ui`, `quantum-editor-dev` |
| retained UI: Slate/UMG/CommonUI (`engine/slate`) | `quantum-slate-ui` |
| game projects, scene YAML, templates | `quantum-game-projects`, `quantum-engine-core` |
| ready-made Lua gameplay scripts + demos | `quantum-scripting`, `quantum-game-projects` |
| the voxel project's Mojang asset pack (licence boundary) | `quantum-voxel-assets` |

## Build and verify

```bash
cmake -S . -B "$QUANTUM_BUILD_DIR"
cmake --build "$QUANTUM_BUILD_DIR" --target quantum-editor --config Debug
cmake --build "$QUANTUM_BUILD_DIR" --target generate_shaders          # after ANY shaderlib edit
cmake --build "$QUANTUM_BUILD_DIR" --target generate_lua_docs         # regenerate build/generated/lua_api.md
"$QUANTUM_BUILD_DIR/Debug/quantum-resource-compiler.exe" shaders --out <dir> --backend opengl --shader <spec>
"$QUANTUM_BUILD_DIR/Debug/quantum-engine.exe" --project <dir> --window 1280x720 \
    --screenshot /tmp/frame.png --capture-frame 120          # PNG of the real presented frame
"$QUANTUM_BUILD_DIR/Debug/quantum-build-generator.exe" generate --project <dir>
"$QUANTUM_BUILD_DIR/Debug/quantum-reflector.exe" reflect --bindings engine/core/script/binding --out <hpp>
```

Key cache options: `API_BACKEND` (auto), `GRAPHIC_BACKEND` (`glcore|vulkan|metal`),
`QUANTUM_SHARED`, `NATIVE_MENU`, `QUANTUM_EDITOR_VERSION`. MSVC is authoritative — MinGW
accepts code MSVC rejects; `cl /Zs` with the include dirs harvested from
`build/quantum-editor.vcxproj` is the cheap pre-check (there is no `compile_commands.json`).

## Verified facts worth knowing before you answer a question

- **Lighting is forward.** `mesh.frag` lights the scene. The deferred classes exist but
  are never instantiated and load `deferred_lighting.{vert,frag}`, which do not exist.
- **"Declared" is not "implemented".** TAA, motion blur, volumetric/height fog,
  light shafts, VSM and SSGI have `.frag` files, enums and header declarations with
  **zero definitions and zero call sites**.
  Before saying a feature exists, look for the `RenderSystem::renderX` definition and its
  call in `draw()`. What does run: SSAO, SSR, depth pre-pass, DOF (CoC + half-res separable
  blur + composite, `quantum-postprocess`), per-pixel fog, fixed-res
  blit, the user post-process chain, CDLOD terrain, particles, PBR + shadows + probes,
  the water surface and its shallow-field simulation, and the HDR chain (bloom threshold +
  mip down/upsample + composite with lens dirt, eye adaptation, tone map, colour grading)
  plus FXAA as the last stage of the frame.
- **Water is a system now.** `WaterSystem` steps a CPU shallow-water heightfield per
  `WaterComponent` - it runs downhill, pools, takes rain, evaporates, ripples - the `Sim`
  shader variant displaces the drawn surface by it, and a floating body is carried by the
  current. There is still **no fluid solver and no 2D water**. See `quantum-water`.
- **Terrain generation is in the engine now.** `engine/core/util/TerrainGenerator.{h,cpp}`
  writes a 16-bit grayscale heightmap from `quantum::Noise` (fbm + ridged multifractal + domain
  warping) plus hydraulic and thermal erosion (`TerrainErosion`, `TerrainMapRegion`, `PngWriter`
  all moved out of `editor/util/`), so a script, a plugin, a CLI verb and the editor brush produce
  the same bytes for the same seed. Three ways in: `WorldGen.generate/presets/params/waterHeight`
  in Lua, `quantum-editor-cmd terrain presets|generate|info`, and `Terrain` in a scene. Proof harness:
  `tests/worldcreator/probe` (48 headless checks + a captured PNG of the real island). API doc:
  `WORLD_CREATOR_TERRAIN.md`. See `quantum-terrain` and `quantum-procedural-noise`.
- **There is no reflection registry.** Engine, editor, YAML serializer and code generator
  each keep hand-maintained parallel lists; `QUANTUM_EDITOR_SOURCES` and
  `engine/CMakeLists.txt` are edited by hand. New `ComponentType` values must be
  *appended* (the ordinal is a mask bit) and `Signature` caps component types at 64.
- **`_HAS_EXCEPTIONS=0`** propagates PUBLIC from `engine/libs/joltphysics/CMakeLists.txt`
  into the engine and editor: nothing may throw across a task/future boundary or the
  process aborts. `HybridArray` is a fixed array in engine builds but resizes under
  `QUANTUM_EDITOR`, so caps can pass in the editor and break a shipped game.
- Lua scripts dispatch **only** `init` automatically; `update`/`onKeyDown`/contacts must
  be subscribed (`RegisterEngineEvent`, or direct `Engine.onX = ...` assignment).
  `DPROPERTY` expands to `public:` plus a marker comment and is regex-parsed from your
  header, so it is not type-checked until code generation. Base class for C++ scripts is
  `quantum::ScriptBase` (`engine/core/script/ScriptBase.h:12`).
- **`engine/components/` ships 59 ready-made Lua gameplay scripts** (camera, controllers,
  rope/slingshot/gravity, water waves/shore/float/swim, waterfall, day-night with a real moon,
  clouds, weather, wind, fire that spreads
  to burnable fuel, spawner/projectile, avatar parts hung on a bone plus a hair spring,
  HUD, audio, particles, screen effects) plus `demos/` with ten headless-runnable scenes — one of
  them (`demo_sky`) a procedural sky fork shipped precompiled, because the runtime never compiles
  a shader. New
  projects are seeded automatically and existing ones by
  `quantum-editor-cmd components install --project <dir>`. Before writing a gameplay script or
  proposing a new component type, check that library - and read `engine/components/README.md`
  for the binding traps it documents (the Lua wrapper owns its entity; engine values arrive as
  userdata, not tables; `Body3D` exposes persistent linear/angular damping; `Joint3D` is not `deriveClass`-ed, so Lua
  cannot create a joint).

## Plans and open work

`TODO_ENGINE.md`, `RENDERING_UPGRADE_PLAN.md`, `UI_SYSTEM_PLAN.md`,
`VISUAL_SCRIPTING_PLAN.md` (Blueprint-style visual scripting spec, phases 0-9, not
started) and `roadmap/`. Read them before designing anything new.

The user writes in **pt-BR**; reply in Portuguese while keeping code, comments and
identifiers in English.
