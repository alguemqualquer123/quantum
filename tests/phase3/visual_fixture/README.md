<!-- (c) SR VINIX -->
<!-- SPDX-License-Identifier: MIT -->

# Phase 3 visual fixture

This is the deterministic runtime scene used by `scripts/phase3_visual.py`.
It contains static geometry, a fixed camera, directional shadows and a small
point-light fill so the capture compares renderer output instead of camera
animation or scene loading time.

The fixture intentionally keeps post-process toggles at their neutral runtime
values. The harness selects one render feature at a time with `--render-feature`
and captures the real presented frame. Identity and cinematic LUTs are generated
by the harness under its disposable artifact directory.

## Validation

From the repository root:

```powershell
python .\scripts\phase3_visual.py `
  --project .\tests\phase3\visual_fixture `
  --engine .\build\Release\quantum-engine.exe `
  --resource-compiler .\build\Release\quantum-resource-compiler.exe `
  --backend opengl `
  --feature taa --feature motionblur --feature hdr --feature lut `
  --feature clustered --feature ssgi --feature ssr `
  --feature atmosphere --feature shadows `
  --artifact-dir $env:TEMP\phase3-visual-fixture `
  --update-baselines
```

Run the same command without `--update-baselines` to compare a later capture.
Backends without a local runtime can still be included to exercise their shader
matrix; pass their executable through `--engine-map` when available.
