---
name: quantum-performance
description: Performance and concurrency rules for Quantum - the engine loop, ThreadPoolManager, the sokol command queue, main-thread-only ECS/render APIs, async asset loading, pools/caches, compile-time caps (MAX_*), build-flag cost (QUANTUM_SHARED, NDEBUG, _HAS_EXCEPTIONS=0), and headless profiling. Use when something is slow, stuttering, deadlocking, crashing at shutdown, or when touching threads, pools, futures or frame pacing.
---

# Performance and concurrency

## The loop

`Engine::updateTime = 1.0 / 60.0` (`engine/core/Engine.cpp:92`) is the fixed step and
`maxDeltatime = 0.25` (`:90`) clamps raw delta. Accumulator at `:1232-1255` with
`MAX_UPDATES_PER_FRAME = 100` (`:1178`) and backlog drop at `:1254-1255`; variable
phase `:1268-1272`; draw `:1278-1300`; `oneTimeScenes` retired under `sceneListMutex`
`:1314-1330`. `Engine::framerate` is **measured only - there is no FPS cap in the
engine**; vsync is compile-time `QUANTUM_VSYNC_ENABLED`
(`engine/platform/win/QuantumWin.cpp:28-29`, applied `:277`). The editor drives the
same `Engine::systemDraw()` (`editor/App.cpp:3013`). Do not raise `maxDeltatime` to
"fix" physics jitter; that clamps the symptom, not the cause.

## Thread rules, in priority order

1. **Never call sokol/`sg_*` from a worker.** Async threads enqueue commands instead:
   `engine/renders/sokol/SokolCmdQueue.h:239-307` (`add_command_*` on the update
   thread, `execute_commands`/`wait_for_flush` on the render thread, double-buffered
   `m_commands[2]` `:296`). Wrap work in `Engine::startAsyncThread/endAsyncThread`
   (`Engine.cpp:728-740`, thread_local depth `:43,:46`); commit
   `Engine.cpp:1306-1308` -> `SystemRender::commit()`, executed at frame start by
   `SokolCmdQueue::execute_commands()` (`engine/renders/sokol/SokolSystem.cpp:87`).
2. **Never mutate ECS or the scene graph off the main thread.** There is no
   thread-safe ECS - `Scene.h`, `engine/core/ecs/*` contain no mutexes or deferred
   queues (only `ComponentTypeRegistry` is locked,
   `engine/core/ecs/ComponentManager.cpp:15,:19`). Hop with
   `EditorHost::enqueueMainThreadTask` (`editor/EditorHost.h:20`, used at
   `editor/App.cpp:3256,3297`; the editor states the rule at
   `editor/window/ResourcesWindow.cpp:2354-2356`).
3. **Keep locks off blocking work.** `ShaderBuilder.cpp:533` takes `cacheMutex`,
   unlocks around disk I/O (`:548`, `:583`), re-locks, and persists outside the lock
   (`:584-586`); its failure set has its own mutex (`:190`) precisely because
   `requestShutdown()` waits on futures while holding `cacheMutex`. Lock order is
   documented at `engine/core/Engine.h:300-307`: never block on `drawSemaphore` while
   holding `sceneListMutex`; snapshot-then-iterate (`:316`).
4. **Don't block a `ThreadPoolManager` worker on the same pool.** Intra-load
   parallelism therefore uses `std::async`, not the bounded pool
   (`engine/core/subsystem/MeshSystem.cpp:1236` `parallelForIndexed`,
   `decodeGLTFImagesParallel` `:1269`). Pool:
   `engine/core/thread/ThreadPoolManager.h:40-63` (`enqueue` -> `std::future`, default
   `hardware_concurrency()` `:32,:36`), initialised with `hardware_concurrency()-1` at
   `Engine.cpp:1035-1036`, shut down `:1372`, retunable via
   `Engine::setMaxResourceLoadingThreads` `:767`.
