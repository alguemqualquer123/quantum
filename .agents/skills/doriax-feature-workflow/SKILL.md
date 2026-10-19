---
name: doriax-feature-workflow
description: The professional end-to-end workflow for implementing a feature in doriax - orient, prove what already exists, pick the cheapest layer, plan before coding, hit every hand-maintained touchpoint, verify headless, and hand off visual checks. Use for any new capability: a component, a render pass, an editor window, a script API, a gameplay feature.
---

# Implementing a feature

## 1. Orient (5 minutes, saves days)

Read the plan documents at the repo root before designing anything:
`TODO_ENGINE.md`, `RENDERING_UPGRADE_PLAN.md`, `UI_SYSTEM_PLAN.md`,
`VISUAL_SCRIPTING_PLAN.md`, and `roadmap/`. Someone has probably already scoped the
request. Then load the relevant specialist skill:

| Task smells like | Skill |
|---|---|
| new component / setting / persisted field / enum | `doriax-api` |
| gameplay logic, Lua or C++ scripts, input | `doriax-scripting` |
| `.frag`/`.vert`/include, variant, shader cache | `doriax-shaders` |
| light, shadow, PBR term, IBL | `doriax-lighting` |
| screen effect, colour grade, AA, SSR/SSAO | `doriax-postprocess` |
| bloom / glow / HDR | `doriax-bloom` |
| fog / haze / smoke / god rays | `doriax-volumetrics` |
| water / buoyancy / reflection on a surface | `doriax-water` |
| heightmap / elevation / splat / LOD | `doriax-terrain` |
| Perlin / fBm / procedural generation | `doriax-procedural-noise` |
| jitter, tunnelling, mass, joints, contacts | `doriax-physics` |
| HUD/menu/widgets or an editor panel | `doriax-ui`, `doriax-editor-dev` |
| slow, stutter, deadlock, shutdown crash | `doriax-performance` |
| project/template/asset pipeline | `doriax-game-projects` |
| anything at all | `doriax-build-verify`, `doriax-safety-rules` |

## 2. Prove existence, don't assume it

This codebase is full of features that exist as a file and an enum but not as a
running pass. The test is a **definition plus a call site**, not a grep hit:

```bash
grep -rn "RenderSystem::renderBloom\|::loadBloom" engine/ --include=*.cpp   # 0 hits => not wired
grep -rn "registerComponent<" engine/core/registry/EntityRegistry.cpp       # what the ECS actually knows
```

Also check whether the behaviour you observe is coming from a **cache** rather than
from source (compiled shaders in `<usercache>/doriax/shaders/v23/*.sdat`, generated
`build/generated/shaders.h`, a project's `.doriax/`). Then report honestly: "declared
but unwired, here is the work", never "it exists, just enable it".

## 3. Pick the cheapest layer that satisfies the request

Ordered by cost, all legitimate:

1. **Project data / Lua** - a scene setting, a script, a forked `postprocess.frag`
   pass. No engine rebuild, no cache bump.
2. **Editor tooling** - a window or a generator (pattern: `editor/ai/AiTerrainCapability.cpp`).
3. **Engine API** - a new component/property through the 8-file checklist (`doriax-api`).
4. **New render pass / subsystem** - the 6-step built-in recipe (`doriax-postprocess`)
   plus a shader-cache version bump.

Prototype at layer 1 to agree the look, then promote. That is how bloom and volumetrics
should be approached here.

## 4. Plan, then write

For anything beyond a few lines, write the plan down first (the user asks for specs and
roadmaps; `VISUAL_SCRIPTING_PLAN.md` is the house format: current state, decisions with
rationale, phases with day estimates, cut list, headless verification, risks, and a
division of labour for agents). Include the touchpoint list from step 3 so nothing is
discovered mid-implementation.

## 5. Implement with the touchpoints closed

Every registry here is a hand-maintained parallel list, so after each edit re-ask
"where else is this type named?": engine registration, `engine/CMakeLists.txt` and the
Xcode `project.pbxproj`, `DORIAX_EDITOR_SOURCES`, `Catalog.h` (append, never insert -
the ordinal is a mask bit), `Catalog.cpp` (name/type/properties/fast resolver),
`Stream.cpp` encode + decode, `Factory.cpp`, `ComponentAddDialog`, the Lua binding, and
`engine_api_suggestions.h`. Missing one compiles and then silently does not appear.

Structural invariants that bite: uniform blocks are uploaded as contiguous bytes (do not
reorder), aggregate GLSL initializers must match member counts, `_HAS_EXCEPTIONS=0`
means nothing may throw across a task/future boundary, and `HybridArray` grows silently
in editor builds but not in game builds.

## 6. Verify, and be precise about what you verified

```bash
cmake --build "$DORIAX_BUILD_DIR" --target doriax-editor --config Debug   # one tree, MSVC
"$DORIAX_BUILD_DIR/Debug/doriax-resource-compiler.exe" shaders --out <dir> --backend opengl
"$DORIAX_BUILD_DIR/Debug/doriax-engine.exe" --project <dir> --window 1280x720
doriax-build-generator.exe status --project <dir>                          # stale generated files?
```

Then state the boundary out loud: "compiles and the variants build; the visual result
is unverified because I must not launch the editor - please check X in the scene." A
change is never reported as working on the strength of a successful build alone.

## 7. Hand off

Commit only your own files, with an explicit list (`doriax-safety-rules`), keep
generated artifacts out, and note in the message which parts remain visually unverified
and which follow-ups the feature needs (settings rows, docs, scene keys, migration for
legacy YAML names).
