# Quantum Phase 3 visual validation

The visual harness runs outside the repository's build tree and never starts
`quantum-editor.exe`. It uses the real presented-frame path in
`quantum-engine --screenshot`, compiles shader variants with
`quantum-resource-compiler`, and stores PNGs, logs, JSON and HTML reports under
the system temporary directory.

## First OpenGL run

```powershell
python scripts/phase3_visual.py `
  --project tests/phase3/visual_fixture `
  --engine build/Debug/quantum-engine.exe `
  --resource-compiler build/Debug/quantum-resource-compiler.exe `
  --backend opengl `
  --update-baselines
```

The first run creates baselines. A later run compares each captured frame and
returns non-zero when the mean RGB error exceeds the threshold:

```powershell
python scripts/phase3_visual.py `
  --project tests/phase3/visual_fixture `
  --engine build/Debug/quantum-engine.exe `
  --resource-compiler build/Debug/quantum-resource-compiler.exe `
  --backend opengl `
  --threshold 0.01
```

The default artifact directory is `%TEMP%\quantum-phase3-visual`. It contains
one project copy per backend, shader compiler logs, engine logs, captured
`current.png`, `diff.png`, `compare.json`, `phase3-visual-report.json` and
`phase3-visual-report.html`. Nothing is written to the source fixture
`tests/phase3/visual_fixture`.

## Backend matrix

Shader compilation can cover every supported target in one run:

```powershell
python scripts/phase3_visual.py `
  --project tests/phase3/visual_fixture `
  --engine build/Debug/quantum-engine.exe `
  --resource-compiler build/Debug/quantum-resource-compiler.exe `
  --backend opengl --backend opengles --backend d3d11 `
  --backend metal-macos --backend metal-ios --backend vulkan
```

Runtime capture is performed only for OpenGL unless another executable is
explicitly mapped. This prevents a Vulkan/Direct3D label from accidentally
using an OpenGL-built runner:

```powershell
python scripts/phase3_visual.py ... `
  --backend opengl --backend vulkan `
  --engine-map vulkan=build-vulkan/Debug/quantum-engine.exe
```

The script does not configure or build another tree. Build each backend in its
own checkout/build environment before supplying its runner, following the
project's one-build-tree rule.

## Feature overrides

`quantum-engine` accepts repeatable `--render-feature` flags so the validation
does not require editor-authored toggles:

```powershell
quantum-engine.exe --project tests/phase3/visual_fixture --window 640x360 `
  --render-feature taa --render-feature motionblur `
  --render-feature hdr --render-feature lut `
  --render-lut textures/phase3_identity_lut.png `
  --screenshot "$env:TEMP\phase3.png" --capture-frame 60
```

The current slice enables TAA, motion blur, HDR/LUT, SSGI, SSR, atmosphere/fog
and shadow overrides. Clustered/tiled selection remains backend-driven; OpenGL
uses its documented compatibility fallback. The stable runtime capture uses
`tests/phase3/visual_fixture`, whose static camera and geometry keep shadow and
temporal comparisons deterministic. Decal capture uses
`tests/phase3/decal_fixture`: the YAML is the editor-authored serialization
fixture and `lua/main.lua` builds the equivalent runtime scene so the harness
captures the real projected-decal pass. The shader matrix also includes
`mesh:Ult,Vc4`, required by `Shape:createPlane` in that fixture.

The engine-side comparison also supports direct use of:

```text
--compare-screenshot BASELINE.png
--compare-threshold 0.01
--compare-diff DIFF.png
--compare-report REPORT.json
```

The comparison is RGB mean error over equal-size PNGs. The diff image is a
false-colour absolute difference, and the JSON report includes dimensions,
mean/max error, differing-pixel ratio, threshold and pass/fail.

## Verified matrix on 2026-11-10

The local MSVC run compiled all requested shader families successfully for
OpenGL, OpenGLES, D3D11, Metal macOS, Metal iOS and Vulkan. The OpenGL runtime
captured and compared the nine stable-fixture cases for TAA, motion blur, HDR,
LUT, clustered, SSGI, SSR, atmosphere and shadows; all nine repeat comparisons
passed. The LUT case uses the dedicated generated `phase3_identity_lut.png`
asset, and the repeat capture measured zero mean/max error and zero
differing pixels. The other backends remain shader-only until a matching
runtime executable is supplied. Linux's native OpenGL backend now has the same
presented-frame readback path as Windows; the desktop workflow runs the static
fixture under Xvfb on the Linux runner so this path is exercised in CI. macOS
Metal now copies the presented `CAMetalDrawable` through the Sokol command queue
into a shared buffer and converts BGRA to RGBA for PNG capture. Vulkan now adds
`VK_IMAGE_USAGE_TRANSFER_SRC_BIT` when the surface supports it, copies the
acquired swapchain image through a host-visible staging buffer, restores the
presentation layout and converts BGRA/RGBA to PNG rows. Native Vulkan execution
and per-GPU baselines remain pending.

## Follow-ups

- Keep the visual and decal fixture baselines stable on each supported GPU/driver and add
  explicit pixel probes for projection bounds, depth fade and texture alpha.
- Supply mapped Vulkan/D3D11 runtime binaries and establish stable baselines per
  GPU/driver rather than comparing different hardware as pixel-identical. The
  Vulkan runtime copy is now implemented, but still needs execution on a Vulkan
  runner and qualification of surfaces that do not expose transfer-source usage.
- Add explicit pixel probes for clustered occupancy, SSR probe fallback and
  shadow cascade stability; system timing alone cannot prove those visual
  properties.
- Keep volumetric/froxel fog and full material-extension validation separate;
  this harness reports the current connected atmosphere path without implying
  that the unfinished froxel implementation exists.
