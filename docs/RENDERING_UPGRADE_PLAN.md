# Quantum Engine - Plano de Upgrade de Renderização (Nível UE5)

## Visão Geral

O objetivo é elevar o sistema de renderização do Quantum ao nível de engines AAA como Unreal Engine 5, implementando feature por feature de forma incremental e testável.

---

## Fase 1: Pipeline HDR + Tone Mapping + Bloom (Impacto Visual ALTO, Esforço MÉDIO)

> **Referência UE5**: `PostProcessEyeAdapation.cpp`, `PostProcessBloomSetup.cpp`, `PostProcessTonemap.cpp`

> **Status (2026-10-25)**: a cadeia inteira roda em `RenderSystem::renderHDR`
> (`engine/core/subsystem/RenderSystem.cpp:5601`), gated por `Scene::isHdrEnabled()`
> (`engine/core/Scene.cpp:550`), e é provada headless pelas linhas `HDR chain ready:`,
> `eye adaptation ready:` e `HDR features:`. O que falta de fato está marcado abaixo.
> documentação do passe: skill `quantum-bloom`.

### 1.1 HDR Render Targets
- [x] Trocar framebuffers de RGBA8 para **RGBA16F** (half-float) nos alvos da cadeia HDR
      (`ensureHDRFramebuffers:5496` pede `ColorFormat::RGBA16F`; `Framebuffer::create()` roteja
      qualquer anexo não-RGBA por `createFramebufferMRT`, que é o que torna o formato real)
- [x] Atualizar `SokolFramebuffer`/`SokolTexture` para suportar formatos HDR (R8/R16/R16F/RGBA16F)
- [x] Pipeline offscreen no formato do alvo: `ObjectRender::setRenderTargetColorFormat()` →
      `SokolObject::rttColorFormat` (sokol rejeita pipeline com `colors[0]` diferente do anexo)
- [ ] **Alvo HDR flutuante no main color pass**: `sceneColorFramebuffer` ainda é RGBA8/sRGB
      (`ensureSceneColorFramebuffer:4848`) e `mesh.frag` termina em `linearTosRGB`, então nenhum
      pixel da cena escreve acima de 1.0. Sem isso o bloom é ofuscamento de pixels claros,
      não emissão real. Precisa de um variante HDR do `mesh.frag`/`pbr.glsl`.
- **Arquivos**: `engine/core/render/Render.h`, `engine/renders/sokol/SokolFramebuffer.h`,
  `engine/renders/sokol/SokolObject.{h,cpp}`

### 1.2 Auto-Exposure (Eye Adaptation)
- [x] Shader `exposure.frag` computando luminância da cena
- [x] Meter de luminância (24×24 taps com jitter, média geométrica em log) — feito como
      geometric mean multi-tap em vez de histograma de 256 bins; o histograma continua opcional
- [x] Buffer de histórico temporal: dois alvos 1×1 R16F em ping-pong por frame
      (`ensureExposureFramebuffers:5576`, `exposureWriteIndex`), sem readback de CPU
- [x] Integrado em `SceneSettings` (`autoExposure`, `autoExposureSpeed`, `autoExposureMinEV`,
      `autoExposureMaxEV`, `exposureCompensation`), com acesso no `Scene`, chave YAML, linha no
      Editor Settings e propriedade Lua
- [x] Bloom dirt: `bloomDirtIntensity` multiplica o glow por uma máscara procedural
      (três oitavas de value noise + vignette) dentro de `bloom_composite.frag` — sem asset
- [ ] Dirt reagindo à exposição e lens flare: a máscara é estática e `lensflares.frag`
      continua inexistente (9.6)
- **Shaders**: `engine/shaderlib/exposure.frag`
- **Referência UE5**: Luminance histogram com 64 bins, adaptação temporal

### 1.3 Bloom
- [x] Shaders `bloom_threshold.frag`, `bloom_downsample.frag`, `bloom_upsample.frag`,
      `bloom_composite.frag`
- [x] Cadeia de 5 mip levels (`MAX_BLOOM_LEVELS`), downsample + upsample aditivo, alocada uma
      vez e reconstruída quando o tamanho muda (`activeBloomMipLevels`)
