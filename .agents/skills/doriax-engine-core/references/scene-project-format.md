# Scene and project file formats

Both sides of the format live in the editor: `editor/Stream.cpp` encodes/decodes
`*.scene`, `editor/Project.cpp` handles `project.yaml` (`loadProject`,
`saveProjectFile` ~:430-438). The engine only ever sees generated C++.

## `project.yaml`

Top-level keys written by `Project.cpp`: `name`, `nextSceneId`, `selectedScene`,
canvas size fields, `scalingMode`, `textureStrategy`, `vsync`/window flags,
`luaDir`, `startSceneId`, `terrainEditor{}`, `tabs[]`, `scenes[]` (per-scene view
flags + `editorCamera`), and `standaloneBundles`.

## `scenes/*.scene`

YAML, one document per scene (see `templates/fps-fps/scenes/main.scene`):

```yaml
id: 1
name: Main Scene
scene:              # keys mirror SceneSettings field names, camelCase
  backgroundColor: [0.25, 0.45, 0.65, 1]   # sRGB, float4
  shadowQuality: low                        # enum written as lowercase name
  ssaoEnabled: false
  gravity3D: [0, -9.81, 0]
sceneType: scene_3d
mainCamera: 1002
maxValues: {...}
shaderKeys: {...}
entities:
  - type: Entity
    entity: 1001                # user ids start at 1001; 1-1000 are system entities
    name: Sun
    components:
      light:                    # key = component's catalog name
        type: directional
        intensity: 4
        shadows: true
```

Rules to respect when changing the format:

- A scene-setting key is the camelCase form of the `SceneSettings` field; the pair
  `engine/core/SceneSettings.h` (defaults) and `editor/Stream.cpp` (codec) is the
  single source of truth. New key => default in `SceneSettings`, encode + decode in
  `Stream.cpp`, and decode must tolerate the key being absent.
- Vectors/colors are YAML flow sequences of floats; enums are lowercase names.
- Changing an existing key's meaning silently breaks every project on disk - prefer
  adding a new key and keeping the old one loadable.
- Generated C++ (`.doriax/generated/`, project `CMakeLists.txt`) is derived output:
  never hand-edit; regenerate by running the scene in the editor or via
  `doriax-build-generator generate`.
