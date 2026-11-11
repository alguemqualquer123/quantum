-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- The user's own level, measured headless: the fps-fps terrain (179.5 m, 1024 heightmap, 4K surface
-- textures), its seven real grass/flower layers painted with its 512 density map, and a field of
-- prop entities on top. The three numbers are the three complaints:
--   spawnMs   what it costs to put N objects into the level,
--   steady    the fps a level this full holds while the camera walks through the grass,
--   repaint   the hitch behind one brush stroke on the density map.
-- Paths are relative to the project's assets directory, which junctions straight into
-- templates/fps-fps, so this measures the same bytes the editor loads.
--   FPS_PROPS=2000 quantum-engine.exe --project tests/scale/fpsprobe --window 1280x720 \
--       --frames 400 --warmup 20 --stats-out <abs>.json --screenshot <abs>.png --capture-frame 300
pcall(function() io.stdout:setvbuf("no") end)

local function envNumber(name, fallback)
    local ok, raw = pcall(function() return os.getenv(name) end)
    local v = ok and tonumber(raw) or nil
    return (v and v > 0) and v or fallback
end

-- The same, but zero is a real answer: "no foliage layers" and "no props" are both runs the cost
-- matrix needs, and the helper above would silently fall back to the default instead.
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

local TERRAIN_SIZE = envNumber("FPS_TERRAIN_SIZE", 179.5)
local MAX_HEIGHT = envNumber("FPS_MAX_HEIGHT", 20)
local LAYERS = envNumber0("FPS_FOLIAGE_LAYERS", 7)
-- The three knobs the cost matrix needs: shadow casters, the light's shadow pass, and the scene's
-- render distance. Each one moves a different line of the frame, so a run per setting says which
-- cost is worth attacking.
local CAST_SHADOWS = envNumber("FPS_CAST_SHADOWS", 1) > 0
local LIGHT_SHADOWS = envNumber("FPS_LIGHT_SHADOWS", 1) > 0
-- The engine's own index-simplification detail levels (MeshLodPool), off until a scene asks. A
-- grass tuft of 129 thousand triangles does not need that many at thirty metres, so this is the
-- knob that says whether the 144 million triangles per frame are a missing feature or a setting.
-- Unset keeps whatever the engine ships with, so a run measures the configuration a project gets.
-- An explicit branch, not `a and nil or b`: that idiom returns the *other* arm whenever the value
-- it should return is nil, which is exactly what a nil means here.
local MESH_LOD = nil
if envText("FPS_MESH_LOD", "") ~= "" then
    MESH_LOD = (tonumber(envText("FPS_MESH_LOD", "")) or 0) > 0
end
local RENDER_DISTANCE = envNumber("FPS_RENDER_DISTANCE", 0)
local DENSITY = envNumber("FPS_FOLIAGE_DENSITY", 1)
local DRAW_DISTANCE = envNumber("FPS_FOLIAGE_DRAW_DISTANCE", 40)
local PROPS = envNumber0("FPS_PROPS", 2000)
local PROP_MESH = envText("FPS_PROP_MESH", "cube.obj")
-- Every foliage layer draws this mesh instead of its own when it is set. The grass .glb files carry
-- thousands of triangles each, so a run with a 12-triangle cube at the same instance count says
-- whether the frame is bound by the GPU rasterising triangles or by the CPU walking the instances.
local FOLIAGE_MESH = envText("FPS_FOLIAGE_MESH", "")
local PROP_SPACING = envNumber("FPS_PROP_SPACING", 3.0)
local MAP_TEXELS = envNumber("FPS_MAP_TEXELS", 512)
local SMALL_RECT = envNumber("FPS_SMALL_RECT", 20)
local SETTLE = envNumber("FPS_SETTLE", 150)
local SAMPLES = envNumber("FPS_SAMPLES", 60)
local PAINT_FRAMES = envNumber("FPS_PAINT_FRAMES", 12)
-- A still camera separates the ring-slide churn (chunks entering/leaving the draw-distance window
-- every frame) from the cost of drawing a field that is not moving.
local CAMERA_STATIC = envNumber("FPS_CAMERA_STATIC", 0) > 0

