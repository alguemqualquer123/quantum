// (c) SR VINIX
// SPDX-License-Identifier: MIT

# Quantum Engine - TODO & Feature Map

## Core Engine

### ECS (Entity Component System)
- [x] ComponentManager com registro tipado
- [x] EntityManager com free-list recycling
- [x] Signature bitset para matching de subsystems
- [x] SubSystem base class (load, draw, destroy, update, fixedUpdate)
- [x] 52+ tipos de componentes
- [x] Object pooling para entidades (free-list LIFO limitada, reciclagem com geração e enumeração de entidades vivas)
- [x] Componente de tags para filtragem rápida

### Engine Core
- [x] Game loop com delta time
- [x] Scene management (múltiplas cenas)
- [x] Scaling modes (fixed width, fixed height, fixed resolution)
- [x] Async resource loading com thread pool
- [x] Resource pool management
- [~] Hot-reload de cenas - `SceneManager.reloadCurrentScene()` reexecuta a factory registrada e está disponível em C++/Lua; watcher automático de arquivos e preservação de estado ainda faltam
- [~] Save/Load de estado do jogo (slots QSV1 v3 com blobs, checksum, limites, migrações v1/v2 e snapshot ECS QNETECS3; serializers de Network/Script/Model/Animation/Action/PositionAction/RotationAction/ScaleAction/ColorAction/AlphaAction/TimedAction/AnimatorController/Mesh/Polygon/Joint2D/Joint3D/Bone/MorphTracks/KeyframeTracks/ScaleTracks/TranslateTracks/RotateTracks/Bundle/InstancedMesh/Terrain/Sprite/SpriteAnimation/MeshPolygon/Points/Lines/Tilemap/Tag/Sound/Particles/Light/Light2D/Sky/Mirror/Occluder2D/WaterExclusion/AutoCollider/Camera/Fog/ReflectionProbe/Water/Decal e parâmetros autorais de Body2D/Body3D integrados, cobertura de todos os componentes ainda pendente)
- [x] Sistema de eventos global (event bus)

---

## Rendering

### Pipeline Principal
- [~] Deferred PBR rendering (declarações/código legado; o caminho ativo da cena é forward)
- [x] Forward rendering
- [~] G-Buffer pass (usado por depth/SSAO/SSR, não como iluminação deferred principal)
- [~] Deferred lighting pass (código legado não instanciado pelo RenderSystem)
- [x] Post-process chain
- [x] Fixed resolution upscale (blit)
- [x] HDR render targets (RGBA16F) - **Implementado**
- [x] Tone mapping (ACES, Reinhard, AgX, PBR Neutral) - **Implementado**
- [x] Bloom multi-pass (threshold + downsample + upsample + composite) - **Implementado**
- [x] Auto-exposure (eye adaptation) - **Implementado**
- [~] Color grading 3D LUT - integrado; harness OpenGL compara identidade e uma fixture cinematográfica determinística, que divergiu da identidade em todos os pixels; runtimes/baselines nativos de outros backends ainda faltam
- [~] Temporal anti-aliasing (TAA) - integrado, harness OpenGL comparado; falta baseline perceptual dedicada
- [x] FXAA fallback - **Implementado**
- [x] Velocity buffer (motion vectors) - **Implementado**
- [~] Motion blur - integrado, harness OpenGL comparado; falta fixture perceptual dedicada
- [x] Depth of field (CoC + blur + composite) - **Implementado**
- [x] Chromatic aberration - **Implementado**
- [x] Vignette - **Implementado**
- [x] Film grain - **Implementado**
- [x] Contact shadows (screen-space) - **Implementado**
- [x] Hi-Z SSR acceleration - **Implementado**
- [~] Clustered forward rendering - SSBO/grid ativos somente nos backends compatíveis; OpenGL 4.1/GLES3 usam fallback UBO, harness OpenGL comparado; ainda falta cache persistente e fixture de limites
- [x] Temporal anti-aliasing (TAA)
- [x] HDR bloom
- [x] Motion blur
- [x] Depth of field (DOF)
- [x] Color grading / LUT
- [~] Screen-space global illumination (SSGI) - pass integrado ao ciclo de render e harness OpenGL comparado; fixture dedicada e tuning visual ainda faltam
- [~] Volumetric lighting (atmospheric height fog, heterogeneous procedural fog, light shafts and optional ray-marched fog are wired; froxel/3D-texture injection and shadow-map volumes remain)
- [ ] Ray tracing (futuro)

