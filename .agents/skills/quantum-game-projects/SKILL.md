---
name: quantum-game-projects
description: Work on Quantum game projects and templates (YAML scenes, Lua scripts, C++ DPROPERTY scripts, ProjectBuild.cmake, exported builds) rather than engine or editor code. Use when creating or editing a project under templates/, writing gameplay logic in Lua/C++, running a project headless, or debugging generated build files.
---

# Quantum game projects

A Quantum *project* is data + scripts: `project.yaml`, `scenes/*.scene`, `scripts/`,
`assets/`. The C++ the game actually compiles is generated from it - never hand-edit
generated output.

## Anatomy

```
project.yaml        # name, canvas, scalingMode, textureStrategy, vsync/window*,
                    # luaDir, startSceneId, terrainEditor{}, tabs[], scenes[], standaloneBundles
scenes/main.scene   # id/name/scene{...}/sceneType/mainCamera/maxValues/shaderKeys/entities[]
scripts/*.lua       # attached through ScriptComponent; self.scene / self.entity available
scripts/*.h|.cpp    # C++ scripts: classes deriving quantum::ScriptBase with DPROPERTY fields
CMakeLists.txt      # GENERATED every build - do not edit
ProjectBuild.cmake  # the hook for your own build settings; never overwritten
.quantum/            # generated/: scene + bundle factories, main.cpp, scene_scripts.cpp,
                    # engine-api/ snapshot, thumbs/
AGENTS.md           # auto-generated project brief (Generator.cpp ~:1109)
```

Read the project's own `AGENTS.md` first - it is generated and states the exact local
engine API snapshot path for that project.

## Editing rules

- Any change to `*.scene`, `*.bundle` or `project.yaml` invalidates the generated C++:
  regenerate by playing/running in the editor, or headless with
  `quantum-build-generator generate --project <dir>` (then `configure` + `build`).
- Put custom compiler flags, includes or link libraries in `ProjectBuild.cmake`, using
  `${QUANTUM_TARGET}` and `${QUANTUM_SCRIPTS_DIR}` instead of literal names - the target
  name differs between the in-editor build and the exported one.
- Entity ids: 1-1000 are reserved for system entities, user entities start at 1001.
- Enum values in YAML are written lowercase (`shadowQuality: low`); colors/vectors are
  flow sequences (`[0.25, 0.45, 0.65, 1]`, sRGB).

## Running and building without the editor

`$QUANTUM_BUILD_DIR` (default `build`) is the one build tree; see `quantum-build-verify`.

```bash
"$QUANTUM_BUILD_DIR/Debug/quantum-engine.exe" --project templates/fps-fps --window 1280x720
quantum-build-generator.exe status   --project <dir>     # stale generated files? exit!=0
quantum-build-generator.exe generate --project <dir>
quantum-build-generator.exe build    --project <dir> --config Release
quantum-resource-compiler.exe pak    --project <dir> --out game.pak
```

Limits worth knowing: `resources.pak` is compiled out of the editor-config dll build,
and `.scene` YAML deserialization is editor-side (projects run through generated C++
or Lua entries), so `quantum-engine --project` currently executes Lua entries only - the
runner logs this explicitly rather than failing silently.

## Lua vs C++ scripts

- Full hook/VFS/hot-reload detail is in `quantum-scripting`; gameplay-relevant limits
  (physics components, water, procedural noise) are in `quantum-physics`,
  `quantum-water` and `quantum-procedural-noise`.
- Lua is the fast path for gameplay; API surface is what `engine/core/script/binding/*.cpp`
  exposes, and `engine_api_suggestions.h` (regenerate with `quantum-reflector`, see
  `quantum-build-verify`) is the completion list - use it to confirm a method exists
  before writing code against it.
- C++ scripts need `DPROPERTY("Name", Type)` on each exposed field so the editor's
  `ScriptParser` can offer it in the inspector, and are hot-reloaded through the
  project shared lib (`Conector` looking up `initScripts`).
- No engine internals are available to project code: if a feature needs new engine API,
  that is a `quantum-engine-core` task (binding + reflector regeneration).

## The shipped component library beats writing gameplay from scratch

