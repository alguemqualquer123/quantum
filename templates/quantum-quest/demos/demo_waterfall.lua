-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Rain on a cliff: water lands on a sloped shelf, runs downhill through a channel to the lip,
-- falls ten metres into a plunge pool, and the pool drains out through a valley. Everything in it
-- is the shallow-field sim, so what you see is the same water the physics floats on.
--
-- Run it from a project that has the library installed:
--   quantum-engine.exe --project <project> --scene demos/demo_waterfall --window 1280x720
-- Keys: R rains harder, F rain off, U the fall runs harder, D less, C prints what the sim holds,
-- ESC quits.
local Kit = require("demos.kit_demo")

local function number(value, fallback)
    if type(value) == "number" and value == value then return value end
    return fallback
end

-- The relief is authored by scripts/gen_terrain_maps.py and both maps come out of the same height
-- function, so these constants are the ones that script printed - not numbers chosen to look nice
-- next to the water.
local TERRAIN_SIZE = 90.0
local MAX_HEIGHT = 16.0
local LIP = Vector3(0, 10.3, -10.2)      -- where the channel meets the cliff
-- Where the sheet reaches the pool. The lake level is not a free number: it is the divide along
-- the outlet corridor (2.62 m, printed by scripts/gen_terrain_maps.py), and at 4 m/s of crest
-- speed the drop has travelled 4.5 m south by the time it has fallen to that plane.
local IMPACT = Vector3(0, 2.7, -5.1)
-- The fall's drain disc is centred on the lip, so a sample there is inside the sink and reads
-- dry no matter how much water is arriving. The number that means something is the stream a few
-- metres upstream of the weir.
local APPROACH = Vector3(0, 10.3, -12.4)

-- One run captures one frame, so a headless pass that wants to see the whole scene asks for the
-- angles one at a time instead of editing this file between runs. WATERFALL_SHOT is the name.
-- Every position is inside the 90 m field and above the water it is looking at: the pool stands
-- ~2.4 m over its own bed, so a camera at 4 m is under the surface, not beside it.
local function getenv(name)
    local out = nil
    pcall(function() out = os.getenv(name) end)
    return out
end
-- Every position is inside the 90 m field and above the water it is looking at: the pool stands
-- ~2.4 m over its own bed, so a camera at 4 m is under the surface, not beside it.
local SHOTS = {
    wide = { position = Vector3(34, 14, 26), target = Vector3(0, 6.0, -3) },
    pool = { position = Vector3(16, 6.5, 9), target = Vector3(0, 3.0, -4) },
    -- the column arcs 4.5 m out over its 7.6 m of fall, so the profile view has to stand east of
    -- the foot of the cliff, not south of it. Every camera is above the 2.6 m lake.
    fall = { position = Vector3(13, 7.0, 1), target = Vector3(0, 6.0, -6.5) },
    lip = { position = Vector3(-12, 13.5, -3), target = Vector3(0, 10.0, -10) },
    top = { position = Vector3(4, 20, -30), target = Vector3(0, 11.0, -13) },
}
local function shot()
    return SHOTS[getenv("WATERFALL_SHOT")] or SHOTS.wide
end