### Shadows
- [x] Cascaded shadow maps (directional light)
- [x] Omnidirectional shadow maps (point lights)
- [x] Spot light shadows
- [x] 2D shadow rendering (light2D + occluders)
- [~] Soft shadows (PCF receiver-plane slope bias and adaptive 2D/3D filtering are wired; PCSS blocker search and full VSM feedback remain)
- [x] Shadow bias auto-ajustável (receiver-plane slope bias)

### SSAO & SSR
- [x] Screen-space ambient occlusion (SSAO)
- [x] SSAO blur
- [x] Screen-space reflections (SSR)
- [x] SSR blur
- [ ] GTAO (Ground Truth AO)
- [~] SSR com fallback para reflection probes — a fixture `--render-feature ssrfallback` adiciona um probe dinâmico, captura suas seis faces e valida SSR + fallback no frame real; baselines nativas por GPU/driver ainda faltam

### Sky & Environment
- [x] Skybox rendering
- [x] Reflection probes (IBL)
- [x] IBL environment map processing (irradiance + prefiltered)
- [ ] Procedural sky (sol, nuvens, atmosfera)
- [ ] Dynamic time of day
- [ ] Volumetric clouds

### Fog
- [~] Fog analítico por pixel com height fog heterogêneo e shafts atmosféricos; froxel volumetric fog/3D texture ainda não está conectado
- [ ] Fog com distância de início/fim
- [ ] Fog com gradiente de cor

### Mirrors
- [x] Planar mirror reflections
- [ ] SSR híbrido com mirrors

### 2D Rendering
- [x] Sprite rendering
- [x] Tilemap rendering com chunking
- [x] 2D lights com normal maps
- [x] 2D occluder shadows
- [x] Polygon mesh rendering
- [ ] 2D particles (separado de 3D)
- [ ] Tilemap animation (water, etc.)

### 3D Rendering
- [x] glTF model loading (async)
- [x] OBJ model loading
- [x] Skeletal animation
- [x] Morph targets
- [x] GPU instanced meshes
- [x] Terrain com clipmap LOD
- [x] Point cloud rendering
- [x] Line rendering
- [ ] Decals
- [ ] LOD automático baseado em distância
- [ ] Meshlets / Mesh shading (futuro)

### Materials
- [x] PBR material (albedo, normal, metallic, roughness, emission)
- [x] Clearcoat BRDF (código pronto no brdf.glsl) - **Shaders criados**
- [x] Sheen BRDF (código pronto no brdf.glsl) - **Shaders criados**
- [ ] Transmission / Subsurface
- [ ] Anisotropy
- [ ] Parallax occlusion mapping
- [ ] Detail textures
- [ ] Material instancing otimizado

---

## Physics

### 2D Physics (Box2D)
- [x] Rigid body (static, dynamic, kinematic)
- [x] Shapes: box, circle, polygon, edge, chain
- [x] Joints: distance, revolute, prismatic, wheel, weld, motor
- [x] Contact callbacks
- [x] Raycasting 2D
- [x] Controllers (buoyancy, wind) - `WindForceController2D` cobre força de vento e `FloatOnWater2D` cobre buoyancy contra um plano 2D com onda opcional; água 2D simulada ainda não existe.
- [ ] Trigger zones otimizadas
- [~] Debug draw para física 2D (overlay `Lines` com corpos/shapes e cor dedicada para sensores; smoke headless validado, captura visual e toggle de edição ainda faltam)

