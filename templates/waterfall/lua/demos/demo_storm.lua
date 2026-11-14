-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- A storm you can drive: rain, snow and hail that you can see, lightning whose thunder arrives
-- late, wind that tilts the drops, fog that closes in and ripples on the sea. Run it from a project
-- that has the library installed:
--   quantum-engine.exe --project <project> --scene demos/demo_storm --window 1280x720
-- Keys: 1 rain, 2 snow, 3 hail, 4 calm, L forces a strike, SPACE shakes the camera, ESC quits.
local Kit = require("demos.kit_demo")

Kit.start(function()
    local scene = Kit.createScene()
    scene.backgroundColor = Vector4(0.24, 0.28, 0.34, 1.0)

    -- The three entities the weather scripts look for by name when their slots are empty are Sun,
    -- Sky and Fog, so they are created with exactly those names. The sun stays dim: a storm is
    -- lit by its own flashes, not by a blue sky.
    local sun = Kit.hold(Light(scene))
    sun.name = "Sun"
    sun:setType(LightType.DIRECTIONAL)
    sun:setIntensity(0.35)

    local sky = Kit.hold(SkyBox(scene))
    sky.name = "Sky"

    local fog = Kit.hold(Fog(scene))
    fog.name = "Fog"

    local water = Kit.hold(Water(scene))
    water.name = "Water"
    water.position = Vector3(0, 0, 60)
    water.size = Vector2(220, 220)
    water.simulate = true
    water.subdivisions = 64
    water:load()

    local camera = Kit.hold(Camera(scene))
    camera.name = "Camera"
    camera.position = Vector3(0, 14, 46)
    camera.target = Vector3(0, 2, 0)
    scene:setCamera(camera)
    Kit.camera3D(camera, 60, 0.1, 500)

    -- The ground the bolts strike and the drops land on. Two things make a Lua-built terrain
    -- actually appear, and both are checked before the geometry is generated: the scene must
    -- already have its camera, and the heightmap must not be empty. Either one failing runs
    -- MeshSystem::resetTerrainToUnbuilt, which clears needUpdateTerrain for good, so a terrain
    -- configured in the wrong order never comes back and the renderer retries its mesh forever.
    -- The relief itself is applied in the vertex shader, so the CPU mesh stays a flat grid.
    local terrain = Kit.hold(Terrain(scene))
    terrain.name = "Terrain"
    terrain.size = 220
    terrain.maxHeight = 4
    terrain.resolution = 64
    terrain.levels = 6
    terrain:setHeightMap("textures/heightmap_hills.png")

    -- Some scenery so a bolt has something to strike and the drops have a scale. The mesh is
    -- optional: a project without assets still runs the whole storm, just on flat ground.
    local posts = {}
    for index = 1, 6 do
        local model = Kit.hold(Model(scene))
        model.name = "Post" .. index
        local angle = (index - 1) / 6.0 * math.pi * 2.0
        model.position = Vector3(math.cos(angle) * 22.0, 1.5, math.sin(angle) * 22.0)
        model.scale = Vector3(1.2, 3.0 + (index % 3), 1.2)
        if model:loadOBJ("cube.obj") then posts[#posts + 1] = model end
    end
    if #posts == 0 then
        print("[demo] storm: no cube.obj in the project, so the storm runs over bare terrain")
    end

    -- The rain falls around the camera and the camera orbits, so the emitter box is dragged along
    -- every frame - that is the part a static demo would never exercise.
    local tower = Kit.spawn(scene, "Tower", 0, 0, 0)
    local shake = Kit.attach(Kit.module("CameraShake"), scene, camera.entity, {
        -- applyToCamera off: OrbitCamera owns the pose and reads this script's offset instead, so
        -- the two never fight over camera.position.
        applyToCamera = false, amplitude = 0.45, rotationAmplitude = 2.4, traumaDecay = 1.1
    })
    Kit.attach(Kit.module("OrbitCamera"), scene, camera.entity, {
        target = tower, radius = 46, height = 12, lookHeight = 3, autoSpin = 4.0,
        mouseLook = false, minPitch = 4, maxPitch = 40, shake = shake
    })

    local waves = Kit.attach(Kit.module("WaterWaves"), scene, water.entity, {
        water = water.entity, waveHeight = 0.35, waveLength = 14, waveSpeed = 1.1,
        waveSteepness = 0.6, gustInterval = 8
    })

    -- WeatherSystem owns fog density, sky dimming, rain into the water grid, drop ripples and wave
    -- gusts; it is the clock every other script reads. Its own particle slot is left empty on
    -- purpose: Precipitation builds and names its entities "Precipitation"/"PrecipitationSprites",
    -- so WeatherSystem's "Rain" fallback finds nothing and the two never write particles.rate
    -- against each other.
    local weather = Kit.attach(Kit.module("WeatherSystem"), scene, Kit.spawn(scene, "Weather", 0, 0, 0), {
        fog = fog.entity, water = water.entity, sky = sky.entity, waves = waves,
        type = 0, intensity = 0.55, targetIntensity = 0.9, changeSpeed = 0.05,
        rainAmount = 2.5, windDirection = 215, gustStrength = 1.6,
        toggleKey = 0, cycleKey = 0
    })

    -- The visible precipitation. Set `texture` to a sprite in the project (the kit ships
    -- assets/textures/rain.png and snow.png) and the drops stop being squares.
    local precip = Kit.attach(Kit.module("Precipitation"), scene, Kit.spawn(scene, "Precip", 0, 0, 0), {
        kind = 0, intensity = 1.0, density = 520, area = 70, spawnHeight = 28,
        windDirection = 215, windSpeed = 6.0, sway = 1.4, streak = 7.0, alpha = 0.75,
        texture = "textures/rain.png", frameName = "drop",
        weather = weather, followWeather = true, followCamera = true
    })

    -- The fade needs a widget to paint: a FULL_LAYOUT Image covers the viewport, and ScreenFade
    -- drives its colour and alpha. A project without a UI scene can use the post-process route
    -- instead, but only once the scene really has a pass to write.
    local cover = Kit.hold(Image(scene))
    cover.name = "Fade"
    cover.anchorPreset = AnchorPreset.FULL_LAYOUT
    cover.color = Vector4(0.0, 0.0, 0.0, 1.0)

    local fade = Kit.attach(Kit.module("ScreenFade"), scene, Kit.spawn(scene, "FadeDriver", 0, 0, 0), {
        image = cover.entity, startBlack = true, autoFadeIn = true, fadeInTime = 2.5,
        flashTime = 0.16
    })

    -- Lightning needs nothing wired - it makes its own flash light, bolt and sound entities - but a
    -- strike only reads as one with the shake and the screen flash attached to it.
    local storm = Kit.attach(Kit.module("Lightning"), scene, Kit.spawn(scene, "Bolt", 0, 0, 0), {
        weather = weather, followWeather = true, stormThreshold = 0.3,
        strikesPerMinute = 30, minDistance = 35, maxDistance = 640, nearDistance = 240,
        cloudHeight = 58, flashIntensity = 1600, flashRange = 480, flickerCount = 3,
        flashDuration = 0.5, boltSegments = 16, boltJitter = 10, boltForks = 2,
        thunderPath = "sounds/thunder.ogg", thunderVolume = 0.9, rumbleRepeat = 2,
        shake = shake, shakeAmount = 0.6, fade = fade, screenFlash = 0.32,
        triggerKey = Input.KEY_L
    })

    -- One table drives every weather knob, so the keys and the timed script below cannot drift
    -- apart: a mode is a weather type, a precipitation kind and a wind, set together. WindZone is
    -- deliberately absent - it pushes Body3D bodies, and this scene has none.
    local modes = {
        { name = "rain", type = 0, kind = 0, intensity = 0.9, wind = 6.0, streak = 7.0 },
        { name = "snow", type = 1, kind = 1, intensity = 0.7, wind = 2.5, streak = 0.0 },
        { name = "hail", type = 0, kind = 2, intensity = 1.0, wind = 9.0, streak = 0.0 },
        { name = "calm", type = 0, kind = 0, intensity = 0.0, wind = 1.5, streak = 4.0 }
    }

    local driver = { name = "StormDriver", frame = 0, mode = 0 }

    function driver.apply(index)
        local mode = modes[index]
        if not mode then return end
        driver.mode = index
        weather.type = mode.type
        weather.targetIntensity = mode.intensity
        weather.windDirection = 215
        precip.streak = mode.streak
        -- setWind re-runs every particle initialiser, so it goes last: the streak above has to be
        -- in place before the look is rebuilt.
        precip:setKind(mode.kind)
        precip:setWind(215, mode.wind)
        print(string.format("[demo] storm: %s, weather target %.2f, %d drops live",
            mode.name, mode.intensity, precip:getCount()))
    end

    function driver.onUpdate()
        driver.frame = driver.frame + 1
        -- Timed on frames, not seconds: a headless run goes many times faster than real time, so a
        -- wall-clock script would only ever reach its first event.
        if driver.frame == 30 then
            local distance = storm:strike(120.0)
            print(string.format("[demo] storm: scripted strike at %.0f m, %d so far", distance, storm:getStrikeCount()))
        elseif driver.frame == 90 then
            driver.apply(2)
        elseif driver.frame == 150 then
            driver.apply(3)
        elseif driver.frame == 210 then
            driver.apply(1)
        end
        if driver.frame % 60 == 0 then
            print(string.format("[demo] storm: mode %d, intensity %.2f, %d drops, %d strikes (last %.0f m)",
                driver.mode, weather:getIntensity(), precip:getCount(), storm:getStrikeCount(), storm:getDistance()))
        end
    end

    function driver.onKeyDown(_, key)
        if key == Input.KEY_1 then driver.apply(1)
        elseif key == Input.KEY_2 then driver.apply(2)
        elseif key == Input.KEY_3 then driver.apply(3)
        elseif key == Input.KEY_4 then driver.apply(4) end
    end

    RegisterEngineEvent(driver, "onUpdate")
    RegisterEngineEvent(driver, "onKeyDown")
    Kit.keep[#Kit.keep + 1] = driver

    Kit.report("storm")
    print("[demo] storm: 1 rain, 2 snow, 3 hail, 4 calm, L strikes; a scripted strike lands at frame 30")
    Kit.watch("storm", 4.0)
end)