- [x] Parâmetros threshold / intensity / soft knee / níveis expostos no `SceneSettings`, no
      editor, no YAML, no codegen e no Lua
- [x] **Lens dirt**: `bloomDirtIntensity` (`SceneSettings.h:96`) tem leitor —
      `renderHDR:5695` entrega o valor em `params.z` do composite, que o multiplica pela
      máscara procedural. O setter corta em [0,1] na entrada (`Scene.cpp`), provado por
      probe (`dirt 9 clamps to 1`, `dirt -3 clamps to 0`, 22/22)
- **Shaders**: `engine/shaderlib/bloom_*.frag`
- **Referência UE5**: `PostProcessBloomSetup.cpp` usa downsample chain de 5 níveis

### 1.4 Tone Mapping
- [x] `tonemap.frag` com as curvas Reinhard, Reinhard Extended, Uncharted 2, ACES Filmic e AgX
- [x] Tone map depois do bloom composite, como último passe antes do destino
- [x] Exposição manual com override do meter (`exposure` vs. `u_exposureTexture`,
      escolhido por `grade2.w`)
- [x] `mode < 0` = sem curva, porque grading e o meter usam o mesmo passe mesmo com tonemap off
- **Shaders**: `engine/shaderlib/tonemap.frag`
- **Referência UE5**: `PostProcessTonemap.cpp` usa ACES com 3D LUT

### 1.5 Color Grading (LUT 3D)
- [x] Grading aplicado: saturação, contraste, brilho, hue, temperatura e tint, depois da curva,
      dentro de `tonemap.frag` (`grade`/`grade2` em `u_fs_tonemapParams`) — não é um passe próprio
- [x] Propriedades em `SceneSettings.h:108-116` + `Scene` + YAML + Editor Settings + Lua + codegen
- [x] Integrado no editor (seção Graphics do Editor Settings)
- [ ] `colorGradingLUTPath` é salvo/editável mas **nenhum shader amostra uma 3D texture**: só o
      caminho analógico roda
- [ ] `colorgrading.frag` continua sem chamada como passe próprio (`ShaderPool` sabe compilá-lo;
      não há `RenderSystem::renderColorGrading`) — o grading que roda está dentro de `tonemap.frag`
- **Shaders**: `engine/shaderlib/colorgrading.frag`, `engine/shaderlib/tonemap.frag`

---

## Fase 2: Anti-Aliasing (TAA + FXAA) (Impacto Visual ALTO, Esforço MÉDIO)

> **Referência UE5**: `TemporalSuperResolution.cpp`, `PostProcessAA.cpp`

### 2.1 Velocity Buffer
- [x] Criar shader `velocity.frag` que computa motion vectors
- [x] Adicionar `prevViewProjection` matrix ao `CameraComponent`
- [ ] Render velocity buffer como MRT no base pass ou pass separado
- [ ] Formato: RG16F (2D motion vectors) + Blue para material ID
- **Shaders**: `engine/shaderlib/velocity.frag`
- **Arquivos**: `engine/core/component/CameraComponent.h`

### 2.2 Temporal Anti-Aliasing (TAA)
- [x] Criar shader `taa.frag` com:
  - [x] Reprojeção de histórico via velocity buffer
  - [x] Clamp de cor com neighborhood clamping (min/max AABB)
  - [x] Blending adaptativo (current vs history baseado em confiança)
  - [x] Jitter de câmera (Halton sequence) para sub-pixel sampling
  - [x] Anti-flicker (variance clipping)
- [ ] Manter 2 history buffers (current + previous)
- [ ] Integrar no post-process chain
- **Shaders**: `engine/shaderlib/taa.frag`
- **Referência UE5**: 16 samples/pixel, history resolution 100-200%

### 2.3 FXAA (Fallback)
- [x] FXAA ligado como último stage do frame (antes só existia o arquivo de shader)
- [x] `fxaa.frag` reescrito: o original não compilava (`texture(sampler2D(tex, uv), uv)`
      passava `vec2` onde o GLSL exige o sampler) e agora usa o par combinado
  - **Shaders**: `engine/shaderlib/fxaa.frag`
