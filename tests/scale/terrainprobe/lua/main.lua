-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Foliage probe: what a big scattered field costs once it is on screen, and what one density
-- repaint costs. The two numbers answer the two complaints:
--   steady  the fps a level with this much grass holds,
--   repaint the hitch felt on each brush stroke that touches the density map.
-- A repaint that names a small rect has to be cheaper than one that covers the map, because only
-- the chunks under the rect re-derive their instances; that is the whole point of the rect, so the
-- probe measures both and compares.
--   FOLIAGE_LAYERS=2 quantum-engine.exe --project tests/scale/terrainprobe --window 1280x720 \
--       --frames 400 --warmup 20 --screenshot /tmp/foliage.png --capture-frame 380
-- Unbuffered, so the last frame printed before a crash is the frame that was actually reached.
pcall(function() io.stdout:setvbuf("no") end)

local function envNumber(name, fallback)
    local ok, raw = pcall(function() return os.getenv(name) end)
    local v = ok and tonumber(raw) or nil
    return (v and v > 0) and v or fallback
end

local SIZE = envNumber("FOLIAGE_SIZE", 180)
local SKIP_FOLIAGE = envNumber("FOLIAGE_SKIP", 0) > 0
local LAYERS = envNumber("FOLIAGE_LAYERS", 2)
local DENSITY = envNumber("FOLIAGE_DENSITY", 2)
local DRAW_DISTANCE = envNumber("FOLIAGE_DRAW_DISTANCE", 60)
local MAX_HEIGHT = envNumber("FOLIAGE_MAX_HEIGHT", 8)
local SETTLE = envNumber("FOLIAGE_SETTLE", 120)
local SAMPLES = envNumber("FOLIAGE_SAMPLES", 60)
local PAINT_FRAMES = envNumber("FOLIAGE_PAINT_FRAMES", 8)