Kit.start(function()
    local scene = Kit.createScene()
    scene.backgroundColor = Vector4(0.30, 0.35, 0.42, 1.0)

    local sun = Kit.hold(Light(scene))
    sun.name = "Sun"
    sun:setType(LightType.DIRECTIONAL)
    sun.direction = Vector3(-0.35, -0.55, -0.75)
    sun:setIntensity(0.62)   -- an overcast sky, so the cliff is lit from above the falls

    -- The sky is what the eye adapter measures, so a default-white sky is a scene that exposes
    -- for the sky and renders the cliff in shadow. demo_ocean picks its blue the same way.
    local sky = Kit.hold(SkyBox(scene))
    sky.name = "Sky"
    sky.color = Vector4(0.42, 0.60, 0.85, 1.0)

    -- The lake mirrors the sky through envColor = skyColour x globalIlluminationIntensity
    -- (RenderSystem.cpp:7835), and that setting's default is 0.1 - a tenth of the sky is dark
    -- enough to read as ink, which is what the wide shot kept showing under a bright blue dome.
    -- The same knob dims the meshes' ambient, so it is raised to the point where the water
    -- reflects the sky without blowing out the olive cliff.
    scene.globalIlluminationIntensity = 0.55

    local fog = Kit.hold(Fog(scene))
    fog.name = "Fog"
    fog.color = Vector3(0.55, 0.63, 0.74)
    -- The whole cliff is 90 m across and the camera sits ~40 m from it, so the calm range has to
    -- clear the scene: WeatherSystem lerps from whatever the Fog entity holds toward its storm
    -- range, and a fog that starts at 45 m eats the frame before the weather even touches it.
    fog:setLinearStartEnd(180, 900)

    -- The camera before the terrain: a Lua-built terrain with no scene camera yet goes through
    -- MeshSystem::resetTerrainToUnbuilt, which clears needUpdateTerrain for good.
    local camera = Kit.hold(Camera(scene))
    camera.name = "Camera"
    local framing = shot()
    camera.position = framing.position
    camera.target = framing.target
    scene:setCamera(camera)
    Kit.camera3D(camera, 55, 0.1, 900)

    local terrain = Kit.hold(Terrain(scene))
    terrain.name = "Terrain"
    terrain.size = TERRAIN_SIZE
    terrain.maxHeight = MAX_HEIGHT
    terrain.resolution = 128
    terrain.levels = 6
    terrain:setHeightMap("terrain_maps/heightmap_cliff.png")
    terrain:setBlendMap("terrain_maps/blendmap_cliff.png")
    -- The blend map's three channels are these three layers, in order: rock on the slopes, grass
    -- on the shelf, mud wherever the water can reach.
    terrain:setTextureDetailRed("textures/ground/ground-mossy-rock-color.jpg")
    terrain:setTextureDetailGreen("textures/ground/ground-grass-color.jpg")
    terrain:setTextureDetailBlue("textures/ground/ground-mud-color.jpg")
    terrain.textureBaseTiles = 14
    terrain.textureDetailTiles = 90
    -- Whatever weight the three blend layers leave unclaimed is painted with the terrain's own
    -- colour, and a Mesh defaults to white: every flat patch of the cliff came out bleached.
    terrain.color = Vector4(0.36, 0.34, 0.31, 1.0)

    -- The shelf. Its own plane sits just under the channel bed at the lip, so every cell on the
    -- slope has a positive floor and the one at the border a negative one: that gradient is the
    -- whole reason the water moves. The border is a drain, which is the lip.
    --
    -- The field has to reach past z=-10.2 or the water leaves the grid before it reaches the
    -- cliff edge and the probe reads an empty cell where the stream should be. It stops just
    -- past it: a field that runs down the cliff face draws a sheet of water over dry rock.
    local river = Kit.hold(Water(scene))
    river.name = "River"
    river.position = Vector3(0, 10.5, -23.75)
    river.size = Vector2(32, 28.5)
    river.subdivisions = 128
    river.simulate = true
    river.simResolution = 128
    river.simDepth = 0.35
    -- 0.7 of the bed drag the pipe model now applies leaves a stream about 1 m/s deep enough to
    -- read; at the old frictionless setting the same rain flushed the shelf dry in seconds.
    river.simFlow = 0.7
    river.simRain = 0.010
    river.simDryDepth = 0.004
    river.simEdgeDrain = true
    river.simRipples = true
    river.simRippleStrength = 0.16
    -- Authored in sRGB: Water::setShallowColor/setDeepColor run Color::sRGBToLinear on the way
    -- in, so the numbers that looked like a pale stream in the editor's own defaults read as
    -- near-black ink when reused as if they were linear. depthFade is the depth where the deep
    -- colour takes over, and a 0.8 m stream has to fade inside its own depth or every pixel of
    -- it is the deep colour.
    river.shallowColor = Vector3(0.38, 0.60, 0.56)
    river.deepColor = Vector3(0.06, 0.24, 0.28)
    river.depthFade = 0.7
    river.shoreFoam = 0.02
    river:load()

    -- The bowl. Its plane is the sill height of the valley, so the lake can only rise to the
    -- lowest point of the rim and then leaves through the trench - a pool with an outlet rather
    -- than a bucket.
    local pool = Kit.hold(Water(scene))
    pool.name = "Pool"
    pool.position = Vector3(0, 1.66, 14.0)
    pool.size = Vector2(48, 44)
    pool.subdivisions = 128
    pool.simulate = true
    pool.simResolution = 192
    pool.simDepth = 0.8
    pool.simFlow = 0.6
    pool.simRain = 0.0
    pool.simDryDepth = 0.004
    pool.simEdgeDrain = true
    pool.simRipples = true
    pool.simRippleStrength = 0.2
    pool.shallowColor = Vector3(0.34, 0.62, 0.60)
    pool.deepColor = Vector3(0.05, 0.22, 0.31)
    -- Shore foam is driven by the distance along the view ray to whatever the depth buffer holds
    -- behind the surface, so a cliff wall rising out of the lake reads as "paper-thin water" for
    -- every pixel near it: at the 0.4 m default the contact line was a 40-pixel snow bank. The
    -- knob is what narrows that band down to an actual waterline froth.
    pool.depthFade = 1.6
    pool.shoreFoam = 0.07
    pool:load()

    local waves = Kit.attach(Kit.module("WaterWaves"), scene, pool.entity, {
        water = pool.entity, waveHeight = 0.09, waveLength = 5.5, waveSpeed = 0.7,
        waveSteepness = 0.35, crestFoam = 0.12, gustInterval = 7
    })
    Kit.attach(Kit.module("WaterWaves"), scene, river.entity, {
        -- crestFoam also scales the flow foam, and it is multiplied by three before it clamps:
        -- 0.45 puts every cell at full white regardless of how fast the water actually runs.
        water = river.entity, waveHeight = 0.04, waveLength = 2.6, waveSpeed = 1.5,
        waveSteepness = 0.25, crestFoam = 0.10, gustInterval = 4
    })

    -- The fall itself: the visible sheet plus the volume that keeps the pool from emptying.
    local falls = Kit.attach(Kit.module("Waterfall"), scene,
        Kit.spawn(scene, "Waterfall", LIP.x, LIP.y, LIP.z), {
            top = LIP, bottom = IMPACT, source = river.entity, sink = pool.entity,
            flowRate = 2.5, width = 4.2, windDirection = 105, windSpeed = 0.9,
            -- The drain has to be a weir at the lip, not a lake-sized sink: at the default 3 m the
            -- disc reaches two metres back up the channel and empties the approach, so the stream
            -- visibly stops short of the cliff.
            lipRadius = 1.1, impactRadius = 2.4,
            -- The crest has to throw the water. A ballistic drop gets steeper as it falls and a
            -- heightfield face can only get gentler, so below ~4 m/s the sheet passes through the
            -- rock on the way down (scripts/gen_terrain_maps.py prints that clearance).
            plunge = 4.0,
            -- A 10 m fall reads as a column only while there are enough drops in the air to
            -- overlap, and each drop has to cover ground at the camera's distance: at 0.42 m the
            -- sheet was a scatter of threads. The sprite already carries the vertical stretch, so
            -- the extra scale stays low and the size does the work.
            drops = 2600, dropSize = 0.8, streak = 1.4, opacity = 0.62,
            thickness = 1.6, spray = 420, mistRadius = 5.0, mistSize = 2.5, mistOpacity = 0.2
        })

    -- Rain you can see, over the whole cliff, and the weather that wets the world with it. The
    -- particles are left unparented from the camera on purpose: the emitter has to cover the
    -- falls, not follow whoever is looking.
    local rain = Kit.attach(Kit.module("Precipitation"), scene,
        Kit.spawn(scene, "RainField", 0, 0, 0), {
            -- 2000/s is the ceiling the component allows (maxParticles = density x life, clamped
            -- at 8192), and the field is 90 m rather than 120 so those drops are one per square
            -- metre instead of one per four: rain you can only measure is not rain you can see.
            kind = 0, intensity = 1.0, density = 2000, area = 90, spawnHeight = 34,
            fallSpeed = 21, windDirection = 105, windSpeed = 1.6, alpha = 0.72, streak = 7,
            -- The drop size is the whole trick. Precipitation's rain preset is 0.05 m, which at
            -- the 25 m the camera sits from an average drop is one pixel - the wide shot showed
            -- the storm as a dusting of dots in the sky. 0.16 m x (1+7) reads as a streak, and
            -- rain_sheet.png keeps its core 60% of the sprite wide so minification cannot shave
            -- it down to nothing the way textures/rain.png (4.5% of pixels above alpha 200) did.
            particleSize = 0.16, texture = "textures/rain_sheet.png", followCamera = false
        })
    local weather = Kit.attach(Kit.module("WeatherSystem"), scene,
        Kit.spawn(scene, "Weather", 0, 0, 0), {
            type = 0, intensity = 0.7, targetIntensity = 0.7, changeSpeed = 0.05,
            water = pool.entity, rainAmount = 0.02, fog = fog.entity, sky = sky.entity,
            waves = waves, windDirection = 105, gustStrength = 0.6,
            -- A drizzle that still lets you see the far ridge, not the 8 m sea-storm default.
            stormFogStart = 110, stormFogEnd = 620, skyDim = 0.2
        })
    rain.weather = weather

    -- A float to prove the pool is a real body of water and not a painted plane: it rides the
    -- current the fall sets up.
    local log = Kit.spawn(scene, "Log", 2.0, 2.2, 4.0)
    local body = Kit.addBoxBody(scene, log, 1.6, 0.4, 0.4)
    body.friction = 0.5
    Kit.attach(Kit.module("FloatOnWater"), scene, log, {
        water = pool.entity, buoyancy = 1.0, rideCurrent = true
    })

    -- ---- the headless proof: the sim has to be moving water, not just drawing it
    local probe = {
        name = "WaterfallProbe", frames = 0, every = 5.0, since = 0.0, started = 0.0
    }
    local function volume(water)
        local out = 0.0
        -- WaterSystem::getVolume answers in litres (WaterSystem.cpp:571), so the readout divides
        -- back to the cubic metres every other number here is already in.
        pcall(function() out = water:getSimVolume() / 1000.0 end)
        return out
    end
    local function depth(water, world)
        local out = 0.0
        pcall(function() out = water:getSimWaterDepth(world.x, world.z) end)
        return out
    end
    local function flow(water, world)
        local out = Vector3(0, 0, 0)
        pcall(function() out = water:getSimVelocity(world.x, world.z) end)
        return out
    end
    local first = nil
    probe.report = function(self)
        local riverVolume, poolVolume = volume(river), volume(pool)
        local at = flow(river, APPROACH)
        local speed = math.sqrt(at.x * at.x + at.z * at.z)
        local airborne = 0
        pcall(function() airborne = falls:getCount() end)
        print(string.format(
            "[waterfall] t=%5.1fs river %8.1f m3 (%.3f m above the weir, %.2f m/s)  "
            .. "pool %9.1f m3 (%.3f m at the fall)  moved over the lip %7.1f m3  in the air %5d",
            self.now(), riverVolume, depth(river, APPROACH), speed, poolVolume,
            depth(pool, IMPACT), falls:getMovedVolume(), airborne))
    end
    function probe.now() return Engine.systemTime - probe.started end
    -- A one-off walk down the channel: the volumes say the field holds water and the lip says it
    -- is not there, so the only way to tell which is true is to read every bed along the run.
    function probe.profile()
        local out = {}
        for i = 0, 10 do
            local z = -34.0 + i * 2.4
            local d = depth(river, Vector3(0, 0, z))
            local v = flow(river, Vector3(0, 0, z))
            out[#out + 1] = string.format("z=%5.1f %.3fm %.2fm/s", z, d,
                math.sqrt(v.x * v.x + v.z * v.z))
        end
        print("[waterfall] channel: " .. table.concat(out, "  "))
        -- The lake is the other half of the picture: shore foam is driven by the *depth buffer*
        -- distance, so a wall of white over the pool means the water is genuinely thin there,
        -- whatever the volume readout says. Walk the same axis and compare.
        out = {}
        for i = 0, 12 do
            local z = -6.0 + i * 3.0
            local v = flow(pool, Vector3(0, 0, z))
            out[#out + 1] = string.format("z=%5.1f %.3fm %.2fm/s", z, depth(pool, Vector3(0, 0, z)),
                math.sqrt(v.x * v.x + v.z * v.z))
        end
        print("[waterfall] lake: " .. table.concat(out, "  "))
    end
    function probe.onUpdate()
        probe.frames = probe.frames + 1
        probe.since = probe.since + number(Engine.deltatime, 0.0)
        if first == nil and probe.frames > 30 then
            first = volume(river) + volume(pool)
            probe.started = Engine.systemTime
        end
        if probe.since >= probe.every then
            probe.since = 0.0
            probe:report()
            if getenv("WATERFALL_PROFILE") ~= nil then probe.profile() end
        end
    end
    function probe.onKeyDown(_, key)
        if key == Input.KEY_r then
            river.simRain = math.min(river.simRain + 0.01, 0.12)
            print("[waterfall] rain up to " .. river.simRain .. " m/s")
        elseif key == Input.KEY_f then
            river.simRain = 0.0
            print("[waterfall] rain off - the shelf now only drains")
        elseif key == Input.KEY_u then
            falls:setFlow(falls.flowRate + 1.0)
            print("[waterfall] flow " .. falls.flowRate .. " m3/s")
        elseif key == Input.KEY_d then
            falls:setFlow(math.max(falls.flowRate - 1.0, 0.0))
            print("[waterfall] flow " .. falls.flowRate .. " m3/s")
        elseif key == Input.KEY_c then
            probe:report()
        elseif key == Input.KEY_ESCAPE then
            probe:report()
            print(string.format("[waterfall] volume held at the start %.1f m3", first or 0.0))
            Kit.report("waterfall")
            System.quit()
        end
    end
    RegisterEngineEvent(probe, "onUpdate")
    RegisterEngineEvent(probe, "onKeyDown")
    Kit.keep[#Kit.keep + 1] = probe

    Kit.report("waterfall")
    print("[waterfall] R/F rain, U/D flow, C report, ESC quits")
    probe:report()
end)