- [x] `u_fs_fxaaParams`: `x = 1/width`, `y = 1/height`, `z = blend [0..1]`, `w = flip Y`
- [x] `Scene::isFXAAEnabled()` / `getFXAAIntensity()` (independentes do `hdrEnabled`),
      com o clamp feito no setter porque a fração de blend é o contrato
- [x] `RenderSystem::loadFXAA/destroyFXAA/ensureFXAAFramebuffer/renderFXAA` +
      `updateSwapchainRedirect`: quem apresentaria o frame passa a escrever em
      `fxaaFramebuffer` e só o resultado anti-aliasado chega ao destino
- [x] Alvo com filtro LINEAR: as amostras ao longo da borda caem entre texels
- [x] Exposto em YAML (`fxaaEnabled`/`fxaaIntensity`), Catalog, Editor Settings, codegen
      (`Factory.cpp`) e Lua (`scene.fxaaEnabled` / `scene.fxaaIntensity`)
- [x] Probe headless `probe_fxaa.lua`: 8 checagens, `FXAA ready` no log, 0 `VALIDATE_`,
      sobrevive aos dois flips do redirect (on → off → on)
- [ ] Termos opcionais do preset 3.11 de qualidade: `subpix` e `edgeShading`
- [ ] Alternar com TAA quando a Fase 2.1/2.2 existir (FXAA é o fallback, não o par)
- **Pendência visual**: o efeito roda e é provado headless, mas ninguém viu o resultado na
  tela - exige abrir o jogo e olhar.

---

## Fase 3: Material System Avançado (Impacto Visual ALTO, Esforço BAIXO)

> **Referência UE5**: Material layers, clearcoat, sheen, subsurface

### 3.1 Clearcoat
- [x] Descomentar e habilitar campos em `Material.h`:
  ```cpp
  float clearcoatFactor;
  float clearcoatRoughness;
  Texture clearcoatTexture;
  Texture clearcoatNormalTexture;
  ```
- [x] Atualizar `mesh.frag` para incluir clearcoat BRDF
- [x] Adicionar shader include `includes/clearcoat.glsl`
- [ ] Atualizar `ShaderPool` variant bits para clearcoat
- **Referência UE5**: `BRDF_ClearCoat` no Filament docs

### 3.2 Sheen (Já implementado no BRDF!)
- [x] Descomentar campos em `Material.h`:
  ```cpp
  float sheenRoughnessFactor;
  vec3 sheenColorFactor;
  Texture sheenColorTexture;
  ```
- [x] Conectar `BRDF_specularSheen()` no `mesh.frag` (código já existe em `brdf.glsl`)
- [ ] Atualizar `MaterialInfo` struct no shader

### 3.3 Transmission / Subsurface
- [x] Adicionar `transmissionFactor` ao `Material`
- [x] Adicionar `subsurfaceColor` e `subsurfaceIntensity`
- [x] Implementar transmissão via screen-space refraction no fragment shader
- [x] Para subsurface: blur de cor em profundidade (simplified SSS)
- [x] Criar shader include `includes/transmission.glsl`
- **Referência UE5**: `PostProcessSubsurface.cpp`

### 3.4 Anisotropy
- [x] Adicionar `anisotropyStrength` e `anisotropyRotation` ao `Material`
- [ ] Criar `includes/anisotropy.glsl` com GGX anisotrópico
- [ ] Requer tangent space (já suportado via TBN)

### 3.5 Parallax Occlusion Mapping (POM)
- [x] Criar shader `includes/parallax.glsl`
- [x] Implementar steep parallax mapping com binary search refinement
- [x] Adicionar parâmetros: height scale, min/max layers, LOD
- [x] Atualizar `Material.h` com `heightTexture` e `heightScale`

### 3.6 Detail Textures
- [ ] Adicionar `detailBaseColorTexture` e `detailNormalTexture` ao `Material`
- [ ] Criar include `includes/detail.glsl` para blend de detail maps
- [ ] Suportar triplanar mapping para detail normals

