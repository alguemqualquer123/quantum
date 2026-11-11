---
name: quantum-bloom
description: Implement or debug bloom in Quantum - the HDR chain (threshold, mip downsample/upsample, composite, tone map) now runs in RenderSystem. Use when the user asks for bloom, glow, HDR emission, light bleeding, bright-pixel thresholding, mip downsample/upsample chains, lens dirt, or asks why a scene with bloomEnabled looks unglowing in an exported build.
---

# Bloom

## Status: it runs, but it is glare from an LDR source

`RenderSystem::loadHDR` (`RenderSystem.cpp:5351`) loads six passes and
`RenderSystem::renderHDR` (`:5601`) draws them, in this order, after the scene color pass:
`bloom_threshold.frag` -> `bloom_downsample.frag` per mip -> `bloom_upsample.frag`
accumulating from the smallest level up -> `bloom_composite.frag` into `hdrFramebuffer`
(RGBA16F, `ensureHDRFramebuffers:5496`) -> `exposure.frag` into a 1x1 ping-pong pair
(`ensureExposureFramebuffers:5576`) -> `tonemap.frag` into the destination.
Framebuffers are allocated once and rebuilt on size change (`activeBloomMipLevels`), and a
headless run proves the chain exists: `Log::verbose("HDR chain ready: %d bloom levels at %ux%u")`
(`:5568`) plus `Log::verbose("eye adaptation ready: 1x1 meter over two ping-pong targets")`
(`:5596`), and one `Log::verbose("HDR features: bloom %d tone map %d grading %d auto exposure %d")`
(`:5616`) *only when the set changes* - that last one is the headless proof channel, since a frame
loop that printed it would flood the log.

The scene target is still **RGBA8/sRGB** - `mesh.frag` ends in `linearTosRGB` and no HDR mesh
variant is compiled - so the chain decodes on entry (`srgbSource = 1.0f`, `:5624`) and the tone
map re-encodes on exit. That is why the look is *glare over bright LDR pixels*, not true
emission: nothing in the scene can write above 1.0, so `bloomThreshold` only separates lit from
unlit. Real HDR bloom needs an emissive term in `mesh.frag`/`pbr.glsl` writing into a float
scene target first.

## The gate that silently turns it off

`renderHDR` only runs when the pass can capture the destination, and the condition is written
twice - keep both in sync or an exported build gets no bloom while the editor does:

- draw time (`RenderSystem.cpp:10355`): `isMainCamera && scene->isHdrEnabled() && !useSSR &&
  (Engine::getFramebuffer() || camera.renderToTexture || useFixedRes || swapchainRedirect)`
- `RenderSystem::updateSwapchainRedirect()` (`:9149`) must contain the HDR branch
  (`loadHDR()` + `ensureSceneColorFramebuffer` + `ensureHDRFramebuffers`), because that flag is
  what makes a plain windowed game render offscreen at all. **Every new offscreen effect has to
  be added there** - the chain was invisible in shipped builds until that branch existed.

`Scene::isHdrEnabled()` (`engine/core/Scene.cpp:550`) is true when **any** of bloom, the tone map,
colour grading or auto exposure is on - the last two because they are applied *by* the tone map, so
a scene that asks for a grade needs the chain even with bloom off. It is evaluated per frame, which
is what lets a runtime preset toggle take effect with no reload. The chain is **not combined with
SSR**: SSR composites into the destination, which is the buffer the bloom chain would read back into
itself. With SSR on, HDR is skipped - that is the reason `hdrEnabled` excludes it rather than
`useSSR` winning later.

## What is wired now (was dead data before this landed)

- **Colour grading** rides the tone map: `tonemap.frag` carries `grade` (saturation / contrast /
  brightness / hue) and `grade2` (temperature / tint / grading-on flag / meter-on flag) in
  `u_fs_tonemapParams`, applied *after* the curve. Because the same pass now carries grading and
  the adapted exposure it also runs with the curve off, which `mode < 0` selects -
  `RenderSystem::renderHDR` passes `isTonemapEnabled() ? mode : -1`, so a scene with grading on and
  tonemap off keeps its authored response instead of silently getting ACES.
  All six `colorGrading*` settings have `Scene` accessors, `Stream.cpp` keys, `Catalog` rows,
  `Factory.cpp` codegen, Lua properties and Editor Settings rows.
- **Auto exposure**: `exposure.frag` runs into a pair of 1x1 R16F targets ping-ponged per frame
  (`exposureWriteIndex`), 24x24 jittered taps, geometric mean of luminance, 18 % middle-grey key
  plus `exposureCompensation` EV, clamped to the min/max EV range, adapting in log space and faster
  brightening than darkening. The tone map *samples* that texel (`grade2.w`); nothing reads back.
  `exposureLoaded` is deliberately optional - a scene that never turns the meter on must not lose
  the bloom chain because of it.

