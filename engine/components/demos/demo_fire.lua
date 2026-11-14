-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- A camp that catches fire: a bonfire, a ring of crates that light each other, a shed that burns
-- away, a torch with a fuel clock, and rain you can call down to put the whole thing out. Run it
-- from a project that has the library installed:
--   quantum-engine.exe --project <project> --scene demos/demo_fire --window 1280x720
-- Keys: R rain, C clear sky, X extinguish everything, F re-light the bonfire, ESC quit.
local Kit = require("demos.kit_demo")

Kit.start(function()
    local scene = Kit.createScene()
    scene.backgroundColor = Vector4(0.02, 0.03, 0.06, 1.0)

    -- A moon dim enough that the bonfire is the thing lighting the camp: with a bright directional
    -- light the flicker and the light range read as nothing.
    local moon = Kit.hold(Light(scene))
    moon.name = "Moon"
    moon:setType(LightType.DIRECTIONAL)
    moon:setIntensity(0.12)
    moon.color = Vector3(0.55, 0.65, 0.9)

    local camera = Kit.hold(Camera(scene))
    camera.name = "Camera"
    camera.position = Vector3(0, 8, 24)
    camera.target = Vector3(0, 1, 0)
    scene:setCamera(camera)
    Kit.camera3D(camera, 60, 0.1, 400)

    local ground = Kit.hold(Terrain(scene))
    ground.name = "Ground"
    ground.size = 160
    ground.maxHeight = 3
    ground.resolution = 64
    ground.levels = 6
    ground:setHeightMap("textures/heightmap_hills.png")

    -- Named "Sky" because that is the fallback WeatherSystem looks for: with a sky to dim, the rain
    -- reads as weather over the camp instead of as a warning that nothing is wired.
    local sky = Kit.hold(SkyBox(scene))
    sky.name = "Sky"
    sky.color = Vector4(0.02, 0.03, 0.06, 1.0)

    -- The same orbit the storm demo drives; the fire layers follow their own entities, not the
    -- camera, so moving the view only changes what is on screen.
    local camp = Kit.spawn(scene, "Camp", 0, 0, 0)
    Kit.attach(Kit.module("OrbitCamera"), scene, camera.entity, {
        target = camp, radius = 24, height = 9, lookHeight = 2, autoSpin = 2.5, mouseLook = false,
        minPitch = 6, maxPitch = 45
    })

    -- Geometry is optional here: a project with cube.obj gets crates you can watch char, and one
    -- without it still runs the whole fire system on the fuel entities alone. A Burnable chars the
    -- Mesh of the entity it sits on, so when the crate loaded the fuel goes onto that entity instead
    -- of a bare Transform next to it.
    local crates, hasModel = {}, false
    for index = 1, 6 do
        local angle = (index - 1) / 6.0 * math.pi * 2.0
        local x, z = math.cos(angle) * 7.0, math.sin(angle) * 7.0
        local model = Kit.hold(Model(scene))
        model.name = "Crate" .. index
        model.position = Vector3(x, 0.6, z)
        model.scale = Vector3(1.1, 1.2, 1.1)
        if model:loadOBJ("cube.obj") then
            hasModel = true
            crates[#crates + 1] = model.entity
        else
            crates[#crates + 1] = Kit.spawn(scene, "Fuel" .. index, x, 0.4, z)
        end
    end
    if not hasModel then
        print("[demo] fire: no cube.obj in the project, so the fuel has no geometry to char")
    end

    local weather = Kit.attach(Kit.module("WeatherSystem"), scene, Kit.spawn(scene, "Weather", 0, 0, 0), {
        sky = sky.entity,
        type = 0, intensity = 0.0, targetIntensity = 0.0, changeSpeed = 0.35,
        rainAmount = 2.2, windDirection = 120, gustStrength = 1.2, toggleKey = 0, cycleKey = 0
    })
    Kit.attach(Kit.module("Precipitation"), scene, Kit.spawn(scene, "Precip", 0, 0, 0), {
        kind = 0, intensity = 0.0, density = 420, area = 60, spawnHeight = 24,
        windDirection = 120, windSpeed = 3.0, streak = 6.0, alpha = 0.7,
        texture = "textures/rain.png", frameName = "drop",
        weather = weather, followWeather = true, followCamera = true
    })

    -- The bonfire is the ignition source: it never runs out of fuel and it reaches the crate ring,
    -- which each carries two metres further to the next one.
    local bonfire = Kit.attach(Kit.module("Fire"), scene, Kit.spawn(scene, "Bonfire", 0, 0, 0), {
        startOnLoad = false, burning = false,
        intensity = 1.0, flameSize = 1.5, flameHeight = 2.4, flameRate = 170, emberRate = 20,
        lightRange = 22, lightIntensity = 10.0, flicker = 0.45, flickerSpeed = 8.0,
        windDirection = 120, windSpeed = 0.9, spreadRadius = 8.5, spreadInterval = 0.6,
        weather = weather, heatDamage = 14.0, damageRadius = 2.0, fallbackSubjectName = "Camera"
    })

    local fuel = {}
    for index, id in ipairs(crates) do
        fuel[#fuel + 1] = Kit.attach(Kit.module("Burnable"), scene, id, {
            -- Staggered so the chain is readable: every crate lighting at once would look like one
            -- flash instead of a fire moving through the camp.
            intensity = 0.75 + (index % 3) * 0.1,
            fuel = 4.0 + index * 1.5,
            flammability = 1.0, igniteChance = 1.0,
            burnSize = 1.0, burnHeight = 1.6,
            tint = hasModel, spreadRadius = 8.0,
            weather = weather, burnAway = index == 6
        })
    end

    -- The shed is fuel without geometry, so it chars nothing and only burn-away proves it went.
    local shed = Kit.attach(Kit.module("Burnable"), scene, Kit.spawn(scene, "Shed", 11, 0, -4), {
        intensity = 1.0, fuel = 9.0, burnSize = 3.2, burnHeight = 2.8, burnRate = 0.8,
        spreadRadius = 0.0, weather = weather, burnAway = true
    })

    -- A torch is a Fire with a fuel clock and no spread: it burns down on its own and stays dark.
    local torch = Kit.attach(Kit.module("Fire"), scene, Kit.spawn(scene, "Torch", -10, 1.8, 5), {
        startOnLoad = false, burning = false,
        fuel = 12.0, burnRate = 1.0, flameSize = 0.45, flameHeight = 0.8, flameRate = 70,
        smoke = true, embers = true, lightRange = 9, lightIntensity = 4.5,
        weather = weather, spreadRadius = 0.0
    })

    local state = { name = "FireDemo", frame = 0, raining = false }

    local function litCount()
        local lit = 0
        for _, entry in ipairs(fuel) do if entry.lit then lit = lit + 1 end end
        if shed.lit then lit = lit + 1 end
        return lit
    end

    -- Precipitation and the fire both read the weather, so rain is one knob: WeatherSystem drives
    -- the drops through followWeather and puts the fires out through the weather slot.
    local function sprinkle(on)
        state.raining = on
        if on then
            weather:rain()
            weather.targetIntensity = 1.0
        else
            weather:clear(4.0)
        end
    end

    function state:onUpdate()
        self.frame = self.frame + 1
        -- Timed on frames: a headless run goes many times faster than real time, so a wall-clock
        -- script would only ever reach its first event.
        if self.frame == 40 then
            bonfire:ignite()
            print("[demo] fire: the bonfire is lit")
        elseif self.frame == 140 then
            torch:ignite()
            print("[demo] fire: the torch is lit, 12 s of fuel")
        elseif self.frame == 400 then
            -- Touching the far side of the ring by hand, so the two ways a fire starts (a flame
            -- placed in the scene, and spread from a neighbour) both get exercised in one run.
            local started = fuel[#fuel]:ignite()
            print(string.format("[demo] fire: shed-side crate hand-lit (%s), the rest has to catch",
                tostring(started)))
        end
        if self.frame % 90 == 0 then
            print(string.format("[demo] fire: %d of %d fuel items burning, %d sprites on the bonfire,"
                .. " torch %s, rain %.2f, shed char %.2f",
                litCount(), #fuel + 1, bonfire:getParticleCount(),
                torch:isBurning() and string.format("%.1f s fuel", torch:getFuel()) or "out",
                weather:getIntensity(), shed:getChar()))
        end
    end

    function state.onKeyDown(_, key)
        if key == Input.KEY_R then
            sprinkle(true)
            print("[demo] fire: rain coming down")
        elseif key == Input.KEY_C then
            sprinkle(false)
            print("[demo] fire: sky clearing")
        elseif key == Input.KEY_X then
            bonfire:extinguish()
            torch:extinguish()
            for _, entry in ipairs(fuel) do entry:extinguish() end
            shed:extinguish()
            print("[demo] fire: everything doused")
        elseif key == Input.KEY_F then
            bonfire:ignite()
            print("[demo] fire: the bonfire is back")
        end
    end

    RegisterEngineEvent(state, "onUpdate")
    RegisterEngineEvent(state, "onKeyDown")
    Kit.keep[#Kit.keep + 1] = state

    Kit.report("fire")
    print("[demo] fire: R rain, C clear, X douse all, F relight the bonfire; the ring catches by"
        .. " itself once the bonfire is going")
    Kit.watch("fire", 5.0)
end)