---

## Fase 4: Shadows Melhoradas (Impacto Visual MÉDIO, Esforço MÉDIO)

> **Referência UE5**: `ScreenSpaceShadows.cpp`, PCSS, Virtual Shadow Maps

### 4.1 Contact Shadows (Screen-Space)
- [x] Criar shader `includes/contact_shadows.glsl`
- [x] Implementar ray marching no depth buffer (4-8 steps)
- [x] Integrar no `mesh.frag` para pontos de contato próximos
- [x] Muito barato - apenas depth buffer sampling
- **Referência UE5**: `Shadows/ScreenSpaceShadows.cpp`

### 4.2 PCSS (Percentage-Closer Soft Shadows)
- [x] Criar shader `includes/pcss.glsl`
- [x] Implementar PCSS:
  - [x] Blocker search pass (encontra blockers do sol)
  - [x] Penumbra estimation (tamanho do penumbra baseado na distância)
  - [x] PCF filtrado com raio variável
- [ ] Adicionar parâmetro `softShadowScale` ao `LightComponent`
- **Referência UE5**: PCSS com block search + variable PCF

### 4.3 Stable Cascaded Shadows
- [ ] Implementar cascade stabilization (snap to texel centers)
- [ ] Adicionar shadow distance fade (suave transição no fim do cascade)
- [ ] Reduzir "swimming" nas bordas dos cascades

### 4.4 Temporal Shadow Filtering
- [ ] Acumular sombras de frames anteriores via velocity buffer
- [ ] Reduzir flickering em sombras de bordas

---

## Fase 5: SSR Melhorado (Impacto Visual MÉDIO, Esforço MÉDIO)

> **Referência UE5**: `ScreenSpaceRayTracing.cpp` com Hi-Z

### 5.1 Hierarchical-Z (Hi-Z) Ray Marching
- [x] Criar shader `hiz_gen.frag` que gera mipmap chain do depth buffer
- [x] Criar shader `ssr_hiz.frag` com Hi-Z para traversal acelerado
- [x] Começar em mip alto, descer quando encontrar miss
- [x] Refinamento binário no nível final
- **Shaders**: `engine/shaderlib/hiz_gen.frag`, `engine/shaderlib/ssr_hiz.frag`
- **Impacto**: Reduz steps de 128 para ~16-32 em média

### 5.2 Temporal SSR
- [ ] Acumular reflexões de frames anteriores
- [ ] Usar velocity buffer para reprojeção
- [ ] Confidence baseada em reprojecção válida

### 5.3 Screen-Space Refractions
- [ ] Criar shader `includes/refraction.glsl`
- [ ] Usar depth buffer para encontrar ponto de saída
- [ ] Amostrar cena refratada com IOR (index of refraction)
- [ ] Integrar com transmissionFactor do material

---

## Fase 6: Volumetric Fog & Lighting (Impacto Visual MUITO ALTO, Esforço ALTO)

> **Referência UE5**: `VolumetricFog.cpp`, Froxel-based

### 6.1 Froxel-Based Volumetric Fog
- [x] Criar 3D texture (froxel volume): 160x90x64 (XYxZ)
- [x] Criar shader `volumetric_fog_inject.frag` - injeta luz e densidade em cada froxel
- [x] Criar shader `volumetric_fog_scattering.frag` - accumula scattering ao longo do eixo Z
- [x] Criar shader `volumetric_fog_composite.frag` - compõe fog na cena
- [x] Integrar luzes pontuais, spot e direcional no injection
- **Shaders**: `engine/shaderlib/volumetric_fog_*.frag`

### 6.2 Exponential Height Fog
- [x] Criar shader `includes/height_fog.glsl`
- [x] Criar shader `height_fog.frag`
- [x] Fog exponencial com base de altura configurável
- [x] Integrar com volumetric fog para efeitos de god rays

### 6.3 Light Shafts (God Rays)
- [x] Criar shader `lightshafts.frag`
- [x] Radial blur baseado no screen-space light position
- [x] Composite aditivo sobre a cena
- [x] Alternativa barata: usar o volumetric fog para god rays naturais