`engine/components/` carries 59 ready-made Lua gameplay scripts (camera, controllers, physics -
rope/slingshot/falling/gravity zone/grabber - water: waves, shore, float, swim, ripples; day/night,
weather, wind, fire + burnable fuel, spawner/projectile/pool, health/pickup/trigger/HUD, audio,
particles, screen fade; avatar part on a bone + hair spring)
plus `demos/` with eight runnable scenes. A **new** project is seeded with all of them by
`Project::installReadyMadeComponents`; an **existing** project gets them with
`quantum-editor-cmd components install --project <dir>` into `<luaRoot>/Components`, with the demos
beside it in `<luaRoot>/demos`. `engine/components/README.md` is the reference (slots, recipes,
public methods, and the twenty-seven binding traps proved while building it).

The rest of the engine folder is consumable the same way - `engine/assets` (stock material maps)
and `engine/shaderlib` (the GLSL a project forks). `quantum-editor-cmd assets list` shows what is
there and how much it weighs, `assets find <text>` locates one file by name, and
`assets copy <root>/<path> --project <dir>` delivers it: a `.lua` into `<luaRoot>/Components`
(where the *New component* dialog looks), anything else into `<project>/assets/engine/<root>/`,
so a scene references the copy as `engine/assets/<file>` rather than the bare engine path.
Nothing ever hand-copies out of the build tree.

They are files, not registrations: each is attached as a `Lua Script` in the Inspector and its
`properties` table builds the rows, with entities dragged into slots. So copy what you need into
your own `scripts/` and edit - nothing breaks if you rename or delete one, and no engine rebuild is
involved. Run a demo headless to see them wired:
`KIT_DEMO_FRAMES=300 quantum-engine.exe --project <dir> --scene demos/demo_beach --window 1280x720`
(the runner boots from `<project>/lua/`, ignoring `luaDir` - `quantum-build-verify`).

## Templates as seeds

`templates/` holds complete sample projects: `platformer-2d`, `fps-fps`, `moba-click`,
`charactercontrol-main`, `third-person`, `voxelworld-main`, `mobile-game`. Copy one and
rename in `project.yaml` rather than starting from scratch. They also carry stray
`editor.log` / `settings.yaml` / `editor_layout.ini` from local runs - do not commit
changes to those.

## Scene YAML facts that cost time to rediscover

- An attribute row writes **`elements` before `offset`** (`dataType, bufferName,
  elements, offset, count`). A text tool that assumes the reverse silently matches nothing.
- Quoting is not uniform: `third-person` and `moba-click` write `name: "Sun"` and
  `- type: "Entity"`, `fps-fps` writes them bare. Any regex over templates must make the
  quotes optional or it will walk off the end of one project.
- A scene's `mesh: buffer.data` is a base64 vertex buffer, and the templates' primitive
  geometry is **inline, not generated** - so a template cube can be malformed and stay
  malformed silently. The `Ground` of all three 3D templates was an *open* box: floor at
  local y=-0.5, four walls, a duplicated front wall and **no top face**, with the normal
  array shifted one face and written at length 0.5. The sun-facing surface therefore
  carried a normal pointing away from the sun, `N·L < 0`, and the ground received **zero
  direct light** - it was lit only by sky IBL, which is what read as a blue cast. With
  `position: [0,-0.5,0]` and `scale: [50,1,50]` the visible plane also sat at world
  y=-1.00 while every prop rested at y≈0.
  Two rules follow: `p_punctual = hasLights` is shared by every mesh
  (`RenderSystem.cpp:3010-3049`), so a *per-mesh* direct-light difference can only be
  geometric, never a variant bit - and when normals disagree with winding on a mesh that
  is not closed, "orient away from the centroid" is the wrong repair. Rebuild the
  geometry.
- Every prop mesh in these templates is a box centred on its own origin spanning
  `[-h, +h]`, and the entity's `position.y` equals exactly its half-height. That pairing
  is what keeps props touching the ground; break either side and everything hovers.
- `Mesh of 'X' has failed to load 8 times, retrying` is deliberate pacing
  (`RenderSystem.cpp:10396-10411`: `STUCK_ATTEMPTS` warns once, then backs off to
  `RETRY_FRAMES`), not missing geometry - those meshes do render.