local keep = {}
local function hold(object)
    keep[#keep + 1] = object
    return object
end

local checks = 0
local failed = 0
local function check(label, ok, detail)
    checks = checks + 1
    if not ok then failed = failed + 1 end
    print(string.format("[foliage] %-34s %s   %s", label, ok and "PASS" or "FAIL", detail or ""))
end

-- `scene` has to be held like everything else: nothing in the driver closure refers to it after
-- setup, so Lua is free to collect the wrapper - and every wrapper that still points at the C++
-- scene it owned then reads freed memory, which surfaces as "Component not registered before use"
-- a few dozen frames later.
local scene = hold(Scene())
Engine.scene = scene
scene.backgroundColor = Vector4(0.42, 0.58, 0.76, 1.0)

local sun = hold(Light(scene))
sun.name = "Sun"
sun:setType(LightType.DIRECTIONAL)
sun.direction = Vector3(-0.30, -0.85, -0.40)
sun:setIntensity(1.5)

local camera = hold(Camera(scene))
camera.name = "FoliageCam"
local width, height = 0, 0
pcall(function()
    width = Engine.getScreenWidth()
    height = Engine.getScreenHeight()
end)
local aspect = (width > 0 and height > 0) and (width / height) or 1.7778
camera:setPerspective(60.0, aspect, 0.1, 2000.0)
camera.autoResize = true
scene:setCamera(camera)

local terrain = hold(Terrain(scene))
terrain.name = "Field"
terrain.position = Vector3(0, 0, 0)
terrain.size = SIZE
terrain.maxHeight = MAX_HEIGHT
terrain.resolution = 32
terrain.rootGridSize = 2
terrain.levels = 6
terrain:setHeightMap("height.png")

-- One plain colour layer: a terrain whose layer list is empty has nothing to splat.
local grass = TerrainSurfaceLayer()
grass.pbr = false
grass.colorFactor = Vector4(0.34, 0.46, 0.24, 1.0)
terrain:setSurfaceLayer(0, grass)

for i = 1, SKIP_FOLIAGE and 0 or LAYERS do
    local layer = terrain:addFoliageLayer("cube.obj", "density.png")
    terrain:setFoliageDensity(layer, DENSITY)
    terrain:setFoliageDrawDistance(layer, DRAW_DISTANCE)
    -- Grass-scale scatter is the shadow cascade's worst customer; the probe keeps it on so the
    -- measured cost is the one the editor pays.
    terrain:setFoliageCastsShadows(layer, true)
end
check("every foliage layer was added", terrain:getNumFoliageLayers() == LAYERS,
    string.format("%d of %d", terrain:getNumFoliageLayers(), LAYERS))

camera.position = Vector3(0, MAX_HEIGHT * 1.4, SIZE * 0.30)
camera.target = Vector3(0, 0, 0)

local MAP_TEXELS = 256
local SMALL_RECT = 20

local driver = {
    name = "FoliageDriver", frames = 0, phase = "settle",
    samples = {}, steady = nil, small = nil, full = nil, peak = 0, window = 0,
}

local function best(dt)
    if driver.phase == "steady" then
        driver.samples[#driver.samples + 1] = dt
        if #driver.samples >= SAMPLES then
            local sorted, sum, max = {}, 0.0, 0.0
            for i = 1, #driver.samples do
                sorted[i] = driver.samples[i]
                sum = sum + driver.samples[i]
                if driver.samples[i] > max then max = driver.samples[i] end
            end
            table.sort(sorted)
            local avg = sum / #sorted
            driver.steady = { avg = avg * 1000.0, p95 = sorted[math.max(1, math.floor(#sorted * 0.95))] * 1000.0, max = max * 1000.0 }
            print(string.format("[foliage] steady layers=%d density=%g dd=%g samples=%d avg=%.2f ms p95=%.2f max=%.2f (%.1f fps)",
                LAYERS, DENSITY, DRAW_DISTANCE, #sorted, driver.steady.avg, driver.steady.p95, driver.steady.max,
                1000.0 / math.max(driver.steady.avg, 1e-6)))
            check("a grass field holds 30 fps", driver.steady.avg <= 33.4, string.format("avg %.2f ms", driver.steady.avg))
            driver.phase = "gap"
            driver.window = PAINT_FRAMES
        end
    elseif driver.phase == "small" or driver.phase == "full" then
        if dt > driver.peak then driver.peak = dt end
        driver.window = driver.window - 1
        if driver.window <= 0 then
            local ms = driver.peak * 1000.0
            if driver.phase == "small" then
                driver.small = ms
                print(string.format("[foliage] repaint a %dx%d texel rect: %.2f ms (%.2f ms over the steady %.2f)",
                    SMALL_RECT, SMALL_RECT, ms, ms - (driver.steady and driver.steady.avg or 0),
                    driver.steady and driver.steady.avg or 0))
                driver.phase = "gap"
                driver.window = PAINT_FRAMES
            else
                driver.full = ms
                print(string.format("[foliage] repaint the whole %d texel map: %.2f ms (%.2f ms over the steady %.2f)",
                    MAP_TEXELS, ms, ms - (driver.steady and driver.steady.avg or 0),
                    driver.steady and driver.steady.avg or 0))
            end
            -- Either way the window is spent: without this the phase stays "full" and every later
            -- frame re-reports the same peak instead of ending the run.
            if driver.phase == "full" and driver.small and driver.full then
                driver.phase = "gap"
                driver.window = PAINT_FRAMES
            end
        end
    end
end

driver.onUpdate = function()
    driver.frames = driver.frames + 1
    if driver.frames <= 5 or driver.frames % 25 == 0 then
        print(string.format("[foliage] frame %d phase=%s", driver.frames, driver.phase))
    end
    local dt = tonumber(Engine.deltatime) or 0.0
    local a = driver.frames * 0.006
    camera.position = Vector3(math.sin(a) * SIZE * 0.30, MAX_HEIGHT * 1.4, math.cos(a) * SIZE * 0.30)
    camera.target = Vector3(0, 0, 0)

    if driver.phase == "settle" then
        if driver.frames >= SETTLE then
            driver.phase = "steady"
        end
    elseif driver.phase == "gap" then
        driver.window = driver.window - 1
        if driver.window <= 0 then
            if driver.small == nil then
                driver.phase = "small"
                driver.peak = 0
                driver.window = PAINT_FRAMES
                terrain:markFoliageDensityDirty(0, 40, 40, 40 + SMALL_RECT, 40 + SMALL_RECT)
            elseif driver.full == nil then
                driver.phase = "full"
                driver.peak = 0
                driver.window = PAINT_FRAMES
                terrain:markFoliageDensityDirty(0, 0, 0, MAP_TEXELS - 1, MAP_TEXELS - 1)
            else
                driver.phase = "done"
                if driver.steady and driver.small and driver.full then
                    check("a small repaint beats a whole map", driver.small < driver.full * 0.85,
                        string.format("%.2f ms vs %.2f ms (%.2fx cheaper)", driver.small, driver.full,
                        driver.full / math.max(driver.small, 1e-6)))
                    check("a repaint is not a freeze", driver.full <= 1000.0,
                        string.format("worst repaint %.2f ms", driver.full))
                end
                print(string.format("[foliage] ===== %d checks, %d failed =====", checks, failed))
            end
        end
    end

    best(dt)
end
-- The event registry roots the driver table, so hanging the keep table off it keeps every
-- wrapper - including the scene - alive for as long as the driver runs.
driver.keep = keep
RegisterEngineEvent(driver, "onUpdate")
hold(driver)
