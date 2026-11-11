-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- The sky as a system: a procedural dome with a sun, a moon whose phase comes from where the sun
-- actually is, stars, and a raymarched cloud deck you can put anywhere from a clear day to a
-- storm. Everything above the horizon in this demo is one forked sky shader driven by two Lua
-- components - DayNightCycle for the clock and Clouds for the weather - so the same Sky entity can
-- have a full moon behind broken cloud, or a new moon and nothing but stars.
--
-- Run it from a project that has the library installed and the fork compiled:
--   quantum-engine.exe --project <project> --scene demos/demo_sky --window 1280x720
-- Keys: [ / ] one hour back or forward, , / . cloud preset, - / = moon phase,
--       1 sun  2 moon  3 clouds  4 horizon, SPACE pause, ESC quits.
--
-- One run captures one frame, so a headless pass that wants the four moments of the day asks for
-- them one at a time: SKY_HOUR, SKY_PRESET, SKY_MOON and SKY_SHOT.
local Kit = require("demos.kit_demo")

local function number(value, fallback)
    if type(value) == "number" and value == value then return value end
    return fallback
end
local function getenv(name)
    local out = nil
    pcall(function() out = os.getenv(name) end)
    return out
end

local PRESETS = { "clear", "few", "overcast", "storm" }

-- Where the camera looks. "sun", "moon" and "up" are resolved against the clock after its first
-- step, because the sun's path is built from the cycle's own axis and tilt - guessing an azimuth
-- here would aim the camera at empty sky in any scene that changes one of them.
local AIM = { sun = true, moon = true, clouds = true, horizon = true }

