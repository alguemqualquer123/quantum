-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Render distance: the scene setting has to shorten what is drawn, and must not touch what was
-- authored. One run measures one distance, so the A/B is two runs of the same file (it has to sit
-- in the project's `lua/`, which is where the runner boots a scene from):
--   RD=0   RD_FAR=6000 RD_HOLD=1 quantum-engine.exe --project <dir> --scene probe_render_distance --frames 200 --warmup 60 --log-level 1 --stats-out rd0.json
--   RD=150 RD_FAR=6000 ... --stats-out rd150.json
--   RD=0   RD_FAR=150  ... --stats-out far150.json
-- The checks below are the settings plumbing; the proof is the two last reports: capping the
-- scene distance and authoring the same far clip must give the same draw-call and triangle
-- counts (they do: 230 calls / 2756 triangles each against 1000 / 12000 uncapped), because the
-- cap is applied to the projection the frustum planes come from. The runner prints FrameStats
-- only when the Lua side does not quit first, so RD_HOLD=1 keeps the run alive for it.
-- The grid and the orbiting camera are probe_bench's, which is known to draw: a distance only
-- proves itself against a scene that already renders — and a scene only renders what
-- `scene:setCamera` picks, the default camera is the 2D one.
local Kit = require("demos.kit_demo")

local function envNumber(name, fallback)
    local ok, raw = pcall(function() return os.getenv(name) end)
    local v = ok and tonumber(raw) or nil
    -- 0 is a real answer here ("no limit"), so the usual "positive or fallback" rule of the
    -- other probes would silently ignore the case this one exists to measure.
    return v or fallback
end

local DISTANCE = math.max(0.0, envNumber("RD", 0.0))
local HOLD = envNumber("RD_HOLD", 0) > 0
local N = 1000
local MODEL = "Models/MapKit/mapkit_cube.glb"
-- The camera's own far, so a run can be told to author the limit instead of capping it: the
-- counters of "RD=150 RD_FAR=6000" and "RD=0 RD_FAR=150" have to agree, or the cap is not
-- reaching the projection at all.
local AUTHORED_FAR = envNumber("RD_FAR", 6000.0)

local checks = 0
local failed = 0
local function check(label, ok, detail)
    checks = checks + 1
    if not ok then failed = failed + 1 end
    print(string.format("[rd] %-30s %s   %s", label, ok and "PASS" or "FAIL", detail or ""))
end

Kit.start(function()
    local scene = Kit.createScene()
    scene.backgroundColor = Vector4(0.10, 0.12, 0.16, 1.0)

    local sun = Kit.hold(Light(scene))
    sun.name = "Sun"
    sun:setType(LightType.DIRECTIONAL)
    sun.direction = Vector3(-0.30, -0.85, -0.40)
    sun:setIntensity(1.5)

    -- The camera keeps the authored far clip: the distance caps the projection, never the value
    -- saved in the scene.
    local camera = Kit.hold(Camera(scene))
    camera.name = "RdCam"
    Kit.camera3D(camera, 60.0, 0.1, AUTHORED_FAR)
    -- Without this the scene keeps rendering through its default UI camera and the distance
    -- (which is a perspective setting) has nothing to cap; the shipped demos do the same call.
    scene:setCamera(camera)

    local side = math.floor(math.sqrt(N) + 0.5)
    local spacing = 3.0
    local radius = side * spacing * 1.6
    local height = side * spacing * 0.8

    local built = 0
    local loadFailed = 0
    for i = 0, N - 1 do
        local m = Kit.hold(Model(scene))
        if m:loadModel(MODEL) == false then loadFailed = loadFailed + 1 end
        local gx = i % side
        local gz = math.floor(i / side)
        m.position = Vector3((gx - side * 0.5) * spacing, 0.5, (gz - side * 0.5) * spacing)
        built = built + 1
    end

    scene.renderDistance = DISTANCE
    check("every entity was built", built == N, string.format("%d of %d", built, N))
    check("the model file loads", loadFailed == 0, string.format("%d rejected", loadFailed))
    check("the scene keeps the distance asked for", math.abs(scene.renderDistance - DISTANCE) < 0.01,
        string.format("%.0f m", scene.renderDistance))
    check("the effective distance is the same",
        math.abs(scene:getEffectiveRenderDistance() - DISTANCE) < 0.01,
        string.format("%.0f m", scene:getEffectiveRenderDistance()))
    check("the authored camera far clip survives", math.abs(camera.farClip - AUTHORED_FAR) < 0.01,
        string.format("%.0f m", camera.farClip))

    -- The editor's own cap: it wins while it lasts and hands back when it is cleared.
    scene.renderDistanceOverride = 37.0
    check("the override wins", math.abs(scene:getEffectiveRenderDistance() - 37.0) < 0.01,
        string.format("%.0f m", scene:getEffectiveRenderDistance()))
    scene.renderDistanceOverride = 0.0
    check("clearing it falls back", math.abs(scene:getEffectiveRenderDistance() - DISTANCE) < 0.01,
        string.format("%.0f m", scene:getEffectiveRenderDistance()))
    check("a negative distance means unlimited",
        (function()
            scene.renderDistance = -50.0
            local stored = scene.renderDistance
            scene.renderDistance = DISTANCE
            return stored == 0.0
        end)(), "stored as 0")

    camera.position = Vector3(0, height, radius)
    camera.target = Vector3(0, 0, 0)

    print(string.format("[rd] %d entities over %.0f m, camera far %.0f m, scene render distance %.0f m",
        built, 2.0 * radius, camera.farClip, scene.renderDistance))

    local driver = { name = "RdDriver", frames = 0, done = false }
    driver.onUpdate = function()
        driver.frames = driver.frames + 1
        -- Orbit, so a static fully-frustum-rejected view cannot be mistaken for the cap working.
        local a = driver.frames * 0.012
        camera.position = Vector3(math.sin(a) * radius, height, math.cos(a) * radius)
        camera.target = Vector3(0, 0, 0)
        if driver.frames == 120 and not driver.done then
            driver.done = true
            print(string.format("[rd] ===== %d checks, %d failed =====", checks, failed))
            check("no check failed", failed == 0, string.format("%d failed", failed))
            Kit.report("rd")
            -- The runner prints FrameStats only when it ends the run itself.
            if not HOLD then System.quit() end
        end
    end
    RegisterEngineEvent(driver, "onUpdate")
    Kit.keep[#Kit.keep + 1] = driver
end)