5. **Nothing may throw across a task/future boundary** - `_HAS_EXCEPTIONS=0`
   (`quantum-api`) leaves `std::packaged_task` with no exception slot, so a throw that
   escapes a worker lambda ends the whole process in `abort()` (the "Debug Error!"
   dialog). Record the failure in a value instead: the asset pools return
   `TextureBuildResult` / `SoundBuildResult` whose `error` string `loadFromFile` maps
   onto `ResourceLoadState::Failed` + `errorMessage`, which the callers already handle
   (`Texture::load`, `AudioSystem::loadSoundSample`). `ShaderBuilder` and `MeshSystem`
   catch inside their task body instead.
6. **Wait on a pending future before erasing it** (`engine/core/pool/SoundPool.cpp:237-245`,
   `TextureDataPool.cpp:410-417`); a move-assign onto a joinable `std::thread` is
   `std::terminate` (`editor/ai/AiService.cpp:857-859`).
7. Jolt uses **its own** job system, not the pool: 10 MB `TempAllocatorImpl` +
   `JobSystemThreadPool` singletons (`engine/core/subsystem/PhysicsSystem.cpp:110-120`),
   referenced per scene at `PhysicsSystem.h:67-68`.

## Async loading pattern

`Engine::asyncLoading` atomic (`Engine.cpp:96`, accessors `:709,:720`); pending-future
caches for shaders (`ShaderBuilder.cpp:565-589`), textures
(`TextureDataPool.cpp:120-163`), sounds (`SoundPool.cpp:61-102`) and models
(`MeshSystem.cpp:141` `getPendingModelLoads`, used `:1108-1177`). Editor/CLI opt in
explicitly (`App.cpp:2685,2707`,
`editor/cli/CommandLine.cpp:549,621`), and `prepareForProjectSwitch()` suspends
background resource work - any failure path after it **must** release the suspension
(seen in `editor/Project.cpp` create-project error handling). A mesh whose GPU
allocation failed used to rerun the whole `RenderSystem::loadMesh` build every frame
because `loadCalled` stayed false; `MeshComponent::loadRetries` / `loadRetryDelay` now
pace a stuck mesh to one attempt every 30 frames after 8 consecutive failures and name
it once in a `Log::warn` (search `STUCK_ATTEMPTS` in
`engine/core/subsystem/RenderSystem.cpp`). `meshLoadsPerFrame` above 0 in a benchmark is
the signature of that bug.

## Memory and caches

Generic free-list pool `engine/core/pool/ObjectPool.h` (O(1) acquire/release `:88-125`,
auto-grow 50% `:87`, handle+generation `:56-64`). Entity ids: LIFO free list capped at
`MAX_FREE_LIST_SIZE = 4096` (`engine/core/ecs/EntityManager.h:42`, reuse `:102-136`,
generation bump `:147-148`). `ComponentManager.h:87-92` stores a raw pointer because
copying the `shared_ptr` cost an atomic pair per access. Pool eviction is
`use_count`-based (`ModelPool.cpp:76-85`, `TexturePool.cpp:105`, `ShaderPool.cpp:766`,
`FontPool.cpp:48`, `SoundPool.cpp:253`); sweep with `Engine::clearPools`/
`clearUnusedPools` (`Engine.cpp:833,843`, called from `editor/Project.cpp:4512`)
instead of destroying resources by hand.

`HybridArray` (`engine/core/util/HybridArray.h`) is a **fixed stack array in engine
builds** (`:48`) but a resizing vector under `#ifdef QUANTUM_EDITOR` (`:17-41`): a loop
that overruns a cap is silent in the editor and corrupt in a shipped game.