Kit.start(function()
    local scene = Kit.createScene()
    scene.backgroundColor = Vector4(0.02, 0.03, 0.05, 1.0)
    -- The lake mirrors the sky through envColor = skyColour x globalIlluminationIntensity
    -- (RenderSystem.cpp:7835), and that setting's default of 0.1 leaves the water ink-black under
    -- a bright dome.
    scene.globalIlluminationIntensity = 0.6

    local sun = Kit.hold(Light(scene))
    sun.name = "Sun"
    sun:setType(LightType.DIRECTIONAL)
    sun:setIntensity(1.0)

    -- A second directional lamp is what makes the night a lit scene rather than an unlit one: the
    -- moon casts, so a full moon throws shadows and a new moon leaves the world to the stars.
    local moonLamp = Kit.hold(Light(scene))
    moonLamp.name = "Moon"
    moonLamp:setType(LightType.DIRECTIONAL)
    moonLamp:setIntensity(0.0)

    local sky = Kit.hold(SkyBox(scene))
    sky.name = "Sky"
    -- The rel path is what becomes the .sdat filename, so it has to match the copy the project
    -- ships in assets/shaders/ - the runtime never compiles a shader, it only reads the built one.
    -- customShader is bound as a read/write property, so the C++ setter name is not a Lua method.
    sky.customShader = "assets/shaders/sky_daynight"

    local fog = Kit.hold(Fog(scene))
    fog.name = "Fog"
    fog.color = Vector3(0.5, 0.6, 0.75)
    fog:setLinearStartEnd(220, 1400)

    local camera = Kit.hold(Camera(scene))
    camera.name = "Camera"
    camera.position = Vector3(0, 9, 0)
    camera.target = Vector3(0, 12, 100)
    scene:setCamera(camera)
    Kit.camera3D(camera, 58, 0.1, 6000)

    local terrain = Kit.hold(Terrain(scene))
    terrain.name = "Terrain"
    -- A field wide enough that its square edge stays out of frame at any horizon aim, and relief
    -- low enough that the lake plane is above its valleys: the coastline is what makes the water
    -- read, and a 12-unit relief buries a 1.4-unit plane everywhere the camera can see.
    terrain.size = 900
    terrain.maxHeight = 7
    terrain.resolution = 64
    terrain.levels = 8
    terrain:setHeightMap("textures/heightmap_hills.png")
    -- The three blend channels are the three detail layers, in that order, and everything they do
    -- not claim falls to the terrain's own colour. scripts/gen_blend_from_height.py wrote this map
    -- from the same height map: rock on the slopes, mud in the band the lake reaches, grass between.
    -- The JPGs are the project's own ground set - the kit ships only procedurally generated assets,
    -- so a project without them still renders, on the base colour below.
    terrain:setBlendMap("terrain_maps/blendmap_hills.png")
    terrain:setTextureDetailRed("textures/ground/ground-mossy-rock-color.jpg")
    terrain:setTextureDetailGreen("textures/ground/ground-grass-color.jpg")
    terrain:setTextureDetailBlue("textures/ground/ground-mud-color.jpg")
    terrain.textureBaseTiles = 14
    terrain.textureDetailTiles = 90
    -- Whatever weight the blend layers leave unclaimed is painted with the terrain's own colour,
    -- and a Mesh defaults to white.
    terrain.color = Vector4(0.30, 0.34, 0.26, 1.0)

    local lake = Kit.hold(Water(scene))
    lake.name = "Lake"
    lake.position = Vector3(0, 3.4, 0)
    lake.size = Vector2(820, 820)
    lake.subdivisions = 96
    lake.shallowColor = Vector3(0.30, 0.52, 0.55)
    lake.deepColor = Vector3(0.03, 0.14, 0.22)
    lake.depthFade = 2.2
    lake.shoreFoam = 0.05
    lake:load()
    Kit.attach(Kit.module("WaterWaves"), scene, lake.entity, {
        water = lake.entity, waveHeight = 0.14, waveLength = 9.0, waveSpeed = 0.6,
        waveSteepness = 0.3, crestFoam = 0.06, gustInterval = 9
    })

    -- Three boulders at the waterline: enough geometry for the moon's shadows to land on
    -- something. Shape(scene, id) is a view over an existing Mesh component, and Kit.spawn only
    -- builds a Transform, so asking the view to create geometry reads a component that is not
    -- there; the no-id constructor is the form that adds one, and it owns what it makes.
    for index, spot in ipairs({ Vector3(-9, 4.4, 16), Vector3(6, 4.2, 25), Vector3(-2, 4.0, 36) }) do
        local rock = Kit.hold(Shape(scene))
        rock.name = "Rock" .. index
        rock.position = spot
        rock.scale = Vector3(1.0 + index * 0.3, 0.62, 0.9 + index * 0.2)
        rock:createSphere(2.0 + index * 0.5, 14, 9)
        rock.color = Vector4(0.42, 0.40, 0.38, 1.0)
    end

    local hour = number(tonumber(getenv("SKY_HOUR")), 9.5)
    local presetName = getenv("SKY_PRESET") or "few"
    local moonPhase = number(tonumber(getenv("SKY_MOON")), 0.5)

    local cycle = Kit.attach(Kit.module("DayNightCycle"), scene,
        Kit.spawn(scene, "Cycle", 0, 0, 0), {
            light = sun.entity, moonLight = moonLamp.entity, sky = sky.entity, fog = fog.entity,
            dayLength = 240.0, startHour = hour, maxIntensity = 1.35, moonIntensity = 0.30,
            moonPhase = moonPhase, sunTilt = 23.5, skyBrightness = 1.0
        })
    local clouds = Kit.attach(Kit.module("Clouds"), scene,
        Kit.spawn(scene, "Clouds", 0, 0, 0), {
            sky = sky.entity, preset = PRESETS[1]
        })
    clouds:setPreset(presetName)

    -- The clock only fills its own vectors inside onUpdate, and the camera has to be aimed with
    -- them, so one step runs before anything is framed. Paused first: the step must not advance
    -- the hour the run was asked to show.
    cycle:setTime(hour)
    cycle:setPaused(true)
    cycle:onUpdate()

    -- The eye is lifted off the water and pulled back from the middle: at water level the nearby
    -- swell fills the lower half and the horizon stops being readable, and the relief still reaches
    -- seven units, so a lower eye lands inside a hill.
    local EYE = Vector3(0, 16, -70)
    local function aim(kind)
        local dir
        if kind == "sun" then dir = cycle:getSunDirection()
        elseif kind == "moon" then dir = cycle:getMoonDirection()
        elseif kind == "clouds" then dir = Vector3(0.18, 0.72, 0.66)
        else dir = Vector3(1.0, 0.06, 0.0) end
        -- A body just under the horizon is exactly when the sky is worth photographing, and aiming
        -- at the body itself points the camera at the ground. Keep the azimuth, lift the pitch.
        if dir.y < 0.06 then
            local flat = Vector3(dir.x, 0.0, dir.z)
            if flat:length() < 0.001 then flat = Vector3(1.0, 0.0, 0.0) end
            dir = flat:normalized() * 0.998 + Vector3(0.0, 0.06, 0.0)
        end
        camera.position = EYE
        camera.target = Vector3(EYE.x + dir.x * 100, EYE.y + dir.y * 100, EYE.z + dir.z * 100)
    end
    local wanted = getenv("SKY_SHOT") or "horizon"
    if not AIM[wanted] then wanted = "horizon" end
    aim(wanted)

    local announcer = {
        onPhase = function(_, name) print("[demo] sky: the clock reached " .. tostring(name)) end,
        onSunrise = function() print("[demo] sky: sunrise") end,
        onSunset = function() print("[demo] sky: sunset") end
    }
    cycle.onPhase, cycle.onPhaseMethod = announcer, "onPhase"
    cycle.onSunrise, cycle.onSunriseMethod = announcer, "onSunrise"
    cycle.onSunset, cycle.onSunsetMethod = announcer, "onSunset"

    local driver = { name = "SkyDemo", frames = 0, every = 4.0, since = 0.0, preset = 2 }
    function driver.state()
        local illumination = 0.0
        pcall(function() illumination = cycle:getIllumination() end)
        return string.format(
            "[sky] %s %-10s sun %+5.1f deg  moon %+5.1f deg (%2.0f%% lit)  clouds %s cover %.2f",
            cycle:getHourText(), cycle:getPhase(), cycle:getSunElevation(),
            cycle:getMoonElevation(), illumination * 100.0, tostring(clouds.preset), clouds.coverage)
    end
    function driver.onUpdate()
        driver.frames = driver.frames + 1
        driver.since = driver.since + number(Engine.deltatime, 0.0)
        if driver.since >= driver.every then
            driver.since = 0.0
            print(driver.state())
        end
    end
    function driver.onKeyDown(_, key)
        if key == Input.KEY_LEFT_BRACKET then
            cycle:setTime(cycle:getHour() - 1.0); cycle:onUpdate(); aim(wanted)
        elseif key == Input.KEY_RIGHT_BRACKET then
            cycle:setTime(cycle:getHour() + 1.0); cycle:onUpdate(); aim(wanted)
        elseif key == Input.KEY_COMMA or key == Input.KEY_PERIOD then
            driver.preset = driver.preset + (key == Input.KEY_PERIOD and 1 or -1)
            driver.preset = (driver.preset - 1) % #PRESETS + 1
            clouds:setPreset(PRESETS[driver.preset])
            clouds.preset = PRESETS[driver.preset]
            print("[sky] clouds -> " .. PRESETS[driver.preset])
        elseif key == Input.KEY_MINUS or key == Input.KEY_EQUAL then
            local step = key == Input.KEY_EQUAL and 0.125 or -0.125
            cycle:setMoonPhase(cycle:getMoonPhase() + step)
            print(string.format("[sky] moon phase %.3f", cycle:getMoonPhase()))
        elseif key == Input.KEY_1 then
            wanted = "sun"; aim(wanted)
        elseif key == Input.KEY_2 then
            wanted = "moon"; aim(wanted)
        elseif key == Input.KEY_3 then
            wanted = "clouds"; aim(wanted)
        elseif key == Input.KEY_4 then
            wanted = "horizon"; aim(wanted)
        elseif key == Input.KEY_SPACE then
            cycle:setPaused(not cycle.paused)
        elseif key == Input.KEY_ESCAPE then
            print(driver.state())
            Kit.report("sky")
            System.quit()
        end
    end
    RegisterEngineEvent(driver, "onUpdate")
    RegisterEngineEvent(driver, "onKeyDown")
    Kit.keep[#Kit.keep + 1] = driver

    Kit.report("sky")
    print(driver.state())
    print("[sky] [ ] hour, , . clouds, - = moon, 1 sun 2 moon 3 clouds 4 horizon, SPACE pause")
    Kit.watch("sky", 6.0)
end)
