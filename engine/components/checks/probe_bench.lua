-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Performance baseline: what a frame costs once the scene holds N rendered entities. The count
-- comes from the environment, so one file draws the whole curve:
--   KIT_BENCH_N=1000 quantum-engine.exe --project <dir> --scene checks/probe_bench --window 1280x720
-- Two numbers, two different failures: the spawn phase is the hitch felt when a level loads, the
-- steady frames are the fps that are then kept. The camera orbits so a static, fully-frustum-
-- rejected view cannot flatter the result.
--   KIT_BENCH_HOLD=1 keeps the run alive after the report, so the runner's own --frames N
--   --stats-out x.json pass can finish and print Engine::FrameStats (draw calls, instances,
--   triangles) - the evidence that the N entities are actually drawn, not just present.
local Kit = require("demos.kit_demo")

local function envNumber(name, fallback)
    local ok, raw = pcall(function() return os.getenv(name) end)
    local v = ok and tonumber(raw) or nil
    return (v and v > 0) and v or fallback
end

local N = envNumber("KIT_BENCH_N", 1000)
local HOLD = envNumber("KIT_BENCH_HOLD", 0) > 0
local WARMUP = 30
local SAMPLES = 90
local MODEL = "Models/MapKit/mapkit_cube.glb"

local checks = 0
local failed = 0
local function check(label, ok, detail)
    checks = checks + 1
    if not ok then failed = failed + 1 end
    print(string.format("[bench] %-34s %s   %s", label, ok and "PASS" or "FAIL", detail or ""))
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
    camera.name = "BenchCam"
    Kit.camera3D(camera, 60.0, 0.1, 6000.0)
    -- Without this the scene renders through its default UI camera, so almost none of the N
    -- entities are drawn and the frame cost measured is not the one being benchmarked.
    scene:setCamera(camera)

    local side = math.floor(math.sqrt(N) + 0.5)
    if side < 1 then side = 1 end
    local spacing = 3.0
    local radius = side * spacing * 1.6
    local height = side * spacing * 0.8

    local spawned = 0
    local loadFailed = 0
    local t0 = os.clock()
    for i = 0, N - 1 do
        local m = Kit.hold(Model(scene))
        local ok = m:loadModel(MODEL)
        if ok == false then loadFailed = loadFailed + 1 end
        local gx = i % side
        local gz = math.floor(i / side)
        m.position = Vector3((gx - side * 0.5) * spacing, 0.5, (gz - side * 0.5) * spacing)
        spawned = spawned + 1
    end
    local spawnMs = (os.clock() - t0) * 1000.0

    check("every entity was built", spawned == N, string.format("%d of %d", spawned, N))
    check("the model file loads", loadFailed == 0, string.format("%d rejected", loadFailed))
    print(string.format("[bench] spawn %d entities: %.0f ms (%.3f ms each)", spawned, spawnMs,
        spawnMs / math.max(spawned, 1)))

    camera.position = Vector3(0, height, radius)
    camera.target = Vector3(0, 0, 0)

    local driver = { name = "BenchDriver", frames = 0, samples = {}, reported = false }
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
            local sorted = {}
            local sum, min, max = 0.0, math.huge, 0.0
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
                "[bench] n=%d samples=%d avg=%.2f ms  min=%.2f  p95=%.2f  max=%.2f  (%.1f fps avg)",
                spawned, #sorted, avg * 1000.0, min * 1000.0, p95 * 1000.0, max * 1000.0, 1.0 / math.max(avg, 1e-6)))
            check("steady frame holds 30 fps", avg * 1000.0 <= 33.4, string.format("avg %.2f ms", avg * 1000.0))
            check("no frame is a hitch", max * 1000.0 <= 100.0, string.format("worst %.2f ms", max * 1000.0))
            print(string.format("[bench] ===== %d checks, %d failed =====", checks, failed))
            Kit.report("bench")
            driver.reported = true
            -- The measurement pass needs the runner to end the run, otherwise its FrameStats
            -- report is never reached and the draw-call evidence is lost.
            if not HOLD then System.quit() end
        end
    end
    RegisterEngineEvent(driver, "onUpdate")
    Kit.keep[#Kit.keep + 1] = driver
end)