---

## Fase 7: Global Illumination Simplificado (Impacto Visual MUITO ALTO, Esforço MUITO ALTO)

> **Referência UE5**: Lumen (Screen Probes + Radiance Cache)

### 7.1 Light Probe Grid
- [x] Criar `LightProbeGrid` class - grade 3D de SH (Spherical Harmonics) probes
- [x] Cada probe armazena: SH L2 (9 coefficients) para irradiância
- [x] Criar include `includes/lightprobe.glsl` para evaluação de SH
- [ ] Atualizar probes via ray tracing simplificado ou cubemap capture
- [ ] Interpolação trilinear entre probes
- **Shaders**: `engine/shaderlib/includes/lightprobe.glsl`

### 7.2 Screen-Space GI (SSGI)
- [x] Criar shader `ssgi.frag`:
  - [x] Ray march no depth buffer em direções aleatórias
  - [x] Amostrar cor no ponto de hit
  - [x] Acumular irradiância por frame (temporal)
- [x] Usar structured buffer para armazenar irradiância por pixel
- [x] Aplicar como termo ambient na iluminação
- **Shaders**: `engine/shaderlib/ssgi.frag`
- **Referência UE5**: Lumen Screen Probe Gather (simplificado)

### 7.3 Radiance Cache
- [ ] Grade world-space de probes com SH L2
- [ ] Cada probe: 6 faces cubemap → SH (ou octahedral map)
- [ ] Atualizar N probes por frame (round-robin)
- [ ] Usar para iluminação indireta de materiais e sombras
- **Referência UE5**: `LumenRadianceCache.cpp`

---

## Fase 8: Virtual Shadow Maps (Impacto Visual ALTO, Esforço MUITO ALTO)

> **Referência UE5**: `VirtualShadowMaps/` (9 files)

### 8.1 Page-Based Virtual Shadows
- [x] Criar shader `vsm_depth.frag` para renderizar depth por page
- [x] Criar shader `vsm_sample.frag` para amostrar com PCF
- [ ] Criar `VirtualShadowMap` class:
  - [ ] Page table (128x128 texels por page)
  - [ ] Physical page pool (max 64K pages)
  - [ ] Clipmap cascades para directional light

### 8.2 Temporal Caching
- [ ] Reutilizar pages não alterados entre frames
- [ ] Invalidation via dirty flag por object

### 8.3 Integração com Nanite-style
- [ ] Se implementar mesh simplification, VSM pode usar isso

---

## Fase 9: Post-Processing Completo (Impacto Visual MÉDIO, Esforço BAIXO)

> **Referência UE5**: `PostProcess/` (80+ files)

### 9.1 Depth of Field (Bokeh)
- [x] Criar shader `dof_coc.frag` - Circle of Confusion calculation
- [x] Criar shader `dof_blur.frag` - Gaussian/Bokeh blur ponderado por CoC
- [x] Criar shader `dof_composite.frag` - blend bokeh over sharp
- [ ] Modo: Gaussian (barato) e Bokeh (quality)
- **Shaders**: `engine/shaderlib/dof_*.frag`

### 9.2 Motion Blur
- [x] Criar shader `motionblur.frag`
- [x] Usar velocity buffer para blur direcional por pixel
- [ ] Tile-based optimization (agrupar pixels com velocidade similar)
- [ ] Configurable: intensity, max samples

### 9.3 Chromatic Aberration
- [x] Criar shader `chromatic.frag`
- [x] Separar RGB com offset radial
- [x] Intensity configurável

### 9.4 Vignette
- [x] Criar shader `vignette.frag`
- [x] Darken suave nas bordas do frame

### 9.5 Film Grain
- [x] Criar shader `filmgrain.frag`
- [x] Noise temporal (varia por frame)
- [x] Intensity configurável

### 9.6 Lens Flares
- [ ] Criar shader `lensflares.frag`
- [ ] Ghost sprites baseados em bright spots
- [ ] Starburst via radial blur

---

## Fase 10: Advanced Features (Nível UE5 Completo)

