<!-- (c) SR VINIX and contributors -->
<!-- SPDX-License-Identifier: MIT -->

# Phase 3 decal fixture

This is a small editor-authored scene whose `DecalTarget` entity contains a
serialized `DecalComponent`. The target is an inline four-vertex ground plane;
the decal is an untextured, opaque red projection so the default white decal
texture is enough to make the pass observable.

## Authored contents

- `project.yaml` registers `scenes/main.scene` as scene 1.
- `scenes/main.scene` contains a camera, directional light, receiver mesh and
  an explicit `decal:` component with non-default size, opacity and depth fade.
- `lua/main.lua` is the runtime counterpart used by the headless capture path;
  it creates the same receiver, camera, light and `Decal` object through the
  public Lua API.

## Validation

From the repository root:

```powershell
& .\build\Debug\quantum-editor-cmd.exe doctor --project .\tests\phase3\decal_fixture
python .\scripts\phase3_visual.py `
  --project .\tests\phase3\decal_fixture `
  --engine .\build\Debug\quantum-engine.exe `
  --resource-compiler .\build\Debug\quantum-resource-compiler.exe `
  --backend opengl --feature decals --artifact-dir $env:TEMP\phase3-decal-fixture
```

The doctor command validates the authored YAML and must not report an error for
the fixture. The visual command compiles the `decal` shader and invokes the
existing runtime capture path.

## Runner boundary

The runner still executes `lua/main.lua` instead of loading the editor `.scene`
file directly. The YAML remains the serialized editor fixture and the Lua file
is deliberately kept equivalent, so the capture is now a real runtime proof of
the decal pass rather than a shader-only check.
