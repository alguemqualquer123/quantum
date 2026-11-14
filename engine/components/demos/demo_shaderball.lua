-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- The look-dev rig: a grid of spheres over one sky, so a material can be judged from its
-- reflection instead of from a number in the Inspector. The top row is dielectric, the bottom is
-- metal, and roughness walks left to right - which is the only arrangement that proves the GGX
-- mip chain of the prefiltered map is right. A single chrome ball cannot prove that: at
-- roughness 0 every prefilter is identical, so a broken chain shows up as row two looking like
-- row one.
--
-- The sky has to be an authored cubemap, not the procedural dome. RenderSystem::updateSkyEnvironment
-- bails out unless sky.texture.getNumFaces() == 6, and hasIBL is nothing but sky.envMapsLoaded, so
-- under a customShader dome a metal sphere reflects the emptyCubeBlack the mesh was bound with and
-- comes out ink-black. Bake the dome first:
--
--   for face in px nx py ny pz nz:
--     CUBE_FACE=$face CUBE_HOUR=18 \
--       quantum-engine.exe --project <project> --scene demos/demo_cubemap \
--       --window 512x512 --screenshot <tmp>/$face.png --capture-frame 40
--   python scripts/assemble_sky_cubemap.py assemble <tmp> <project>/assets/textures/sky_18.png
--
--   BALL_SKYMAP=textures/sky_18.png \
--     quantum-engine.exe --project <project> --scene demos/demo_shaderball \
--     --window 1280x720 --screenshot <out> --capture-frame 60
--
-- BALL_METAL and BALL_ROUGH take comma-separated factors and are what turns the grid into a single
-- hero ball (BALL_METAL=1 BALL_ROUGH=0). BALL_HIDE_SKY leaves the dome invisible: the cube keeps
-- lighting and reflecting, so the row reads against a flat background instead of against itself.
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

-- "0, 0.25,0.5" -> {0, 0.25, 0.5}. Anything unparsable falls out rather than poisoning the grid
-- with a nil that only shows up three spheres later.
local function factors(raw, fallback)
    local out = {}
    for token in string.gmatch(raw or fallback, "[^,%s]+") do
        local v = tonumber(token)
        if v then out[#out + 1] = math.max(0.0, math.min(1.0, v)) end
    end
    if #out == 0 then out = { 0.0, 1.0 } end
    return out
end

Kit.start(function()
    local map = getenv("BALL_SKYMAP")
    if not map then
        print("[shaderball] BALL_SKYMAP is required: a procedural dome has no cubemap, so every")
        print("[shaderball] metal sphere would reflect emptyCubeBlack. See the header of this file.")
        System.quit()
        return
    end

    local scene = Kit.createScene()
    scene.backgroundColor = Vector4(0.05, 0.06, 0.08, 1.0)
    -- envColor is skyColour x globalIlluminationIntensity, and the setting defaults to 0.1 - a
    -- reflection knob, not a diffuse one, so leaving it at the default dims the whole grid.
    scene.globalIlluminationIntensity = 1.0

    local sky = Kit.hold(SkyBox(scene))
    sky.name = "Sky"
    -- The cube already carries the sky, so any tint here multiplies into both the background and
    -- the reflection.
    sky.color = Vector4(1.0, 1.0, 1.0, 1.0)
    sky:setTexture(map)
    if getenv("BALL_HIDE_SKY") then sky.visible = false end

    -- One lamp, and it is only there for the dielectric row: a metal sphere has no diffuse lobe at
    -- all, so direct light cannot tell rough metal from matte plastic - the reflection can.
    local sun = Kit.hold(Light(scene))
    sun.name = "Sun"
    sun:setType(LightType.DIRECTIONAL)
    sun:setDirection(-0.35, -0.55, -0.75)
    sun:setIntensity(1.2)

    local metals = factors(getenv("BALL_METAL"), "0,1")
    local roughs = factors(getenv("BALL_ROUGH"), "0,0.25,0.5,0.75,1")
    local radius = number(tonumber(getenv("BALL_RADIUS")), 1.0)
    local spacing = radius * 2.6

    local nCols, nRows = #roughs, #metals
    local balls = 0
    for row, metal in ipairs(metals) do
        for col, rough in ipairs(roughs) do
            local ball = Kit.hold(Shape(scene))
            ball.name = string.format("Ball_%d_%d", row, col)
            ball.position = Vector3(
                (col - (nCols + 1) * 0.5) * spacing,
                (nRows + 1) * 0.5 * spacing - row * spacing,
                0)
            ball:createSphere(radius, 64, 32)
            -- false on the component, and the mesh is then compiled without the IBL variant at all:
            -- the sphere keeps its diffuse term and never samples the prefiltered cube.
            ball.receiveIBL = true
            local material = ball:getMaterial()
            material.metallicFactor = metal
            material.roughnessFactor = rough
            material.baseColorFactor = Vector4(1.0, 1.0, 1.0, 1.0)
            ball:setMaterial(material)
            balls = balls + 1
        end
    end

    local width = (#roughs - 1) * spacing + radius * 2.0
    local height = (#metals - 1) * spacing + radius * 2.0
    local camera = Kit.hold(Camera(scene))
    camera.name = "Camera"
    scene:setCamera(camera)
    Kit.camera3D(camera, 30, 0.1, 400)
    -- Framed off the wider axis, and the taller one gets the 1.8 because a landscape window cuts
    -- rows before it cuts columns. A 1x5 row and a single hero ball both have to fit.
    local distance = math.max(width, height * 1.8) * 1.15
    camera.position = Vector3(0, 0, distance)
    camera.target = Vector3(0, 0, 0)

    print(string.format("[shaderball] %dx%d grid metal={%s} rough={%s} radius %.2f d %.1f map=%s",
        #metals, #roughs, table.concat(metals, ","), table.concat(roughs, ","),
        radius, distance, map))

    local driver = { name = "ShaderBall", frames = 0, every = 4.0, since = 0.0 }
    function driver.onUpdate()
        driver.frames = driver.frames + 1
        driver.since = driver.since + number(Engine.deltatime, 0.0)
        if driver.since >= driver.every then
            driver.since = 0.0
            print(string.format("[shaderball] %d frames, %d balls, sky %s",
                driver.frames, balls, tostring(map)))
        end
    end
    function driver.onKeyDown(_, key)
        if key == Input.KEY_ESCAPE then
            Kit.report("shaderball")
            System.quit()
        end
    end
    RegisterEngineEvent(driver, "onUpdate")
    RegisterEngineEvent(driver, "onKeyDown")
    Kit.keep[#Kit.keep + 1] = driver

    Kit.report("shaderball")
    Kit.watch("shaderball", 6.0)
end)