local HEIGHT_MAP = envText("FPS_HEIGHT_MAP", "terrain_maps/terrain_edit_4233178914_1003_height_2.png")
local BLEND_MAP = "terrain_maps/terrain_edit_4233178914_1003_blend_3.png"
-- The level's own density map is almost empty outside the painted patches, which measures the
-- engine but not the scatter. density_all.png is white everywhere, so the stress run places as many
-- instances as the cells allow; FPS_DENSITY_MAP points back at the painted one when the level's own
-- numbers are wanted.
local DENSITY_MAP = envText("FPS_DENSITY_MAP", "density_all.png")
local ROCKY_DIFF = "TerrainTeste/rocky_terrain_02_4k.blend/textures/rocky_terrain_02_diff_4k.jpg"
local GRASS_DIR = "TerrainTeste/Foliages/Separate/"
local FOLIAGE_MESHES = {
    "BuffaloGrass.glb",
    "BlanketPhlox.glb",
    "BuffaloGrass_Sprigs.glb",
    "Kinnikinnick.glb",
    "OnionSprigs.glb",
    "WheatGrass_Sprigs.glb",
    "WheatGrass.glb",
}

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
    print(string.format("[fps] %-34s %s   %s", label, ok and "PASS" or "FAIL", detail or ""))
end

local scene = hold(Scene())
Engine.scene = scene
scene.backgroundColor = Vector4(0.42, 0.58, 0.76, 1.0)

local sun = hold(Light(scene))
sun.name = "Sun"
sun:setType(LightType.DIRECTIONAL)
sun.direction = Vector3(-0.30, -0.85, -0.40)
sun:setIntensity(1.5)
if not LIGHT_SHADOWS then sun.shadows = false end
scene.renderDistance = RENDER_DISTANCE
if MESH_LOD ~= nil then scene.meshLodEnabled = MESH_LOD end

local camera = hold(Camera(scene))
camera.name = "FpsCam"
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
terrain.size = TERRAIN_SIZE
terrain.maxHeight = MAX_HEIGHT
terrain.resolution = 32
terrain.rootGridSize = 2
terrain.levels = 6
terrain:setHeightMap(HEIGHT_MAP)
terrain:setBlendMap(BLEND_MAP)

-- The level's two splat layers, both on the 4K photograph the scene uses: this is what makes the
-- texture upload cost part of the measurement instead of a separate mystery.
for i = 1, 2 do
    local layer = TerrainSurfaceLayer()
    layer.pbr = true
    local tex = hold(Texture())
    tex:setPath(ROCKY_DIFF)
    tex:load(ROCKY_DIFF)
    layer.colorTexture = tex
    layer.uvScale = Vector2(24, 24)
    terrain:setSurfaceLayer(i - 1, layer)
end

for i = 1, LAYERS do
    local mesh = FOLIAGE_MESH
    if mesh == "" then mesh = GRASS_DIR .. FOLIAGE_MESHES[i] end
    local index = terrain:addFoliageLayer(mesh, DENSITY_MAP)
    terrain:setFoliageDensity(index, DENSITY)
    terrain:setFoliageDrawDistance(index, DRAW_DISTANCE)
    terrain:setFoliageCastsShadows(index, CAST_SHADOWS)
end
check("every foliage layer was added", terrain:getNumFoliageLayers() == LAYERS,
    string.format("%d of %d", terrain:getNumFoliageLayers(), LAYERS))

local side = math.floor(math.sqrt(PROPS) + 0.5)
if side < 1 then side = 1 end
local props = {}
local spawned = 0
local loadFailed = 0
local spawnMs = 0.0