### 3D Physics (Jolt Physics)
- [x] Rigid body
- [x] Shapes: box, sphere, capsule, convex hull, mesh
- [x] Joints: fixed, distance, hinge, cone, 6DOF, path, gear, rack-pinion, pulley, swing-twist
- [x] Contact callbacks
- [x] Raycasting 3D
- [x] Shape casting
- [~] Vehicle physics (controller Lua com suspensão/tração; constraint veicular nativa ainda falta)
- [ ] Soft body physics
- [ ] Ragdoll system
- [~] Physics material (friction/restitution por shape, biblioteca nomeada ainda falta)
- [~] Collision layers / filtering rules (runtime implementado; ferramentas visuais ainda faltam)

---

## Audio

### SoLoud Backend
- [x] WAV streaming
- [x] Spatial 3D audio
- [x] Global volume control
- [x] Pause/Resume
- [x] Filtros: reverb, echo, flanger, chorus, bassboost, DC removal, equalizer
- [x] OGG Vorbis streaming
- [x] Music streaming com crossfade
- [~] Audio occlusion / obstruction (smoke físico validado; medição acústica dedicada ainda falta)
- [x] Ambience system
- [x] Audio bus / mixer hierarchy
- [~] Doppler effect (runtime integrado; prova acústica mensurável ainda falta)

---

## Input

- [x] Keyboard (todas as teclas)
- [x] Mouse (posição, botões, scroll)
- [x] Touch (multi-touch)
- [x] Gamepad (XInput Windows, evdev Linux, GameController Apple)
- [x] Gamepad mapping database (SDL format)
- [x] Input mapping / rebinding system
- [x] Input actions (abstract input bindings, transitions, persistence of bindings/context state and priority-based contexts, C++/Lua press/release callbacks and action enumeration)
- [~] Vibration / haptic feedback (XInput Windows; backends nativos adicionais ainda faltam)
- [x] Gesture recognition
- [~] IME (Input Method Editor) para CJK (Windows integrado; backends adicionais ainda faltam)

---

## Scripting

### Lua 5.4 + LuaBridge3
- [x] Bindings: Engine, Scene, Input
- [x] Bindings: Entity, Component operations
- [x] Bindings: Math types (Vector2/3/4, Matrix, Quaternion)
- [x] Bindings: Actions/Animations
- [x] Bindings: Game objects (Mesh, Sprite, Light, Camera, etc.)
- [x] Bindings: File I/O
- [x] Bindings: Threading
- [x] Bindings: Utilities
- [x] Script property reflection
- [~] Debugger Lua (attached/Remote TCP com breakpoints, step, next/out, watches, stack, filtros de erro e IDs persistentes; captura visual do painel ainda falta)
- [~] Hot-reload de scripts Lua: troca manual e polling opt-in por mtime preservam a instância e oferecem `onReload`; hot reload C++ com ABI, staging e troca segura de DLL está integrado, mas teste GUI/Play ainda falta
- [ ] Coroutines better integration
- [~] Script profiling (tempo/chamadas/erros por frame e `slowestScripts` com nome/origem/linha; alocação detalhada ainda falta)

### C++ Scripting
- [x] ScriptBase class
- [~] C++ hot-reload com DLL swapping (ABI fingerprint, staging, validação e troca segura integrados; teste GUI/Play ainda falta)

---

## UI System

- [x] Anchor-based layout system
- [x] Text rendering (HarfBuzz + SheenBidi para RTL)
- [x] Button com eventos
- [x] Container
- [x] Image
- [x] Panel (draggable, resizable)
- [x] Scrollbar
- [x] TextEdit (input field)
- [x] ProgressBar
- [x] Polygon (UI shapes)
- [ ] Layout automático (flexbox/grid)
- [ ] Tooltip system
- [ ] Dropdown / Select
- [ ] Slider widget
- [ ] Checkbox / Toggle
- [ ] Table / List view
- [ ] Tab system
- [ ] Modal dialogs
- [ ] Styling system (temas CSS-like)
- [ ] Transitions / animations UI

---

## Animation

