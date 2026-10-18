# Doriax Engine - Plano de Upgrade de Renderização (Nível UE5)

## Visão Geral

O objetivo é elevar o sistema de renderização da Doriax ao nível de engines AAA como Unreal Engine 5, implementando feature por feature de forma incremental e testável.

---

## Fase 1: Pipeline HDR + Tone Mapping + Bloom (Impacto Visual ALTO, Esforço MÉDIO)

> **Referência UE5**: `PostProcessEyeAdapation.cpp`, `PostProcessBloomSetup.cpp`, `PostProcessTonemap.cpp`

### 1.1 HDR Render Targets
- [x] Trocar framebuffers de RGBA8 para **RGBA16F** (half-float) no main color pass
- [x] Atualizar `SokolFramebuffer` para suportar formatos HDR
- [x] Adicionar enum `TextureFormat::RGBA16F` se não existir
- [x] Criar `FramebufferRender` dedicado para HDR scene color
- **Arquivos**: `engine/core/render/Render.h`, `engine/renders/sokol/SokolFramebuffer.h`

### 1.2 Auto-Exposure (Eye Adaptation)
- [x] Criar shader `exposure.frag` para computar luminância da cena
- [ ] Implementar histograma de luminância (256 bins) via compute ou fullscreen pass
- [ ] Adicionar buffer de histórico de exposição (temporal smoothing)
- [ ] Criar `ExposureComponent` ou integrar no `SceneSettings`
- **Shaders**: `engine/tools/shaderlib/exposure.frag`, `includes/histogram.glsl`
- **Referência UE5**: Luminance histogram com 64 bins, adaptação temporal

### 1.3 Bloom
- [x] Criar shader `bloom_threshold.frag` - extrai pixels acima do threshold HDR
- [x] Criar shader `bloom_downsample.frag` - downsampling com filtro 13-tap
- [x] Criar shader `bloom_upsample.frag` - upsampling com interpolação gaussiana
- [x] Criar shader `bloom_composite.frag` - soma todos os mip levels
- [ ] Implementar ping-pong chain com 5-6 mip levels
- [ ] Adicionar parâmetros: threshold, intensity, knee (soft threshold)
- **Shaders**: `engine/tools/shaderlib/bloom_*.frag`
- **Referência UE5**: `PostProcessBloomSetup.cpp` usa downsample chain de 5 níveis

### 1.4 Tone Mapping
- [x] Criar shader `tonemap.frag` com múltiplas curvas:
  - [x] Reinhard
  - [x] ACES Filmic (UE5 default)
  - [x] AgX (novo, mais perceptual)
  - [x] Khronos PBR Neutral
- [x] Aplicar tone mapping APÓS bloom composite
- [ ] Adicionar parâmetro de exposição manual override
- **Shaders**: `engine/tools/shaderlib/tonemap.frag`
- **Referência UE5**: `PostProcessTonemap.cpp` usa ACES com 3D LUT

### 1.5 Color Grading (LUT 3D)
- [x] Criar shader `colorgrading.frag` que amostra a LUT
- [ ] Criar shader `lut_gen.frag` para gerar 3D LUT (16^3 ou 32^3)
- [ ] Adicionar propriedades: saturation, contrast, temperature, tint, shadows/midtones/highlights
- [ ] Integrar no editor (janela de post-process)
- **Shaders**: `engine/tools/shaderlib/colorgrading.frag`, `includes/colorgrading.glsl`

---

## Fase 2: Anti-Aliasing (TAA + FXAA) (Impacto Visual ALTO, Esforço MÉDIO)

> **Referência UE5**: `TemporalSuperResolution.cpp`, `PostProcessAA.cpp`

### 2.1 Velocity Buffer
- [x] Criar shader `velocity.frag` que computa motion vectors
- [x] Adicionar `prevViewProjection` matrix ao `CameraComponent`
- [ ] Render velocity buffer como MRT no base pass ou pass separado
- [ ] Formato: RG16F (2D motion vectors) + Blue para material ID
- **Shaders**: `engine/tools/shaderlib/velocity.frag`
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
- **Shaders**: `engine/tools/shaderlib/taa.frag`
- **Referência UE5**: 16 samples/pixel, history resolution 100-200%

### 2.3 FXAA (Fallback)
- [x] Implementar FXAA 3.11 como fallback para hardware sem compute
- [x] Criar shader `fxaa.frag`
- **Shaders**: `engine/tools/shaderlib/fxaa.frag`

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
- **Shaders**: `engine/tools/shaderlib/hiz_gen.frag`, `engine/tools/shaderlib/ssr_hiz.frag`
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
- **Shaders**: `engine/tools/shaderlib/volumetric_fog_*.frag`

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
- **Shaders**: `engine/tools/shaderlib/includes/lightprobe.glsl`

### 7.2 Screen-Space GI (SSGI)
- [x] Criar shader `ssgi.frag`:
  - [x] Ray march no depth buffer em direções aleatórias
  - [x] Amostrar cor no ponto de hit
  - [x] Acumular irradiância por frame (temporal)
- [x] Usar structured buffer para armazenar irradiância por pixel
- [x] Aplicar como termo ambient na iluminação
- **Shaders**: `engine/tools/shaderlib/ssgi.frag`
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
- **Shaders**: `engine/tools/shaderlib/dof_*.frag`

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
| `engine/tools/shaderlib/mesh.frag` | Clearcoat, sheen, transmission |
| `engine/tools/shaderlib/includes/brdf.glsl` | Anisotropy, clearcoat BRDF |
| `engine/renders/sokol/SokolFramebuffer.h` | Suporte HDR formats |
| `engine/core/pool/ShaderPool.h` | Novos variant bits |

---

## Checklist Geral de Qualidade

- [ ] Pipeline HDR completo (16-bit float)
- [ ] Auto-exposure com histograma
- [ ] Bloom multi-pass
- [ ] Tone mapping ACES/AgX
- [ ] Color grading 3D LUT
- [ ] TAA temporal anti-aliasing
- [ ] FXAA fallback
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