- **`meshType` is not a key.** `Stream::decodeMeshComponent` reads `buffer`, `indices`,
  `eBuffers`, `submeshes`, `bonesMatrix`, `morphWeights` - never a primitive descriptor, so
  `mesh: {meshType: box, path: ""}` loads an *empty* mesh: no error, no geometry, no log.
  Author real bytes with `python scripts/gen_primitive_yaml.py box --tile 1.0
  --world 20 1 1 --default-material --indent 6` (it transcribes
  `MeshSystem::createBox`: 48-byte stride, position/texcoord1/normal/color, face order
  front/back/left/right/top/bottom, and emits its own `mesh:` key at `--indent`).
- **The payload's parent matters more than its bytes.** The same block pasted under
  `transform:` instead of `mesh:` parses into valid YAML, decodes nothing and is invisible
  to a check that only reads `components.mesh`. `submeshes`/`eBuffers` are the mesh-only
  keys (`Stream.cpp:5115-5127`); `buffer`/`indices` alone are not a clue, `UIComponent`
  owns a glyph buffer with the same names.
- **Ortho depth is `[-far, -near]`, not `[near, far]`.** `Matrix4::orthoMatrix`
  (`:682-704`) maps view z that way, so a 2D camera at z=10 with `nearClip: -1,
  farClip: 1` puts every entity at z=0 at ndc z=+10 and clips the whole scene. Perspective
  uses a different formula (`:709-733`, divides by `-viewZ`). Check both this and that
  `mainCamera:` names the entity id that actually holds the `camera:` component.
- **Tiling lives in the UV attribute, per face.** `Material` has no scale key (only
  `heightScale` and the per-map `*TexCoord` slot) and the transform never touches UVs, so
  a stretched map is only fixable in the vertices. A face's u and v follow specific world
  axes (`±Z` faces: x,y; `±X`: z,y; `±Y`: x,z) and every face of one box must come out at
  the same metres-per-repeat - a 40×1×40 ground with UV 0..25 on all six faces reads fine
  on top and 40× squashed on the skirt.
- `python scripts/check_template_scenes.py` asserts all four of the above across
  `templates/*` headless (prints how many inline meshes it actually decoded, so a vacuous
  pass is visible; exit 1 on any finding). Run it after any hand edit to a `.scene`.

## Asset pack traps

`engine/assets/materials/ground` ships twelve tile sets, and **five of their height maps
were a constant 255 fill** (asphalt, grass, grass-leafy, mud-leaves, pebbles). A flat
height map is not merely "no relief": the parallax pass still runs and offsets every UV
by a constant, so the texture is quietly shifted while costing the full relief loop.
Check `height.png.std() < 1` before believing a material has height, and note that a
material with no relief can never show a difference in an A/B of `heightScale` - those
measurements are void, not negative results.

A project does **not** read the engine pack. Each 3D template carries its own copies
under `templates/<project>/materials/{default,ground}/`, and the scene YAML names those
relative paths - repairing `engine/assets/materials/**` leaves every project broken.
Re-run any asset fix against the per-project copies and check them separately. Two of
them (`fps-fps` ground-asphalt and `moba-click` ground-grass height maps) were flat
*and* 16-bit grayscale; `third-person` shipped a correct 8-bit cobblestone, so the pack
was inconsistent between projects, not uniformly broken.

Read the PNG IHDR, not the PIL mode: `TextureData::loadTexture` (`:207-237`) keeps a
**single-channel 16-bit** image at `ColorFormat::RED16` and downconverts everything else
to 8-bit RGBA, so colour type 0 + bit depth 16 is the only asset that reaches the engine
as 16-bit. `SokolTexture::generateMipmaps` historically recognised only RGBA8/BGRA8/R8
and answered anything else with `Undefined pixel format to generate mipmaps` plus a
**mipmap-less** texture - silent minification aliasing on a height or normal map.
That message never reaches stdout on the `benchmark` path (the `.log` is shader-cache
lines plus JSON), so it is only recoverable from a screenshot of the editor Output panel.