### Actions & Tweens
- [x] Position tween com easing
- [x] Rotation tween com easing
- [x] Scale tween com easing
- [x] Color tween
- [x] Alpha tween
- [x] Timed action
- [x] Sprite animation (sprite sheet)
- [x] 12+ funções de easing (linear, quad, cubic, quart, quint, sine, expo, circ, back, elastic, bounce)

### Keyframe Animation
- [x] Translation tracks
- [x] Rotation tracks
- [x] Scale tracks
- [x] Morph tracks
- [x] Animation blending com crossfade
- [x] Animation component
- [~] IK (Inverse Kinematics: cadeia de duas articulações positional/orientada com alvo/polo no AnimatorController; authoring visual ainda pendente)
- [x] Animation state machine
- [~] Animation layers (camadas ordenadas com peso, override/additive e índice por clip no blend de transform/morph, persistência e Lua; editor visual ainda pendente)
- [x] Root motion (AnimatorController consome a translação do alvo raiz, aplica o delta ao controlador e preserva o osso raiz)
- [x] Animation events (callbacks em frames específicos)

### Particles
- [x] Particle system simulation
- [x] Emissão, velocidade, gravidade, lifetime
- [~] Particle collisions
- [~] Particle trails
- [ ] GPU particles
- [ ] Particle editor visual

---

## Networking

- [x] Basic client/server architecture
- [x] Entity replication framework (início)
- [x] Background thread para networking
- [x] UDP com reliable/unreliable channels (RTO, janela congestionável, limites, métricas e simulação de perda/reordenação)
- [x] Snapshot interpolation
- [x] Client-side prediction
- [x] Lag compensation
- [x] Interest management por área/grupo e política de owner/autoridade
- [x] Validação de endpoint, limites anti-abuso e filas limitadas
- [x] Smoke de duas instâncias headless
- [ ] Matchmaking
- [ ] Lobby system
- [ ] Chat in-game
- [ ] Anti-cheat básico

---

## Shader System

- [x] GLSL source compilation
- [x] SPIR-V cross-compilation (GLSL, HLSL, MSL)
- [x] Shader reflection
- [x] Embedded shader headers por backend
- [x] Live recompilation no editor
- [x] Fork de built-in shaders
- [ ] Shader permutations / variants
- [ ] Compute shaders
- [ ] Shader visual editor (node-based)

---

## Voxel Module

- [x] Voxel world management
- [x] Voxel chunk data
- [x] Procedural terrain generation
- [~] Voxel lighting (propagação entre chunks, ponte `MeshSystem::createVoxelMesh`, entidades ECS por chunk e LOD 0–3 por raio/câmera via `updateVoxelWorldFromCamera`; `World::setBlock` marca bordas e recalcula iluminação no próximo update; edição Lua e consumo incremental pelo bridge agora existem, mas a ferramenta visual e seleção integrada ainda faltam)
- [x] Voxel physics (colisor estático dos chunks validado headless)
- [ ] Voxel editor no editor
- [~] LOD de voxels (mesher determinístico, ponte de buffers, dirty rebuild incremental, seleção automática 0–3 no streaming/câmera, skirts laterais de transição e smoke headless direto de índices/faces; validação visual de seams e ferramenta ainda faltam)
- [~] Infinite world streaming (janela determinística com `World::updateAsync`, orçamento de futures, descarte fora da janela, entidades renderizáveis owner-thread e LOD automático por câmera; edição e transições ainda faltam)
- [~] Biomas (classificação/geração determinística; decoração ainda falta)

---

## Editor

### Core
- [x] Dear ImGui interface
- [x] Scene hierarchy panel
- [x] Component inspector
- [x] Asset browser / catalog
- [x] Scene viewport (2D/3D)
- [x] Code editor com syntax highlighting
- [x] Animation timeline editor
- [x] Terrain sculpting tools
- [x] Undo/redo command system (30+ tipos)
- [x] Project management
- [x] Export (source, desktop, web)
- [x] Theme customization
- [x] Crash handler com backtrace