`ComponentArray` stores its components **by value** in a `std::vector<T>`
(`engine/core/ecs/ComponentArray.h:39`), and `MeshComponent` is 48 KB - four `Submesh` of 9,360
plus 96 `Matrix4` plus 20 `ExternalBuffer`. So an append is a 48 KB construction *and* the vector
re-commits the whole array on every growth step. Measured on this machine, one `Mesh(scene)` cost
**0.130 ms** under the vector's own 1.5x policy, which is why `emplace` grows by eight from a first
block of 256 (`FIRST_CAPACITY` / `GROWTH_FACTOR` `:36-37`, `growOnce` `:74`): **0.028 ms**, of which
0.012 ms is simply touching the 48 KB. Keeping the same bytes behind three heap blocks was measured
and is **worse** (a large allocation always hands back freshly committed pages, so the copies are
replaced by the same page cost) - do not fix this by indirection. `remove` `:123` still erases and
shifts every later element over the hole, so deleting is O(scene) in copies: measured 0.145 ms per
mesh at 2000 when removing from the front, 0.019 ms from the back. `tests/scale/spawnprobe` has
`costrow` (creation) and `costdel` (both ends of the deletion) for this.

GPU object pools are fixed at `sg_setup` and **never grow**
(`engine/renders/sokol/SokolSystem.cpp:68-73` - buffer 16384, image 2048, sampler 2048,
view 4096, pipeline 8192, shader 1024). Exhausting one prints a `*_POOL_EXHAUSTED`
validation error and the mesh simply never loads, so a pool is a hard scene-size limit,
not a perf knob. A mesh keeps its own vertex and index buffer, so `buffer_pool_size`
caps the mesh count (16384 slots is a little over 8000 meshes). Pipelines no longer
count per mesh: `SokolObject` takes them from a ref-counted shared cache keyed by vertex
layout + fixed-function state (`acquirePipeline` / `releasePipeline` /
`retainAll` / `clearPipelineCache` in `engine/renders/sokol/SokolObject.cpp`), so N
meshes sharing a material state hold one `sg_pipeline`. `beginLoad()` releases the
handles of a half-built attempt - that leak used to accelerate the exhaustion.

## Limits that bite (`engine/core/Engine.h`)

`MAX_LIGHTS 6` `:14`, `MAX_LIGHTS_2D 16` `:20`, `MAX_SHADOWCASCADES 4` `:28`,
`MAX_SUBMESHES 4` `:65`, `MAX_TILEMAP_TILESRECT 64` `:74`, `MAX_TILEMAP_TILES 512` `:78`,
`MAX_SPRITE_FRAMES 32` `:82`, `MAX_BONES 96` `:91`, `MAX_MORPHTARGETS 8` `:95`,
`SSAO_KERNEL_SIZE 32` `:101`, `MAX_EXTERNAL_BUFFERS 20` `:110`,
`MAX_BROADPHASELAYER_3D 6` `:114`, `MAX_SHAPES_2D 4` `:118`, `MAX_SHAPE_POINTS_2D 8` `:122`,
`MAX_SHAPES_3D 4` `:126`, `MAX_SHAPE_VERTICES_3D 64` `:130`,
`MAX_SHAPE_INDICES_3D 192` `:134`. Plus `SSR_MAX_STEPS 128`
(`engine/shaderlib/ssr.frag:19`), `MAX_TERRAIN_NODES 2000000`
(`engine/core/component/TerrainComponent.h:19`), and the 64-component `Signature`
bitset cap (`quantum-api`). Light counts are clamped at runtime
(`RenderSystem.cpp:648-650`, `:739-740`), so exceeding a cap degrades quietly.

## Cost knobs

