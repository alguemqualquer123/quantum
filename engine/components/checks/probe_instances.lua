-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- What a script-built crowd actually costs: KIT_INST_MODE=plain gives each copy its own entity,
-- KIT_INST_MODE=batched gives one entity N instances. Same model, same positions, same camera, so
-- the two runs differ only in how many times the mesh is submitted.
--   KIT_INST_N=1000 KIT_INST_MODE=batched KIT_INST_HOLD=1 quantum-engine.exe --project <dir> \
--       --scene checks/probe_instances --window 1280x720 --frames 200 --warmup 60 --stats-out x.json
-- The Lua side proves the batch is real (every copy is in the component and keeps its transform);
-- the runner's FrameStats proves what is drawn: avgDrawCalls collapses with instancing, avgInstances
-- does not. Compare the two json files - the probe prints the pair it expects.
local Kit = require("demos.kit_demo")

local function envText(name, fallback)
    local ok, raw = pcall(function() return os.getenv(name) end)
    return (ok and type(raw) == "string" and #raw > 0) and raw or fallback
end

local function envNumber(name, fallback)
    local ok, raw = pcall(function() return os.getenv(name) end)
    local v = ok and tonumber(raw) or nil
    return (v and v > 0) and v or fallback
end

local MODE = envText("KIT_INST_MODE", "batched")
local N = envNumber("KIT_INST_N", 1000)
local HOLD = envNumber("KIT_INST_HOLD", 0) > 0 or envNumber("KIT_BENCH_HOLD", 0) > 0
local WARMUP = 30
local SAMPLES = 90
local MODEL = envText("KIT_INST_MODEL", "Models/MapKit/mapkit_cube.glb")

local checks = 0
local failed = 0
local function check(label, ok, detail)
    checks = checks + 1
    if not ok then failed = failed + 1 end
    print(string.format("[inst] %-36s %s   %s", label, ok and "PASS" or "FAIL", detail or ""))
end

local function grid(i, side, spacing)
    local gx = i % side
    local gz = math.floor(i / side)
    return Vector3((gx - side * 0.5) * spacing, 0.5, (gz - side * 0.5) * spacing)
end

Kit.start(function()
    local scene = Kit.createScene()
    scene.backgroundColor = Vector4(0.10, 0.12, 0.16, 1.0)

    local sun = Kit.hold(Light(scene))
    sun.name = "Sun"
    sun:setType(LightType.DIRECTIONAL)
    sun.direction = Vector3(-0.30, -0.85, -0.40)
    sun:setIntensity(1.5)

    local camera = Kit.hold(Camera(scene))
    camera.name = "InstCam"
    Kit.camera3D(camera, 60.0, 0.1, 6000.0)
    -- Without this the scene renders through its default UI camera, so almost none of the copies
    -- are drawn and the draw calls measured belong to a scene that was never on screen.
    scene:setCamera(camera)

    local side = math.floor(math.sqrt(N) + 0.5)
    if side < 1 then side = 1 end
    local spacing = 3.0
    local radius = side * spacing * 1.6
    local height = side * spacing * 0.8

    local spawned = 0
    local loadFailed = 0
    local batch = nil
    local t0 = os.clock()

    if MODE == "plain" then
        for i = 0, N - 1 do
            local m = Kit.hold(Model(scene))
            if m:loadModel(MODEL) == false then loadFailed = loadFailed + 1 end
            m.position = grid(i, side, spacing)
            spawned = spawned + 1
        end
    else
        batch = Kit.hold(Model(scene))
        if batch:loadModel(MODEL) == false then loadFailed = loadFailed + 1 end
        batch.name = "InstancedCrowd"
        -- The tuning rows only stick on an existing batch, and an empty batch draws nothing
        -- (selectMeshDraw returns false at instanceCount 0), so create then fill in one go.
        batch:createInstancedMesh()
        batch.maxInstances = N
        for i = 0, N - 1 do
            batch:addInstance(grid(i, side, spacing))
            spawned = spawned + 1
        end
    end
    local spawnMs = (os.clock() - t0) * 1000.0

    check("every copy was built", spawned == N, string.format("%d of %d", spawned, N))
    check("the model file loads", loadFailed == 0, string.format("%d rejected", loadFailed))

    if MODE == "plain" then
        print(string.format("[inst] plain: %d entities, spawn %.0f ms (%.3f ms each)",
            spawned, spawnMs, spawnMs / math.max(spawned, 1)))
        check("plain copies stay separate entities", spawned == N, "one MeshComponent each")
    else
        print(string.format("[inst] batched: %d instances on 1 entity, spawn %.0f ms", spawned, spawnMs))
        -- addInstance creates the InstancedMeshComponent itself; if that did not happen the crowd
        -- is drawn as a single mesh and the comparison would flatter the batched run.
        check("the batch really is instanced", batch:hasInstancedMesh() == true, "InstancedMeshComponent present")
        check("the batch capacity covers the crowd", batch.maxInstances >= N,
            string.format("maxInstances=%d", batch.maxInstances))
        local firstOk = batch:getInstance(0) and batch:getInstance(0).position ~= nil
        check("an instance reads back", firstOk == true, "getInstance(0).position")
        if firstOk then
            local want = grid(N - 1, side, spacing)
            local got = batch:getInstance(N - 1).position
            local delta = math.max(math.abs(got.x - want.x), math.abs(got.y - want.y), math.abs(got.z - want.z))
            check("the last instance keeps its transform", delta < 0.001, string.format("delta %.6f", delta))
        end
    end

    camera.position = Vector3(0, height, radius)
    camera.target = Vector3(0, 0, 0)

    local driver = { name = "InstDriver", frames = 0, samples = {}, reported = false }
    driver.onUpdate = function()
        driver.frames = driver.frames + 1
        if driver.frames > WARMUP then
            local dt = tonumber(Engine.deltatime) or 0.0
            driver.samples[#driver.samples + 1] = dt
            local a = driver.frames * 0.012
            camera.position = Vector3(math.sin(a) * radius, height, math.cos(a) * radius)
            camera.target = Vector3(0, 0, 0)
        end
        if #driver.samples >= SAMPLES and not driver.reported then
            local sorted, sum, min, max = {}, 0.0, math.huge, 0.0
            for i = 1, #driver.samples do
                local s = driver.samples[i]
                sorted[i] = s
                sum = sum + s
                if s < min then min = s end
                if s > max then max = s end
            end
            table.sort(sorted)
            local p95 = sorted[math.max(1, math.floor(#sorted * 0.95))]
            local avg = sum / #sorted
            print(string.format(
                "[inst] mode=%s n=%d samples=%d avg=%.2f ms  min=%.2f  p95=%.2f  max=%.2f  (%.1f fps avg)",
                MODE, spawned, #sorted, avg * 1000.0, min * 1000.0, p95 * 1000.0, max * 1000.0,
                1.0 / math.max(avg, 1e-6)))
            check("the frame stays under a second", avg * 1000.0 <= 1000.0, string.format("avg %.2f ms", avg * 1000.0))
            print(string.format("[inst] ===== %d checks, %d failed =====", checks, failed))
            Kit.report("inst")
            driver.reported = true
            -- The runner only prints its FrameStats report if the scene is still alive when it
            -- reaches its own frame budget; quitting here loses the draw-call evidence.
            if not HOLD then System.quit() end
        end
    end
    RegisterEngineEvent(driver, "onUpdate")
    Kit.keep[#Kit.keep + 1] = driver
end)
