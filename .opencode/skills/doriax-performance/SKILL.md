---
name: doriax-performance
description: Performance and concurrency rules for doriax - the engine loop, ThreadPoolManager, the sokol command queue, main-thread-only ECS/render APIs, async asset loading, pools/caches, compile-time caps (MAX_*), build-flag cost (DORIAX_SHARED, NDEBUG, _HAS_EXCEPTIONS=0), and headless profiling. Use when something is slow, stuttering, deadlocking, crashing at shutdown, or when touching threads, pools, futures or frame pacing.
---

# Performance and concurrency

## The loop

`Engine::updateTime = 1.0 / 60.0` (`engine/core/Engine.cpp:81`) is the fixed step and
`maxDeltatime = 0.25` (`:79`) clamps raw delta. Accumulator at `:1074-1091` with
`MAX_UPDATES_PER_FRAME = 100` (`:1036`) and backlog drop at `:1087-1090`; variable
phase `:1096-1100`; draw `:1103-1114`; `oneTimeScenes` retired under `sceneListMutex`
`:1116-1138`. `Engine::framerate` is **measured only - there is no FPS cap in the
engine**; vsync is compile-time `DORIAX_VSYNC_ENABLED`
(`engine/platform/win/DoriaxWin.cpp:23-24`, applied `:266-267`). The editor drives the
same `Engine::systemDraw()` (`editor/App.cpp:2214`). Do not raise `maxDeltatime` to
"fix" physics jitter; that clamps the symptom, not the cause.

## Thread rules, in priority order

1. **Never call sokol/`sg_*` from a worker.** Async threads enqueue commands instead:
   `engine/renders/sokol/SokolCmdQueue.h:232-307` (`add_command_*` on the update
   thread, `execute_commands`/`wait_for_flush` on the render thread, double-buffered
   `m_commands[2]` `:296`). Wrap work in `Engine::startAsyncThread/endAsyncThread`
   (`Engine.cpp:618-647`, thread_local depth `:38-41`); commit
   `Engine.cpp:626-628` -> `SystemRender::commit()` `:1114`, execute at frame start
   `:1049`.
2. **Never mutate ECS or the scene graph off the main thread.** There is no
   thread-safe ECS - `Scene.h`, `engine/core/ecs/*`, `registry/EntityRegistry.h` contain
   no mutexes or deferred queues (only `ComponentTypeRegistry` is locked,
   `engine/core/registry/ComponentManager.cpp:14-28`). Hop with
   `EditorHost::enqueueMainThreadTask` (`editor/EditorHost.h:20`, used at
   `editor/App.cpp:2260,2969`; the editor states the rule at
   `editor/window/ResourcesWindow.cpp:2194-2196`).
3. **Keep locks off blocking work.** `ShaderBuilder.cpp:517-547` takes `cacheMutex`,
   unlocks around disk I/O (`:533`, `:585`), re-locks, and persists outside the lock
   (`:568-571`); its failure set has its own mutex (`:188-190`) precisely because
   `requestShutdown()` waits on futures while holding `cacheMutex`. Lock order is
   documented at `engine/core/Engine.h:245-252`: never block on `drawSemaphore` while
   holding `sceneListMutex`; snapshot-then-iterate (`:261-263`).
4. **Don't block a `ThreadPoolManager` worker on the same pool.** Intra-load
   parallelism therefore uses `std::async`, not the bounded pool
   (`engine/core/io/MeshSystem.cpp:1037-1072` `parallelForIndexed`,
   `decodeGLTFImagesParallel` `:1077-1081`). Pool:
   `engine/core/thread/ThreadPoolManager.h:20-45` (`enqueue` -> `std::future`, default
   `hardware_concurrency()` `:32,:36`), initialised with `hardware_concurrency()-1` at
   `Engine.cpp:904-907`, shut down `:1170-1172`, retunable via
   `Engine::setMaxResourceLoadingThreads` `:653-658`.
5. **Nothing may throw across a task/future boundary** - `_HAS_EXCEPTIONS=0`
   (`doriax-api`). Record failure in state instead (`ShaderBuilder.cpp:611-618`).
6. **Wait on a pending future before erasing it** (`engine/core/pool/SoundPool.cpp:197-206`,
   `TextureDataPool.cpp:384-392`); a move-assign onto a joinable `std::thread` is
   `std::terminate` (`editor/ai/AiService.cpp:869-876`).
7. Jolt uses **its own** job system, not the pool: 10 MB `TempAllocatorImpl` +
   `JobSystemThreadPool` singletons (`engine/core/subsystem/PhysicsSystem.cpp:53-63`),
   referenced per scene at `PhysicsSystem.h:47-49`.

## Async loading pattern

`Engine::asyncLoading` atomic (`Engine.cpp:85`, accessors `:599-616`); pending-future
caches for shaders (`ShaderBuilder.cpp:609-621`), textures
(`TextureDataPool.cpp:106-145`), sounds (`SoundPool.cpp:88-95`) and models
(`MeshSystem.cpp:1214`). Editor/CLI opt in explicitly (`App.cpp:1993,2054`,
`editor/cli/CommandLine.cpp:503,575`), and `prepareForProjectSwitch()` suspends
background resource work - any failure path after it **must** release the suspension
(seen in `editor/Project.cpp` create-project error handling).