Culling `RenderSystem::isInsideCamera` (declarations `RenderSystem.h:1100`, definitions
`:9126-9163`; used per mesh `RenderSystem.cpp:4220,4414,4791`, per instanced batch `:4039`,
per-chunk tilemap `:9080`), LOD
(`RenderSystem::selectMeshLod` `:3885`, fed by `updateMeshLods` `:3643`, gated by the
scene-wide `SceneSettings::meshLodEnabled = true` default `engine/core/SceneSettings.h:61`
plus per-mesh `MeshComponent::lodEnabled` (`MeshComponent.h:228`, also default true);
toggling either sets `meshLodToggled`
`RenderSystem.cpp:10395` and rebuilds every view), import-time
texture resize cap (`MeshSystem.cpp:1211-1214`, `g_imageDecodeMaxDimension` `:43`).
**Scene render distance** (`SceneSettings::renderDistance`, plus the editor-only
`AppSettings::viewportRenderDistance` override) is the culling knob a designer can actually set:
`RenderSystem::getRenderedNearFar` (`RenderSystem.cpp:9521`) clamps the far before the projection is
built, so the frustum's far plane - the one `isInsideCamera` tests - moves with it, and the same
clamp tightens shadow near/far (`configureLightShadowNearFar`), foliage `drawDistance` and the CDLOD
ranges (`MeshSystem::updateTerrainAutoRanges`). Measured on 1000 cubes around a 170 m camera:
uncapped 1000 draw calls / 12000 triangles / 6.59 ms draw, capped at 150 m 230 / 2756 / 2.77 ms.
It is perspective-only (a `farClip == 0` "no far plane" camera and 2D/UI cameras keep their own
clips) and it must dirty every camera when it changes (`renderDistanceChanged` above the camera loop,
`RenderSystem.cpp:10293`), because a projection is rebuilt only while its camera is dirty.
**A scene-wide shader-variant change rebuilds every mesh in one frame, and that is a known,
measured hitch.** Fog/SSAO/DOF/IBL, `depthPrepassEnabled`, `defaultMeshShader`, IBL and the first
shadow-casting 2D light or reflection probe are baked into each mesh's variant, so
`RenderSystem::needReloadMeshes` (`RenderSystem.cpp:9196`) flags every `MeshComponent` and the next
frame destroys and re-uploads all of them. `tests/scale/fpsprobe/lua/reloadcost.lua` measures it
(Release, 1280x720, SSAO flipped at frame 240): 4k cube props take the frame from 7.8 ms to 69 ms,
10k props from 20 ms to 248 ms (~226 ms of rebuild). A mesh carrying `needReload` is *skipped by
`drawMesh`* (`:4214`) until it reloads, which is why the rebuild cannot simply be spread over
frames. Spreading it was tried and reverted: a flag + cursor queue draining 256 rebuilds per frame
before the entity scan lowered the 10k peak only to 166 ms (that frame also pays the one-time SSAO
target/variant setup), raised the total work (144 ms → 162-305 ms for the same 4k scene, because
both variants stay resident while it migrates), and produced a sokol validation error the old path
never prints - `Pipeline of type 4 is not usable (id 0, state -1); the pass that asked for it has
no shader variant`. Anyone retrying this must first make a half-migrated scene draw correctly: the
fix belongs in the pipeline/variant resolution, not in the scheduling.
**The foliage frame was being spent on the CPU rebuild of the instances, not on the GPU.**
`RenderSystem::updateInstancedMesh` (`RenderSystem.cpp:9360`) re-derives every visible instance's
matrix and merges its bound, and the entity walk called it on any frame the fade eye moved at all -
which is every frame the camera walks. Measured with a temporary timer: 0.2 µs per instance,
26 ms per frame over the 30k instances of a 400 m terrain with seven grass layers
(`tests/scale/fpsprobe`, Release, `FPS_PROPS=0`), while `drawSceneMs` was only 6 ms. Two thirds of
it was `mesh.aabb.merge(instanceMatrix * mesh.verticesAABB)` - eight corner transforms per instance -
which is now two point merges against the instance's circumradius (looser but still conservative,
since the sphere contains the rotated box), and the exact `fadeEyeLocal != lastFadeCullEye` test in
the walk (`:10490`) tolerates 2% of the fade range. That run went 40 ms → 30 ms per frame
(23.7 → 33.4 fps) and `updateMs` 33.7 → 11.7 ms. An infinite `verticesAABB` must still leave the
mesh uncullable (`setInfinite()` after the loop): the radius merge only bounds a finite box.
**One draw call per object is the wall that is left.** 12k cube props as separate `Model` entities
submit 11.8k draw calls, which measured 21 ms of `drawSceneMs` in Release (~1.8 µs a call); nothing
batches identical meshes across entities, so `avgDrawCalls` is a scene-size limit until dynamic
batching exists. `SceneSettings::renderDistance` is what buys it back: the same 12k-prop plus
grass scene at 90 m draws 1271 calls and holds 30.8 fps.
**There is no compressed-texture path** - only RGBA8/BGRA8/R8
(`engine/renders/sokol/SokolTexture.cpp:149-156`) with per-texture malloc'd mipmaps
(`:170-194`), and the accepted image formats are png/jpg/bmp/tga/gif/hdr/psd/pic/pnm/svg
(`editor/util/Util.h:23,60`); no ktx/dds/basisu anywhere. Draco mesh decode exists via
tinygltf. Build flags: `QUANTUM_SHARED` forced ON for the editor (`CMakeLists.txt:72`),
`NDEBUG`/`_DEBUG` propagate PUBLIC from Jolt (`engine/libs/joltphysics/CMakeLists.txt:551-552`), C++17 (`engine/CMakeLists.txt:5`, applied `CMakeLists.txt:881`),
and **no LTO/IPO** (zero `INTERPROCEDURAL` hits).