local t0 = os.clock()
for i = 0, PROPS - 1 do
    local model = hold(Model(scene))
    if model:loadModel(PROP_MESH) == false then loadFailed = loadFailed + 1 end
    local gx = i % side
    local gz = math.floor(i / side)
    model.position = Vector3((gx - side * 0.5) * PROP_SPACING, 1.0, (gz - side * 0.5) * PROP_SPACING)
    props[#props + 1] = model
    spawned = spawned + 1
end
spawnMs = (os.clock() - t0) * 1000.0

-- Above the relief and looking into it: a fixed low height buries the camera in the hills of a
-- real heightmap, and the frame then measures nothing.
local CAM_HEIGHT = envNumber("FPS_CAMERA_HEIGHT", MAX_HEIGHT * 1.2 + 8)
local CAM_RADIUS = envNumber("FPS_CAMERA_RADIUS", TERRAIN_SIZE * 0.42)
local orbitRadius = CAM_RADIUS
camera.position = Vector3(0, CAM_HEIGHT, orbitRadius)
camera.target = Vector3(0, MAX_HEIGHT * 0.4, 0)

print(string.format("[fps] meshLodEnabled=%s (%s)", tostring(scene.meshLodEnabled),
    MESH_LOD == nil and "engine default" or "FPS_MESH_LOD"))
print(string.format("[fps] props=%d in %.0f ms (%.4f ms/prop) rejected_loads=%d",
    spawned, spawnMs, spawnMs / math.max(spawned, 1), loadFailed))
check("every prop was built", spawned == PROPS, string.format("%d of %d", spawned, PROPS))
check("the prop mesh loads", loadFailed == 0, string.format("%d rejected", loadFailed))

local driver = {
    name = "FpsDriver", frames = 0, phase = "settle",
    samples = {}, steady = nil, small = nil, full = nil, peak = 0, window = 0, keep = keep,
}

local function finishRepaint(ms, label)
    if driver.phase == "small" then
        driver.small = ms
        print(string.format("[fps] repaint a %dx%d texel rect: %.2f ms", SMALL_RECT, SMALL_RECT, ms))
    else
        driver.full = ms
        print(string.format("[fps] repaint the whole %d texel map: %.2f ms", MAP_TEXELS, ms))
    end
end

local function collect(dt)
    driver.samples[#driver.samples + 1] = dt
    if #driver.samples < SAMPLES then return end
    local sorted, sum, max = {}, 0.0, 0.0
    for i = 1, #driver.samples do
        sorted[i] = driver.samples[i]
        sum = sum + driver.samples[i]
        if driver.samples[i] > max then max = driver.samples[i] end
    end
    table.sort(sorted)
    local avg = sum / #sorted
    driver.steady = { avg = avg * 1000.0, p95 = sorted[math.max(1, math.floor(#sorted * 0.95))] * 1000.0, max = max * 1000.0 }
    print(string.format("[fps] steady props=%d layers=%d dd=%g samples=%d avg=%.2f ms p95=%.2f max=%.2f (%.1f fps)",
        spawned, LAYERS, DRAW_DISTANCE, #sorted, driver.steady.avg, driver.steady.p95, driver.steady.max,
        1000.0 / math.max(driver.steady.avg, 1e-6)))
    check("a full level holds 30 fps", driver.steady.avg <= 33.4, string.format("avg %.2f ms", driver.steady.avg))
    check("no frame is a hitch", driver.steady.max <= 100.0, string.format("worst %.2f ms", driver.steady.max))
    check("putting the props in is not a hitch", spawnMs <= 1000.0, string.format("%.0f ms", spawnMs))
    driver.phase = "gap"
    driver.window = PAINT_FRAMES
end

driver.onUpdate = function()
    driver.frames = driver.frames + 1
    if driver.frames <= 5 or driver.frames % 25 == 0 then
        print(string.format("[fps] frame %d phase=%s entities=%d", driver.frames, driver.phase,
            #scene:getEntityList()))
    end
    local dt = tonumber(Engine.deltatime) or 0.0
    if not CAMERA_STATIC then
        local a = driver.frames * 0.006
        camera.position = Vector3(math.sin(a) * orbitRadius, CAM_HEIGHT, math.cos(a) * orbitRadius)
        camera.target = Vector3(0, MAX_HEIGHT * 0.4, 0)
    end

    if driver.phase == "settle" then
        if driver.frames >= SETTLE then driver.phase = "steady" end
    elseif driver.phase == "steady" then
        collect(dt)
    elseif driver.phase == "gap" then
        driver.window = driver.window - 1
        if driver.window <= 0 then
            if driver.small == nil then
                driver.phase = "small"
                driver.peak = 0
                driver.window = PAINT_FRAMES
                terrain:markFoliageDensityDirty(0, 100, 100, 100 + SMALL_RECT, 100 + SMALL_RECT)
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
                print(string.format("[fps] ===== %d checks, %d failed =====", checks, failed))
            end
        end
    elseif driver.phase == "small" or driver.phase == "full" then
        if dt > driver.peak then driver.peak = dt end
        driver.window = driver.window - 1
        if driver.window <= 0 then
            finishRepaint(driver.peak * 1000.0)
            driver.phase = "gap"
            driver.window = PAINT_FRAMES
        end
    end
end
RegisterEngineEvent(driver, "onUpdate")
hold(driver)
