---
name: doriax-lighting
description: Doriax lighting and shadows - glTF PBR/BRDF shader includes, directional/point/spot lights, shadow atlases, cascades, IBL reflection probes, 2D lights and occluders. Use when changing light behaviour, fixing shadow acne/peter-panning, adding a shading term (sheen, clearcoat, transmission), or debugging 2D lighting.
---

# Lighting and shadows

All shading happens forward, in `engine/tools/shaderlib/mesh.frag`. There is no
deferred lighting pass (see `doriax-shaders`).

## Shader includes (`engine/tools/shaderlib/includes/`)

| Concern | Anchor |
|---|---|
| metallic/roughness extraction | `pbr.glsl:53` `getMetallicRoughnessInfo` |
| BRDF, sheen (`D_Charlie`, `BRDF_specularSheen`) | `brdf.glsl:45-106` |
| clearcoat lobe | `clearcoat.glsl` |
| transmission | `transmission.glsl:66` |
| image-based lighting | `ibl.glsl`, `lightprobe.glsl` |
| punctual lights | `punctual.glsl` |
| PCSS / PCF filtering | `pcss.glsl`, `shadows.glsl` |
| parallax occlusion | `parallax.glsl` |
| normal encoding | `octahedral.glsl` |
| colour space | `srgb.glsl` |
| 2D light accumulation | `lighting2d.glsl` |
| per-pixel fog | `fog.glsl:7` `getFogColor` |

`MaterialInfo` (`mesh.frag:152`) carries 16 members including the sheen/clearcoat/
transmission factors; default IOR is 1.5 (`mesh.frag:~263`). Adding a term means:
struct member + its positional zero-init (`mesh.frag:259-261`, count must stay in
sync) + the variant bit that gates it + the uniform block in `Render.h`
`UniformBlockType`.

## Lights

- Types `DIRECTIONAL` / `POINT` / `SPOT` - `engine/core/component/LightComponent.h:17-20`.
- `MAX_LIGHTS = 6` per pass (set in `ShaderBuilder::setupShaderArgs:661-740`);
  raising it changes every `mesh:*` variant -> bump the shader cache version dir.
- Up to `MAX_SHADOWCASCADES` = 4 cascades for directional light.

## Pass order (`RenderSystem::draw()`, `engine/core/subsystem/RenderSystem.cpp:6812`)

1. Directional/spot shadow atlas, one viewport per slot (`:6886-6919`,
   `ensureShadowAtlas:1526`) - depth-only, comparison sampler, atlas format v17.
2. Point-cube atlas (`:6921-6952`, v9).
3. 2D polar shadow atlas rows (`:6955-7019`, `ensureShadow2DAtlas:911`,
   `loadOccluder2DPass:1882`, v10).
4. Reflection-probe capture (`:7024`, `renderReflectionProbeCapture:1347`;
   box-projected probes `:1270`).
5. Per camera: optional G-buffer + SSR (gated `isSSREnabled():7081-7084`), optional
   depth pre-pass (`:3396`), SSAO (`:3329`), colour pass (sky `drawSky:4899` ->
   opaque `drawMesh:2939` -> UI/points/lines -> sorted transparents `:7366`).

Spot **cookie** masks are baked CPU-side into an atlas
(`updateSpotMaskAtlas:544`, v16) - a new light texture feature must follow that
either/or: CPU bake vs GPU pass.

## Fog (the only atmospheric effect that runs)

`FogComponent` with LINEAR/EXP/EXP2 (`RenderSystem.cpp:982-1003`), applied per pixel
in `fog.glsl` behind variant bit 12 `HAS_FOG` ('Fog', `ShaderBuilder.cpp:459`),
YAML through `editor/Stream.cpp:5092-5117`. Height fog, volumetric fog and light
shafts have shader files but no pass - see `doriax-volumetrics`.

## 2D lighting

`USE_LIGHT2D` variant bit 22 ('L2d', `ShaderBuilder.cpp:469`) plus
`lighting2d.glsl`; occluders are `Occluder2DComponent` (AUTO_QUAD/POLYGON,
inspector row `editor/window/Properties.cpp:7986`, edited in
`editor/window/SceneWindow.cpp:1096+`).

## Debugging posture

Lighting regressions are almost never one cause: check (a) the variant letters in the
`.sdat` name actually contain the term you enabled, (b) the cache dir isn't serving a
pre-change compile, (c) `resource-compiler shaders` accepts the variant, then (d) ask
the user to look at the scene - an agent must not launch the editor.