### 10.1 Clustered Forward Rendering
- [ ] Implementar tile-based light culling em compute shader
- [ ] Grade 16x16 tiles, cada tile lista luzes que o intersectam
- [ ] Reduzir light loop de O(n) para O(lights_per_tile)
- **Arquivo**: Modificar `RenderSystem` para usar clustered

### 10.2 GPU-Driven Rendering
- [ ] Implementar frustum culling via compute shader
- [ ] Indirect draw buffers (glDrawElementsIndirect)
- [ ] Reduzir CPU overhead em cenas com muitos objetos

### 10.3 Mesh LOD Automático
- [ ] Implementar simplificação de malha (edge collapse)
- [ ] LOD levels baseados em distância/screen coverage
- [ ] Transições suaves (dithered crossfade)

### 10.4 Subsurface Scattering (SSS)
- [ ] Dual-source blur separável (separable SSS)
- [ ] Dois passes: horizontal + vertical
- [ ] Kernel baseado em perfil de pele humana

---

## Ordem de Implementação Recomendada

```
1. HDR + Tone Mapping + Bloom  ← MELHOR custo-benefício
2. TAA / FXAA                  ← Melhora muito a qualidade
3. Material Clearcoat/Sheen     ← Fácil, código já existe
4. Contact Shadows              ← Barato, alto impacto
5. PCSS                         ← Melhora sombras
6. SSR Hi-Z                     ← Performance + qualidade
7. Volumetric Fog               ← Visual WOW
8. Depth of Field               ← Post-process clássico
9. Motion Blur                  ← Post-process clássico
10. Light Probes / SSGI         ← GI dinâmica
11. Virtual Shadow Maps          ← Mega-feature
12. Clustered Rendering          ← Performance scaling
```

---

## Arquivos Principais a Modificar

| Arquivo | Mudanças |
|---------|----------|
| `engine/core/render/Render.h` | Novos formatos (RGBA16F, etc.) |
| `engine/core/texture/Material.h` | Clearcoat, sheen, transmission, anisotropy, height |
| `engine/core/subsystem/RenderSystem.h` | HDR framebuffers, exposure, bloom, velocity, volumetric |
| `engine/core/subsystem/RenderSystem.cpp` | Novos passes de renderização |
| `engine/core/component/CameraComponent.h` | prevViewProjection, exposure |
| `engine/core/scene/SceneSettings.h` | Bloom, tone mapping, fog, GI params |
| `engine/shaderlib/mesh.frag` | Clearcoat, sheen, transmission |
| `engine/shaderlib/includes/brdf.glsl` | Anisotropy, clearcoat BRDF |
| `engine/renders/sokol/SokolFramebuffer.h` | Suporte HDR formats |
| `engine/core/pool/ShaderPool.h` | Novos variant bits |

---

## Checklist Geral de Qualidade

- [ ] Pipeline HDR completo (16-bit float) — a cadeia HDR é RGBA16F; o alvo da cena ainda é RGBA8/sRGB
- [x] Auto-exposure (média geométrica multi-tap em log, não histograma)
- [x] Bloom multi-pass
- [x] Tone mapping ACES/AgX
- [ ] Color grading 3D LUT — só o caminho analógico dentro de `tonemap.frag`
- [ ] TAA temporal anti-aliasing
- [x] FXAA fallback (último stage do frame, depois do tone map e do upscale de
      resolução interna; force `scene.fxaaEnabled` e ajuste `scene.fxaaIntensity`)
- [ ] Velocity buffer
- [ ] Motion blur
- [ ] Depth of field (bokeh)
- [ ] Clearcoat material
- [ ] Sheen material
- [ ] Transmission/subsurface
- [ ] Parallax occlusion mapping
- [ ] Contact shadows
- [ ] PCSS soft shadows
- [ ] SSR com Hi-Z
- [ ] Volumetric fog
- [ ] Height fog
- [ ] Light shafts
- [ ] Screen-space GI
- [ ] Light probe grid
- [ ] Virtual shadow maps
- [ ] Clustered forward rendering
- [ ] Chromatic aberration
- [ ] Vignette
- [ ] Film grain