What is left declared-but-unwired - do not offer these in UI as if they worked, and grep for the
*definition and call site* before claiming any of them:

- **3D LUT** - `colorGradingLUTPath` (`SceneSettings.h:116`) is stored, persisted and editable but
  no shader samples a 3D texture: the built-in adjustments above are what actually run.

**Lens dirt runs** - it used to be on that list. `Scene::getBloomDirtIntensity()` is read by the
composite (`RenderSystem.cpp:5695`, `params.z`) and `bloom_composite.frag` multiplies the glow by a
procedural mask (three value-noise octaves weighted by a vignette), so no asset is needed.
`Scene::setBloomDirtIntensity` clamps to [0,1] - a scene file asking for 4.0 must not turn the lens
opaque. The mask is *static*: nothing ties it to the meter, and there is no flare shader. Re-bundling
it has its own trap - see `quantum-shaders`: the shader map is linked into `quantum-resource-compiler`
itself, so bundling with a compiler older than `generate_shaders` silently re-emits the maskless pass.

## The float-target trap

`sokol` refuses a pipeline whose `colors[0].pixel_format` differs from the pass attachment
(`VALIDATE_APIP_COLORATTACHMENTS_FORMAT`, and every later apply/bind fails with
`VALIDATE_AU_NO_PIPELINE` / `VALIDATE_ABND_NO_PIPELINE`). `SokolObject::endLoad` builds the
PIP_RTT family at a fixed format, so an offscreen pass that writes a float target has to declare
it *before* loading: `ObjectRender::setRenderTargetColorFormat(ColorFormat)` ->
`SokolObject::rttColorFormat` (`SokolObject.cpp:395`, default RGBA8). `loadHDR()` sets RGBA16F for
the four bloom passes and R16F for the meter. The pipeline cache key already includes the color
format, so a second format costs its own cached pipeline and nothing else.

This became fatal when `Framebuffer::create()` started routing a single non-RGBA attachment through
`createFramebufferMRT` - before that the float `setColorAttachments()` requests were silently
ignored and the HDR targets really were RGBA8, so the pipelines matched. Fixing the target format
is what exposed the mismatch: 363 of those errors per frame-set in the run log. If a new offscreen
pass gets a non-RGBA target, set both sides.

## Gotchas specific to this pass

- `MAX_BLOOM_LEVELS` is 5 (`engine/core/Engine.h:105-106`); `Scene::setBloomLevels` clamps into
  `[1, MAX_BLOOM_LEVELS]` because each level is a framebuffer request coming from a scene file.
- Adding `ShaderType`s changes `ShaderKey` composition -> bump the compiled-shader cache version
  (`editor/App.cpp:3844`, `.../quantum/shaders/vNN`, currently v37) or developers keep seeing stale
  compiles. **That version only guards the editor's per-user cache.** A project's own
  `assets/shaders/*.sdat` carries no version and no source hash, so after changing a uniform block
  the bundled variant is silently the old layout: the C++ struct sends 48 bytes (`fs_tonemap_t` with
  `grade` + `grade2`) into a block the stale `.sdat` declared at 32, and sokol fails every frame with
  `VALIDATE_AU_SIZE: sg_apply_uniforms: data size doesn't match declared uniform block size` while the
  chain still loads and logs `HDR chain ready:`. Re-bundle into each project after any block change:
  `quantum-resource-compiler.exe shaders --out <project>/assets/shaders --backend opengl --shader
  "<spec>"` per pass. A project with no `assets/shaders` at all does not run headless either: the
  runner exits 1 with `Quantum is missing precompiled shaders` and names the missing specs.
- Shader sources are **embedded**: run `cmake --build "$QUANTUM_BUILD_DIR" --target
  generate_shaders` after any `engine/shaderlib/` edit, or the change is invisible
  (`quantum-shaders`).
- Verify headless, never by launching the editor:
  `quantum-resource-compiler.exe shaders --out <dir> --backend opengl --shader "<type>"` per pass,
  then a scene that turns the chain on and prints the `HDR chain ready:` line. Assert a probe on
  the *gate* as well as the log: `isHdrEnabled()` decides the whole chain, and each pass can fail
  to load without taking the others down. **The look is a separate question** - the engine has no
  framebuffer readback, so grading and adaptation are proven to run, not proven to look right.
- A scene setting alone is not enough - see `quantum-api` for the settings touchpoint list
  (`SceneSettings` default -> `Scene` accessor -> `Stream` key -> `Catalog` row -> `Factory`
  codegen -> Lua property).