## Measuring

Built-in monitor `editor/window/ResourceMonitorWindow.h:23-109` (CPU/RAM/GPU/VRAM/temp/
net, caller-fed counters `:61`, 240-sample realloc-free rings `:38-52`, no worker
thread `:20-22`); samplers `editor/util/SystemStats.h:21-69`, `editor/util/ProcessStats.h`.
The engine has no profiler API - only `Engine::getFramerate` (`Engine.h:407`, defined
`Engine.cpp:600`), `getDeltatime` (`Engine.h:408`), `getSystemTime` (`:429`,
`Engine.cpp:705`), `getQueuedResourceCount`
(`:774`), `ResourceProgress` counters (`engine/core/thread/ResourceProgress.h:40-63`)
and `RenderSystem::isAllLoaded` (`RenderSystem.h:961`).
`Engine::FrameStats` (`Engine.h:215`, counters `:228-242`, `beginFrameStats` /
`endFrameStats` `Engine.cpp:612,:617`) is the in-engine per-frame counter: draw calls,
instances, triangles, mesh loads/drops, the four frame phases and up to 32 named
systems. Headless measurement runs `quantum-engine.exe --project <dir> --window
1280x720 --frames N [--warmup M] [--name L] [--stats-out x.json]`
(`tools/engine/main.cpp:327-470`, flags `:588-604`): it samples FrameStats once per
presented frame, drops the first `--warmup` frames (default a quarter of `--frames`, so
an async load settles), prints one JSON report (`wallFps`, `avgFps`, `avgDrawCalls`,
`avgInstances`, `avgTriangles`, `meshLoadsPerFrame`, `meshDropsPerFrame`,
`fixedUpdateMs`, `updateMs`, `drawSceneMs`, `commitMs`, `engineFrameMs`,
`slowestSystems[]`) and quits - a before/after pair is one diff. Three measurement
traps: a **Debug** build pays sokol validation plus unoptimized MSVC, so a draw call
costs ~8.5 µs there against ~3.1 µs in Release and only Release numbers are honest; the
machine is shared with other agents' builds, so check CPU load before blaming a
regression; and a `VAR=0` env knob arrives as the *string* `"0"`, which is **truthy in
Lua** - `STRESS_SHADOWS=0` disables nothing. Lua-side knobs: `setUpdateTimeMS`,
`maxDeltatime`, `asyncLoading`, `setMaxResourceLoadingThreads`
(`binding/CoreClassesLua.cpp:401-419`). Logging is not
free: `Log::verbose/warn/error` always format and route (`engine/core/Log.cpp:10-45`);
`Log::debug` compiles out under `NDEBUG` (`:24-31`).
