-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- One clock runs the whole island: the sun walks the sky, a storm rolls in with rain that fills the
-- sea, wind pushes the crates, a ferry crosses the inlet and a mast falls into the water. Run it from
-- a project that has the library installed:
--   quantum-engine.exe --project <project> --scene demos/demo_daynight --window 1280x720
local Kit = require("demos.kit_demo")

Kit.start(function()
    local scene = Kit.createScene()
    scene.backgroundColor = Vector4(0.5, 0.62, 0.78, 1.0)

    -- DayNightCycle finds its Light/SkyBox/Fog by name when the slots are empty, and the names it
    -- falls back to are Sun / Sky / Fog, so the entities are created with exactly those.
    local sun = Kit.hold(Light(scene))
    sun.name = "Sun"
    sun:setType(LightType.DIRECTIONAL)
    sun:setIntensity(1.0)

    local camera = Kit.hold(Camera(scene))
    camera.name = "Camera"
    camera.position = Vector3(18, 12, 34)
    camera.target = Vector3(0, 1.5, 0)
    scene:setCamera(camera)
    Kit.camera3D(camera, 60, 0.1, 350)

    local terrain = Kit.hold(Terrain(scene))
    terrain.name = "Terrain"
    terrain.size = 160
    terrain.maxHeight = 6
    terrain.resolution = 64
    terrain.levels = 6
    -- An empty heightmap tears the terrain down and clears needUpdateTerrain for good, so the
    -- mesh never appears and the renderer retries it forever. The kit ships one; the relief is
    -- applied by the vertex shader, so the CPU mesh stays the flat grid it is built from.
    terrain:setHeightMap("textures/heightmap_hills.png")

    local water = Kit.hold(Water(scene))
    water.name = "Water"
    water.position = Vector3(0, 0, 14)
    water.size = Vector2(220, 220)
    water.simulate = true
    water.subdivisions = 64
    water:load()

    local sky = Kit.hold(SkyBox(scene))
    sky.name = "Sky"
    local fog = Kit.hold(Fog(scene))
    fog.name = "Fog"

    -- The announcer is a plain table: a hook slot is called as target[method](target, value), so it
    -- needs the methods, not an entity. Passing an entity id instead makes the call a no-op.
    local announcer = {
        onSunrise = function() print("[demo] daynight: sunrise, the fog lifts") end,
        onSunset = function() print("[demo] daynight: sunset, the lights go down") end,
        onHour = function(_, hour) print(string.format("[demo] daynight: %02d:00", hour % 24)) end
    }

    -- Starting at 17:30 means the first minute of the run passes through golden hour, night and the
    -- moon, which is the part a slow cycle never shows.
    Kit.attach(Kit.module("DayNightCycle"), scene, Kit.spawn(scene, "Cycle", 0, 0, 0), {
        light = sun.entity, sky = sky.entity, fog = fog.entity,
        dayLength = 48, startHour = 17.5, maxIntensity = 1.25, moonIntensity = 0.2,
        sunTilt = 23.5, toggleKey = 0,
        onSunrise = announcer, onSunset = announcer, onHour = announcer
    })

    local waves = Kit.attach(Kit.module("WaterWaves"), scene, water.entity, {
        water = water.entity, waveHeight = 0.5, waveLength = 18, waveSpeed = 1.3,
        waveSteepness = 0.7, gustInterval = 10
    })

    -- Weather reaches into everything through slots: rain lands on the water grid, thickens the fog,
    -- darkens the sky and gusts the waves. An empty slot simply skips that target.
    Kit.attach(Kit.module("WeatherSystem"), scene, Kit.spawn(scene, "Weather", 0, 0, 0), {
        water = water.entity, fog = fog.entity, sky = sky.entity, waves = waves,
        type = 0, intensity = 0.0, targetIntensity = 0.8, changeSpeed = 0.08,
        rainAmount = 3.0, windDirection = 210, gustStrength = 1.5, toggleKey = 0, cycleKey = 0
    })

    -- Wind pushes the bodies it is told about: three slots plus a comma separated name list, because
    -- Lua cannot walk the scene.
    local function crate(name, x, y, z, size)
        local id = Kit.spawn(scene, name, x, y, z)
        local body = Kit.addBoxBody(scene, id, size, size, size)
        body.friction = 0.4
        return id, body
    end
    local leafA = crate("LeafA", -6, 1.0, 6, 0.6)
    local leafB = crate("LeafB", -3, 1.0, 8, 0.5)
    local leafC = crate("LeafC", 2, 1.0, 9, 0.7)
    Kit.attach(Kit.module("WindZone"), scene, Kit.spawn(scene, "Wind", 0, 0, 0), {
        push = true, direction = 210, drift = 25, strength = 3.5,
        gustInterval = 6, gustStrength = 2.2, gustRamp = 1.2, radius = 40, factor = 1.0,
        subject = leafA, targetA = leafB, targetB = leafC
    })

    -- The ferry is two anchor entities and a deck: riders within carryRadius move with it.
    -- The deck is created KINEMATIC rather than retyped after the load: PhysicsSystem feeds a
    -- kinematic body the delta between its Transform and its Jolt position, so it pushes what stands
    -- on it, and Body3D:setType on an added body asserts in a Debug engine.
    local deckId = Kit.spawn(scene, "Ferry", -14, 0.6, 4)
    Kit.addBoxBody(scene, deckId, 4, 0.4, 3, BodyType.KINEMATIC)
    Kit.attach(Kit.module("MovePlatform"), scene, deckId, {
        from = Kit.spawn(scene, "Pier", -14, 0.6, 4),
        to = Kit.spawn(scene, "Landing", 14, 0.6, 4),
        speed = 3.0, ease = 1.0, pingPong = true, pauseAtEach = 1.0,
        carryRadius = 3.0, carryHeight = 0.6, riderA = crate("Cargo", -14, 1.2, 4, 0.8)
    })

    -- A body that falls on its own: asleep until wakeAfter, continuous collision so it never passes
    -- through the deck, and it falls again every few seconds.
    local mastId, mastBody = crate("Mast", 6, 14, 2, 1.1)
    mastBody.gravityFactor = 1.0
    Kit.attach(Kit.module("FallingObject"), scene, mastId, {
        startAsleep = true, wakeAfter = 2.5, antiTunnel = true,
        impactDelta = 1.5, respawnTime = 8.0, freezeOnLand = false
    })

    -- Buoyancy comes from the same grid the rain fills, so the barrel bobs with the swell.
    local barrelId = crate("Barrel", 10, 1.0, 16, 0.9)
    Kit.attach(Kit.module("FloatOnWater"), scene, barrelId, {
        water = water.entity, buoyancy = 1.0, rideCurrent = true, bob = 0.3
    })

    -- The post-process chain is the only way to open on black without a UI asset in the project.
    Kit.attach(Kit.module("ScreenFade"), scene, Kit.spawn(scene, "Fade", 0, 0, 0), {
        usePostProcess = true, startBlack = true, autoFadeIn = true, fadeInTime = 3.0
    })

    Kit.report("daynight")
    print("[demo] daynight: 48 s per full day, the storm builds as the sun goes down")
    Kit.watch("daynight", 8.0)
end)