## Memory and caches

Generic free-list pool `engine/core/pool/ObjectPool.h` (O(1) acquire/release `:88-125`,
auto-grow 50% `:90`, handle+generation `:56-64`). Entity ids: LIFO free list capped at
`MAX_FREE_LIST_SIZE = 4096` (`engine/core/ecs/EntityManager.h:42`, reuse `:100-136`,
generation bump `:142-158`). `ComponentManager.h:87` stores a raw pointer because
copying the `shared_ptr` cost an atomic pair per access. Pool eviction is
`use_count`-based (`ModelPool.cpp:73-89`, `TexturePool.cpp:154`, `ShaderPool.cpp:913`,
`FontPool.cpp:58`, `SoundPool.cpp:237`); sweep with `Engine::clearPools`/
`clearUnusedPools` (`Engine.cpp:719-735`, called from `editor/Project.cpp:3445`)
instead of destroying resources by hand.

`HybridArray` (`engine/core/util/HybridArray.h`) is a **fixed stack array in engine
builds** (`:48`) but a resizing vector under `#ifdef DORIAX_EDITOR` (`:17-41`): a loop
that overruns a cap is silent in the editor and corrupt in a shipped game.

## Limits that bite (`engine/core/Engine.h`)

`MAX_LIGHTS 6` `:14`, `MAX_LIGHTS_2D 16` `:20`, `MAX_SHADOWCASCADES 4` `:28`,
`MAX_SUBMESHES 16` `:67`, `MAX_TILEMAP_TILES 2048` `:75`, `MAX_SPRITE_FRAMES 128` `:79`,
`MAX_BONES 128` `:88`, `MAX_MORPHTARGETS 8` `:92`, `SSAO_KERNEL_SIZE 32` `:98`,
`MAX_EXTERNAL_BUFFERS 30` `:102`, `MAX_BROADPHASELAYER_3D 6` `:106`, `MAX_SHAPES 10`
`:110`, `MAX_SHAPE_POINTS_2D 16` `:114`, `MAX_SHAPE_VERTICES_3D 256` `:118`,
`MAX_SHAPE_INDICES_3D 768` `:122`. Plus `SSR_MAX_STEPS 128`
(`engine/tools/shaderlib/ssr.frag:19`), `MAX_TERRAIN_NODES 2000000`
(`engine/core/component/TerrainComponent.h:19`), and the 64-component `Signature`
bitset cap (`doriax-api`). Light counts are clamped at runtime
(`RenderSystem.cpp:404-405`, `:429-430`), so exceeding a cap degrades quietly.

## Cost knobs

Culling `RenderSystem::isInsideCamera` (`RenderSystem.h:675`; used `:2940,3115,3461,5416`),
per-chunk tilemap cull (`RenderSystem.cpp:2966`), instancing (`:2899-2902`), LOD
(`MeshSystem.cpp:2701`), import-time texture resize cap (`MeshSystem.cpp:1026-1033`).
**There is no compressed-texture path** - only RGBA8/BGRA8/R8
(`engine/renders/sokol/SokolTexture.cpp:145-152`) with per-texture malloc'd mipmaps
(`:163-183`), and the accepted image formats are png/jpg/bmp/tga/gif/hdr/psd/pic/pnm/svg
(`editor/util/Util.h:23,56`); no ktx/dds/basisu anywhere. Draco mesh decode exists via
tinygltf. Build flags: `DORIAX_SHARED` forced ON for the editor (`CMakeLists.txt:72`),
`NDEBUG`/`_DEBUG` propagate PUBLIC from Jolt (`:550-552`), C++17 (`CMakeLists.txt:740`),
and **no LTO/IPO** (zero `INTERPROCEDURAL` hits).

## Measuring

Built-in monitor `editor/window/ResourceMonitorWindow.h:17-108` (CPU/RAM/GPU/VRAM/temp/
net, caller-fed counters `:102`, 240-sample realloc-free rings `:38-50`, no worker
thread `:22`); samplers `editor/util/SystemStats.h:21-69`, `editor/util/ProcessStats.h`.
The engine has no profiler API - only `Engine::getFramerate` (`Engine.h:234`),
`getDeltatime`, `getSystemTime` (`Engine.cpp:595-597`), `getQueuedResourceCount`
(`:660`), `ResourceProgress` counters (`engine/core/thread/ResourceProgress.h:40-63`)
and `RenderSystem::isAllLoaded` (`RenderSystem.h:688`). Headless measurement runs
`doriax-engine.exe --project <dir> --window 1280x720` (`tools/engine/main.cpp:339-353,
:403-433`); it has no `--frames`/auto-exit flag yet, so count frames in
`Engine::onUpdate`. Lua-side knobs: `setUpdateTimeMS`, `maxDeltatime`, `asyncLoading`,
`setMaxResourceLoadingThreads` (`binding/CoreClassesLua.cpp:236-254`). Logging is not
free: `Log::verbose/warn/error` always format and route (`engine/core/util/Log.cpp:10-45`);
`Log::debug` compiles out under `NDEBUG` (`:24-31`).
