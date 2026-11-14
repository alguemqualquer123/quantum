-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- A million plants on a generated island, measured instead of asserted.
--
-- Run from the repository root:
--   build/Debug/quantum-editor-cmd.exe terrain generate --out tests/wind/forest/assets/textures/heightmap_forest.png --preset island --size 512 --seed 7313
--   python tests/wind/forest/scripts/make_density_map.py
--   build/Debug/quantum-engine.exe --project tests/wind/forest --window 1280x720 --frames 300 --warmup 120 \
--       --stats-out /tmp/forest.json --screenshot /tmp/forest.png --capture-frame 280
--
-- The last command's JSON is the evidence this file exists for: avgInstances is the plant count the
-- renderer actually submitted and avgDrawCalls is what it cost. A million plants are not a million
-- entities - MeshSystem::createFoliageEntity builds one Transform+Mesh+Model+InstancedMesh entity per
-- 8 m placement cell, so the whole field is a few hundred entities and the plants are instance data.
--
-- The heightmap is generated before this project ever runs, and the density map is painted from
-- those exact bytes: the scatter has to respect the same terrain the renderer samples, or grass grows
-- in the sea. See WIND_SYSTEM_PLAN.md phase 5 for the wind that is supposed to move all of this.

local hold = {}
local function keep(value)
    table.insert(hold, value)
    return value
end

local SEED = 7313
local TERRAIN_SIZE = 900.0
local MAX_HEIGHT = 48.0
local PLANT_TARGET = 1000000
local DRAW_DISTANCE = 128.0
-- both of these come from the bytes the generator and the painter wrote for this seed, and the
-- scatter has to be tuned against them or the count is a guess: the map is 59% bare ground and the
-- island's centre texel sits 26.68 m up, which is where the camera goes
local MEAN_COVER = 0.183
local GROUND_HEIGHT = 26.68

-- foliageCellAttempts is ceil(density * 8 * 8) per cell and a texel accepts an attempt when its map
-- value beats the roll, so the field holds about (pi * D^2) * density * meanCover plants - the same
-- law read backwards, with the bare ground paid for.
local function densityFor(target, distance, cover)
    return target / (math.pi * distance * distance * cover)
end

local scene
local report = { frames = 0 }

local builder = {}
builder.name = "WindForestStress"

function builder.onViewLoaded()
    scene = keep(Scene())
    Engine.scene = scene
    scene.backgroundColor = Vector4(0.46, 0.62, 0.78, 1.0)
    -- the water mirrors the sky through this setting and its 0.1 default turns a lake to ink
    scene.globalIlluminationIntensity = 0.55

    scene.windEnabled = true
    scene.windSpeed = 12.0
    scene.windDirection = 35.0
    scene.windGustiness = 0.5
    scene.windChange = 0.4

    local sun = keep(Light(scene))
    sun.name = "Sun"
    sun:setType(LightType.DIRECTIONAL)
    sun.direction = Vector3(-0.35, -0.85, 0.39)
    sun:setIntensity(1.35)

    local sky = keep(SkyBox(scene))
    sky.name = "Sky"
    sky.color = Vector4(0.42, 0.60, 0.85, 1.0)

    local camera = keep(Camera(scene))
    camera.name = "Camera"
    -- low, over the meadow: the whole draw-distance ring should fill the frame
    camera.position = Vector3(0.0, GROUND_HEIGHT + 2.5, 0.0)
    camera.target = Vector3(0.0, GROUND_HEIGHT + 0.5, -60.0)
    scene:setCamera(camera)
    -- a script-built camera is a UI camera with a 10-unit far clip
    camera:setPerspective(60.0, 1280.0 / 720.0, 0.3, 400.0)
    camera.autoResize = true

    local terrain = keep(Terrain(scene))
    terrain.name = "Island"
    terrain.size = TERRAIN_SIZE
    terrain.maxHeight = MAX_HEIGHT
    terrain.resolution = 64
    terrain.levels = 8
    terrain.color = Vector4(0.34, 0.30, 0.24, 1.0)
    terrain:setHeightMap("textures/heightmap_forest.png")

    local density = densityFor(PLANT_TARGET, DRAW_DISTANCE, MEAN_COVER)
    local grass = terrain:addFoliageLayer("TerrainTeste/Foliages/Separate/BuffaloGrass.glb",
        "textures/foliage_density.png")
    terrain:setFoliageDensity(grass, density)
    terrain:setFoliageDrawDistance(grass, DRAW_DISTANCE)

    local sea = keep(Water(scene))
    sea.name = "Sea"
    sea.position = Vector3(0.0, 0.34 * MAX_HEIGHT, 0.0)
    sea.size = Vector2(TERRAIN_SIZE, TERRAIN_SIZE)
    sea.subdivisions = 96
    sea.shallowColor = Vector3(0.25, 0.50, 0.55)
    sea.deepColor = Vector3(0.02, 0.10, 0.18)
    sea.depthFade = 3.0
    sea.shoreFoam = 0.05

    print("[forest] plants targeted: " .. string.format("%.0f", PLANT_TARGET))
    print("[forest] draw distance: " .. string.format("%.1f", DRAW_DISTANCE) .. " m")
    print("[forest] density: " .. string.format("%.2f", density) .. " per square metre")
    print("[forest] attempts per cell: " .. string.format("%.0f", math.min(8192, math.ceil(density * 64.0))))
    print("[forest] cells in view: " .. string.format("%.0f", math.pi * DRAW_DISTANCE * DRAW_DISTANCE / 64.0))
    print("[forest] mean density-map cover: " .. string.format("%.3f", MEAN_COVER))
end

-- Engine.framerate is the only timing the script side can read; the instance and draw-call counts come
-- from --stats-out, so this prints what the frame rate did and the harness prints what it cost.
function builder.onUpdate(dt)
    report.frames = report.frames + 1
    if report.frames % 30 == 0 then
        print("[forest] frame " .. report.frames .. " fps " .. string.format("%.1f", Engine.framerate))
    end
    if report.frames == 200 then
        print("[forest] settle done - the scatter is stable, measure from here")
    end
end

RegisterEngineEvent(builder, "onViewLoaded")
RegisterEngineEvent(builder, "onUpdate")
