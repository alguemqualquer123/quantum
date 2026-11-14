-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- A beach out of one script: sea, sand, sky, fog, living waves, crates that float on the surface and
-- an ambient loop. Run it from a project that has the library installed:
--   quantum-engine.exe --project <project> --scene demos/demo_beach --window 1280x720
local Kit = require("demos.kit_demo")

Kit.start(function()
    local scene = Kit.createScene()
    -- The colour setters are bound as scalar arguments in Lua, so every vector value goes through
    -- the property instead: Scene:setBackgroundColor(Vector4) and Light:setDirection(Vector3) are
    -- rejected by the bindings.
    scene.backgroundColor = Vector4(0.55, 0.72, 0.88, 1.0)

    local sun = Kit.hold(Light(scene))
    sun.name = "Sun"
    sun:setType(LightType.DIRECTIONAL)
    sun.direction = Vector3(-0.42, -0.86, -0.28)
    sun:setIntensity(1.25)

    local camera = Kit.hold(Camera(scene))
    camera.name = "Camera"
    camera.position = Vector3(0, 7, 28)
    camera.target = Vector3(0, 0.5, 0)
    scene:setCamera(camera)
    Kit.camera3D(camera, 60, 0.1, 300)

    -- The shore needs a beach to meet: size and height are the ones that read well with the presets.
    local terrain = Kit.hold(Terrain(scene))
    terrain.name = "Terrain"
    terrain.size = 120
    terrain.maxHeight = 5
    terrain.resolution = 64
    terrain.levels = 6
    -- An empty heightmap tears the terrain down and clears needUpdateTerrain for good, so the
    -- mesh never appears and the renderer retries it forever. The kit ships one; the relief is
    -- applied by the vertex shader, so the CPU mesh stays the flat grid it is built from.
    terrain:setHeightMap("textures/heightmap_hills.png")

    local water = Kit.hold(Water(scene))
    water.name = "Water"
    water.position = Vector3(0, 0, 10)
    water:load()

    local sky = Kit.hold(SkyBox(scene))
    sky.name = "Sky"
    local fog = Kit.hold(Fog(scene))
    fog.name = "Fog"

    -- One component carries the whole look: the preset paints water, sand, sky and fog at once, and
    -- clarity / wavePower / shorelineFoam are the only knobs left to turn.
    local waves = Kit.attach(Kit.module("WaterWaves"), scene, water.entity, {
        waveHeight = 0.42, waveLength = 16, waveSpeed = 1.2, waveSteepness = 0.7, gustInterval = 9
    })
    Kit.attach(Kit.module("BeachShore"), scene, water.entity, {
        water = water.entity, terrain = terrain.entity, sky = sky.entity, fog = fog.entity,
        waves = waves, preset = 0, clarity = 0.85, wavePower = 0.6, shorelineFoam = 0.75
    })

    -- A floating body is a Transform entity, a real Body3D with a shape, and FloatOnWater on top.
    local function floater(name, x, z, size)
        local id = Kit.spawn(scene, name, x, 1.4, z)
        local body = Kit.addBoxBody(scene, id, size, size, size)
        body.friction = 0.7
        return Kit.attach(Kit.module("FloatOnWater"), scene, id, {
            water = water.entity, buoyancy = 1.0, rideCurrent = true
        }), id
    end
    floater("Crate", -5, 12, 1.2)
    floater("Barrel", 4, 16, 0.9)

    -- Ambient audio needs the project's own file, so the slot is left empty on purpose: the component
    -- prints one line and disables itself instead of throwing.
    local ambience = Kit.hold(Sound(scene))
    ambience.name = "Surf"
    Kit.attach(Kit.module("AmbientSound"), scene, ambience.entity, {
        file = "", listener = camera.entity, minDistance = 6, maxDistance = 90
    })

    -- The fade runs through the user post-process chain, so the demo needs no UI asset to open on black.
    Kit.attach(Kit.module("ScreenFade"), scene, Kit.spawn(scene, "Fade", 0, 0, 0), {
        usePostProcess = true, startBlack = true, autoFadeIn = true, fadeInTime = 2.5
    })

    Kit.report("beach")
    print("[demo] beach: ESC closes it; the waves gust on their own and the crates ride them")
    Kit.watch("beach", 4.0)
end)
