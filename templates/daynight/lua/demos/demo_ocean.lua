-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Open water with real waves: a gusting Sea, crates that float and are carried by the current, a
-- swimmer at the camera, and a wake that punches ripples into the simulation. Run it from a project
-- that has the library installed:
--   quantum-engine.exe --project <project> --scene demos/demo_ocean --window 1280x720
local Kit = require("demos.kit_demo")

Kit.start(function()
    local scene = Kit.createScene()
    scene.backgroundColor = Vector4(0.28, 0.42, 0.58, 1.0)

    local sun = Kit.hold(Light(scene))
    sun.name = "Sun"
    sun:setType(LightType.DIRECTIONAL)
    sun.direction = Vector3(-0.35, -0.9, -0.25)
    sun:setIntensity(1.1)

    local camera = Kit.hold(Camera(scene))
    camera.name = "Camera"
    camera.position = Vector3(0, 4, 26)
    camera.target = Vector3(0, 0.6, 0)
    scene:setCamera(camera)
    Kit.camera3D(camera, 60, 0.1, 500)

    -- The surface is a 300 m plane with the CPU heightfield on: the waves below drive the look, the
    -- sim grid is what ripples, rain and floating bodies read.
    local water = Kit.hold(Water(scene))
    water.name = "Sea"
    water.position = Vector3(0, 0, 0)
    water.size = Vector2(300, 300)
    water.simulate = true
    water.subdivisions = 64
    water:load()

    local sky = Kit.hold(SkyBox(scene))
    sky.name = "Sky"
    sky.color = Vector4(0.36, 0.58, 0.86, 1.0)
    local fog = Kit.hold(Fog(scene))
    fog.name = "Fog"
    fog.color = Vector3(0.62, 0.76, 0.88)
    fog:setLinearStartEnd(60, 520)

    -- Sea state lives in one component: swell height, length, speed and how often a gust arrives.
    local waves = Kit.attach(Kit.module("WaterWaves"), scene, water.entity, {
        water = water.entity, waveHeight = 0.9, waveLength = 26, waveSpeed = 1.6,
        waveSteepness = 0.8, windDrift = 0.35, gustInterval = 12, gustStrength = 1.8
    })

    -- Floating bodies are Transform + Body3D + FloatOnWater: buoyancy is read from the sim grid, so
    -- a crate really bobs with the swell and drifts with the current.
    local function floater(name, x, z, size)
        local id = Kit.spawn(scene, name, x, 1.2, z)
        local body = Kit.addBoxBody(scene, id, size, size, size)
        body.friction = 0.6
        return Kit.attach(Kit.module("FloatOnWater"), scene, id, {
            water = water.entity, buoyancy = 1.0, rideCurrent = true, bob = 0.25
        })
    end
    floater("Crate", -7, 8, 1.4)
    floater("Barrel", 5, 12, 0.9)
    floater("Log", 0, -6, 2.0)

    -- The wake component follows a body and disturbs the grid under it; it splashes on its own when
    -- the subject is moving fast enough.
    local hull = floater("Buoy", 10, 2, 0.7)
    Kit.attach(Kit.module("WaterRipples"), scene, water.entity, {
        water = water.entity, follow = hull.entity, radius = 1.6, minSpeed = 0.6,
        strength = 0.5, trail = true, splashInterval = 2.5
    })

    -- A swimmer is a body at the surface plus the controller: W/A/S/D swim, space rises and the dive
    -- key goes under. The camera follows it through FollowCamera rather than through the controller.
    local swimmer = Kit.spawn(scene, "Swimmer", -3, 0.4, 14)
    Kit.addBoxBody(scene, swimmer, 0.6, 0.4, 1.4)
    Kit.attach(Kit.module("SwimController"), scene, swimmer, {
        water = water.entity, waves = waves, eyeHeight = 0.35, maxDepth = 6,
        swimSpeed = 4.5, autoSurface = true
    })
    Kit.attach(Kit.module("FollowCamera"), scene, camera.entity, {
        target = swimmer, smoothing = 0.35, lookHeight = 1.4
    })

    -- Weather on top: rain is a key press, and the storm only touches what the slots name.
    Kit.attach(Kit.module("WeatherSystem"), scene, Kit.spawn(scene, "Weather", 0, 0, 0), {
        water = water.entity, fog = fog.entity, sky = sky.entity, waves = waves,
        rainAmount = 0.5, changeSpeed = 0.35, toggleKey = 0
    })

    Kit.report("ocean")
    print("[demo] ocean: ESC closes it; the swell gusts on its own and the crates drift with it")
    Kit.watch("ocean", 6.0)
end)
