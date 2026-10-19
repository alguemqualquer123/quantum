# Doriax Engine - TODO & Feature Map

## Core Engine

### ECS (Entity Component System)
- [x] ComponentManager com registro tipado
- [x] EntityManager com free-list recycling
- [x] Signature bitset para matching de subsystems
- [x] SubSystem base class (load, draw, destroy, update, fixedUpdate)
- [x] 52+ tipos de componentes
- [ ] Object pooling para entidades
- [ ] Componente de tags para filtragem rápida

### Engine Core
- [x] Game loop com delta time
- [x] Scene management (múltiplas cenas)
- [x] Scaling modes (fixed width, fixed height, fixed resolution)
- [x] Async resource loading com thread pool
- [x] Resource pool management
- [ ] Hot-reload de cenas
- [ ] Save/Load de estado do jogo
- [ ] Sistema de eventos global (event bus)

---

## Rendering

### Pipeline Principal
- [x] Deferred PBR rendering
- [x] Forward rendering
- [x] G-Buffer pass
- [x] Deferred lighting pass
- [x] Post-process chain
- [x] Fixed resolution upscale (blit)
- [x] HDR render targets (RGBA16F) - **Implementado**
- [x] Tone mapping (ACES, Reinhard, AgX, PBR Neutral) - **Implementado**
- [x] Bloom multi-pass (threshold + downsample + upsample + composite) - **Implementado**
- [x] Auto-exposure (eye adaptation) - **Implementado**
- [x] Color grading 3D LUT - **Implementado**
- [x] Temporal anti-aliasing (TAA) - **Implementado**
- [x] FXAA fallback - **Implementado**
- [x] Velocity buffer (motion vectors) - **Implementado**
- [x] Motion blur - **Implementado**
- [x] Depth of field (CoC + blur + composite) - **Implementado**
- [x] Chromatic aberration - **Implementado**
- [x] Vignette - **Implementado**
- [x] Film grain - **Implementado**
- [x] Contact shadows (screen-space) - **Implementado**
- [x] Hi-Z SSR acceleration - **Implementado**
- [ ] Clustered forward rendering
- [ ] Temporal anti-aliasing (TAA)
- [ ] HDR bloom
- [ ] Motion blur
- [ ] Depth of field (DOF)
- [ ] Color grading / LUT
- [ ] Screen-space global illumination (SSGI)
- [ ] Volumetric lighting (ray marching)
- [ ] Ray tracing (futuro)

### Shadows
- [x] Cascaded shadow maps (directional light)
- [x] Omnidirectional shadow maps (point lights)
- [x] Spot light shadows
- [x] 2D shadow rendering (light2D + occluders)
- [ ] Soft shadows (PCF melhorado / VSM)
- [ ] Shadow bias auto-ajustável

### SSAO & SSR
- [x] Screen-space ambient occlusion (SSAO)
- [x] SSAO blur
- [x] Screen-space reflections (SSR)
- [x] SSR blur
- [ ] GTAO (Ground Truth AO)
- [ ] SSR com fallback para reflection probes

### Sky & Environment
- [x] Skybox rendering
- [x] Reflection probes (IBL)
- [x] IBL environment map processing (irradiance + prefiltered)
- [ ] Procedural sky (sol, nuvens, atmosfera)
- [ ] Dynamic time of day
- [ ] Volumetric clouds

### Fog
- [x] Volumetric fog component
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
- [ ] Controllers (buoyancy, wind)
- [ ] Trigger zones otimizadas
- [ ] Debug draw para física 2D

### 3D Physics (Jolt Physics)
- [x] Rigid body
- [x] Shapes: box, sphere, capsule, convex hull, mesh
- [x] Joints: fixed, distance, hinge, cone, 6DOF, path, gear, rack-pinion, pulley, swing-twist
- [x] Contact callbacks
- [x] Raycasting 3D
- [x] Shape casting
- [ ] Vehicle physics (car, aircraft)
- [ ] Soft body physics
- [ ] Ragdoll system
- [ ] Physics material (friction, restitution por material)
- [ ] Collision layers / filtering rules

---

## Audio

### SoLoud Backend
- [x] WAV streaming
- [x] Spatial 3D audio
- [x] Global volume control
- [x] Pause/Resume
- [x] Filtros: reverb, echo, flanger, chorus, bassboost, DC removal, equalizer
- [ ] OGG Vorbis streaming
- [ ] Music streaming com crossfade
- [ ] Audio occlusion / obstruction
- [ ] Ambience system
- [ ] Audio bus / mixer hierarchy
- [ ] Doppler effect

---

## Input

- [x] Keyboard (todas as teclas)
- [x] Mouse (posição, botões, scroll)
- [x] Touch (multi-touch)
- [x] Gamepad (XInput Windows, evdev Linux, GameController Apple)
- [x] Gamepad mapping database (SDL format)
- [ ] Input mapping / rebinding system
- [ ] Input actions (abstract input bindings)
- [ ] Vibration / haptic feedback
- [ ] Gesture recognition
- [ ] IME (Input Method Editor) para CJK

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
- [ ] Debugger Lua (breakpoints, step, watch)
- [ ] Hot-reload de scripts
- [ ] Coroutines better integration
- [ ] Script profiling

### C++ Scripting
- [x] ScriptBase class
- [ ] C++ hot-reload com DLL swapping

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
- [ ] IK (Inverse Kinematics)
- [ ] Animation state machine
- [ ] Animation layers
- [ ] Root motion
- [ ] Animation events (callbacks em frames específicos)

### Particles
- [x] Particle system simulation
- [x] Emissão, velocidade, gravidade, lifetime
- [ ] Particle collisions
- [ ] Particle trails
- [ ] GPU particles
- [ ] Particle editor visual

---

## Networking

- [x] Basic client/server architecture
- [x] Entity replication framework (início)
- [x] Background thread para networking
- [ ] UDP com reliable/unreliable channels
- [ ] Snapshot interpolation
- [ ] Client-side prediction
- [ ] Lag compensation
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
- [ ] Voxel lighting (propagação de luz)
- [ ] Voxel physics
- [ ] Voxel editor no editor
- [ ] LOD de voxels
- [ ] Infinite world streaming
- [ ] Biomas

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
- [x] External docs (docs.doriax.org)
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