### Gizmos
- [x] Translation gizmo
- [x] Rotation gizmo
- [x] Scale gizmo
- [x] 2D object gizmo
- [x] Anchor gizmo
- [x] Viewport orientation gizmo
- [ ] Multi-select gizmo
- [ ] Snapping grid visual

### AI Assistant
- [x] Chat integration
- [x] Action proposals
- [x] Tool execution
- [x] Conversation persistence
- [x] Rate limiting e retry
- [x] Terrain editing via AI
- [ ] Code generation assistida
- [ ] Auto-complete de componentes

### Preview
- [x] Material preview
- [x] Mesh preview
- [x] Model preview
- [x] Direction indicator
- [ ] Particle preview
- [ ] Animation preview

---

## Platform Support

### Windows
- [x] Win32 window
- [x] XInput gamepad
- [x] D3D11 context
- [x] OpenGL backend
- [x] Vulkan backend
- [ ] Windows Store (UWP) support

### Linux
- [x] X11 window
- [x] evdev gamepad
- [x] OpenGL backend
- [x] Vulkan backend
- [ ] Wayland support

### macOS
- [x] Cocoa window
- [x] Metal backend
- [x] GameController framework
- [ ] Apple Silicon optimization

### iOS
- [x] Metal backend
- [x] GameController framework
- [ ] App Store optimization
- [ ] Touch ID / Face ID integration

### Android
- [x] OpenGL ES 3.0
- [x] JNI bridge
- [ ] Vulkan backend
- [ ] Google Play integration

### HTML5/Web
- [x] WebGL backend
- [x] WebAssembly export
- [ ] WebAudio API
- [ ] Touch events otimizados
- [ ] PWA support

---

## Third-Party Libraries

- [x] Sokol (graphics, audio, app)
- [x] Lua 5.4
- [x] LuaBridge3
- [x] Box2D v3
- [x] Jolt Physics
- [x] SoLoud
- [x] STB (image, truetype, vorbis, rect packing)
- [x] tinygltf
- [x] tinyobjloader
- [x] tinyxml2
- [x] HarfBuzz
- [x] SheenBidi
- [x] NanoSVG
- [x] nlohmann JSON
- [x] Dear ImGui (editor)
- [x] Native File Dialog (editor)
- [x] yaml-cpp (editor)

---

## Build System

- [x] CMake build
- [x] Build scripts Windows (.bat)
- [x] GitHub Actions CI/CD
- [x] Nix dev shell
- [ ] Docker build environment
- [ ] Pre-commit hooks
- [ ] Automated testing framework
- [ ] Benchmarking suite

---

## Templates

- [x] Character Control (3D)
- [x] FPS template
- [x] MOBA click-based
- [x] Mobile game
- [x] Platformer 2D
- [x] Third-person
- [ ] Racing template
- [ ] Puzzle template
- [ ] RTS template
- [ ] Visual novel template

---

## Documentation

- [x] README.md principal
- [x] Roadmap (IDEAS.md)
- [x] External docs (docs.quantum.org)
- [ ] API reference completa
- [ ] Tutoriais step-by-step
- [ ] Video tutorials
- [ ] Exemplos por gênero
- [ ] Migration guide entre versões
- [ ] Contributing guide
- [ ] Changelog detalhado

---

## Melhorias Gerais Sugeridas

### Performance
- [ ] Frustum culling otimizado (octree/BVH)
- [ ] Occlusion culling
- [ ] Texture streaming
- [ ] Mesh LOD automático
- [ ] Draw call batching
- [ ] Profiling tools integrados

### Ferramentas
- [ ] Visual shader editor (node-based)
- [ ] Particle editor visual
- [ ] Animation state machine editor
- [ ] Tilemap editor melhorado
- [ ] Level designer visual
- [ ] Terrain texture painting
- [ ] Foliage painting tool

### Qualidade de Vida
- [ ] Auto-save periódico
- [ ] Project templates customizáveis
- [ ] Snippets de código
- [ ] Search global no editor
- [ ] Keyboard shortcuts customizáveis
- [ ] Multi-monitor support
- [ ] Plugin system para o editor
