-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- What a scene-wide shader-variant reload costs at scale. Flipping a setting that is baked into
-- every mesh's variant (SSAO, depth pre-pass, IBL, the first 2D shadow light, the first
-- reflection probe) flags every MeshComponent in the scene, so the next frame destroys and
-- rebuilds every one of them. This measures that frame with N props and nothing else in view.
--   RELOAD_PROPS=10000 quantum-engine.exe --project tests/scale/fpsprobe --scene reloadcost \
--       --window 1280x720 --frames 400 --warmup 300 --stats-out <abs>.json
pcall(function() io.stdout:setvbuf("no") end)

local function envNumber0(name, fallback)
    local ok, raw = pcall(function() return os.getenv(name) end)
    local v = ok and tonumber(raw) or nil
    return v or fallback
end

local function envText(name, fallback)
    local ok, raw = pcall(function() return os.getenv(name) end)
    if ok and type(raw) == "string" and raw ~= "" then return raw end
    return fallback
end

local PROPS = envNumber0("RELOAD_PROPS", 10000)
local PROP_MESH = envText("RELOAD_PROP_MESH", "cube.obj")
local SPACING = envNumber0("RELOAD_SPACING", 3.0)
local SETTLE = envNumber0("RELOAD_SETTLE", 240)
local WINDOW = envNumber0("RELOAD_WINDOW", 40)
-- Frames at the end of the window, measured after the sweep finished: their mean is the level the
-- frame sits at once the setting has taken effect, and everything above it is reload work.
local TAIL = envNumber0("RELOAD_TAIL", 40)
-- The setting flipped to force the reload. Each arm goes through Scene::set* -> needReloadMeshes.
local TRIGGER = envText("RELOAD_TRIGGER", "ssao")

local keep = {}
local function hold(value)
    keep[#keep + 1] = value
    return value
end

local scene = hold(Scene())
Engine.scene = scene
scene.backgroundColor = Vector4(0.42, 0.58, 0.76, 1.0)

local sun = hold(Light(scene))
sun.name = "Sun"
sun:setType(LightType.DIRECTIONAL)
sun.direction = Vector3(-0.30, -0.85, -0.40)
sun:setIntensity(1.5)

local camera = hold(Camera(scene))
camera.name = "ReloadCam"
local width, height = 0, 0
pcall(function()
    width = Engine.getScreenWidth()
    height = Engine.getScreenHeight()
end)
local aspect = (width > 0 and height > 0) and (width / height) or 1.7778
camera:setPerspective(60.0, aspect, 0.1, 4000.0)
scene:setCamera(camera)

local side = math.floor(math.sqrt(PROPS) + 0.5)
if side < 1 then side = 1 end
local t0 = os.clock()
for i = 0, PROPS - 1 do
    local model = hold(Model(scene))
    model:loadModel(PROP_MESH)
    local gx = i % side
    local gz = math.floor(i / side)
    model.position = Vector3((gx - side * 0.5) * SPACING, 1.0, (gz - side * 0.5) * SPACING)
end
local spawnMs = (os.clock() - t0) * 1000.0

local orbit = side * SPACING * 0.7 + 40.0
camera.position = Vector3(0, orbit * 0.55, orbit)
camera.target = Vector3(0, 1.0, 0)

local apply = function()
    if TRIGGER == "none" then
        return
    elseif TRIGGER == "ssao" then
        scene.ssaoEnabled = not scene.ssaoEnabled
    elseif TRIGGER == "prepass" then
        scene.depthPrepassEnabled = not scene.depthPrepassEnabled
    else
        scene.dofEnabled = not scene.dofEnabled
    end
end

local driver = { name = "ReloadDriver", frames = 0, phase = "settle", window = 0,
                 peak = 0.0, sum = 0.0, count = 0, before = 0.0, samples = {}, keep = keep }

local function topFive(samples)
    local sorted = {}
    for i = 1, #samples do sorted[i] = samples[i] end
    table.sort(sorted, function(a, b) return a > b end)
    local out = {}
    for i = 1, math.min(5, #sorted) do out[i] = string.format("%.1f", sorted[i] * 1000.0) end
    return table.concat(out, ", ")
end

driver.onUpdate = function()
    driver.frames = driver.frames + 1
    local dt = tonumber(Engine.deltatime) or 0.0

    if driver.phase == "settle" then
        driver.before = driver.before + dt
        if driver.frames >= SETTLE and driver.frames % 30 == 0 then
            driver.phase = "fire"
            driver.window = WINDOW
            apply()
            print(string.format("[reload] entities=%d props=%d spawn=%.0f ms trigger=%s steady-before=%.2f ms",
                #scene:getEntityList(), PROPS, spawnMs, TRIGGER,
                (driver.before / SETTLE) * 1000.0))
        end
    elseif driver.phase == "fire" then
        if dt > driver.peak then driver.peak = dt end
        driver.count = driver.count + 1
        driver.samples[driver.count] = dt
        driver.window = driver.window - 1
        if driver.window <= 0 then
            -- The tail of the window is the new steady state: the setting flipped here also adds a
            -- real per-frame cost (the SSAO pass itself), so only the frames above that level are
            -- reload work.
            local tailFrames = math.min(TAIL, driver.count)
            local headFrames = driver.count - tailFrames
            local tailSum, extra = 0.0, 0.0
            for i = 1, driver.count do
                if i > headFrames then
                    tailSum = tailSum + driver.samples[i]
                else
                    extra = extra + driver.samples[i]
                end
            end
            local tail = tailSum / math.max(tailFrames, 1)
            print(string.format("[reload] worst frames (ms): %s", topFive(driver.samples)))
            print(string.format("[reload] new steady tail: %.2f ms over %d frames", tail * 1000.0, tailFrames))
            print(string.format("[reload] reload paid %.0f ms above that level across %d frames (peak frame %.1f ms)",
                extra * 1000.0 - tail * 1000.0 * headFrames, headFrames, driver.peak * 1000.0))
            driver.phase = "done"
            System.quit()
        end
    end
end
RegisterEngineEvent(driver, "onUpdate")
hold(driver)
