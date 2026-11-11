-- (c) SR VINIX
-- SPDX-License-Identifier: MIT
-- Terrain only: a bare CDLOD terrain under an orbiting camera, on the API that predates the scale
-- work, so the same file runs on an old binary.
--
-- Every wrapper is kept in a table the driver owns. The event registry roots the driver, so
-- nothing Lua has to survive is collectable - and it has to be deliberate: a `scene` local that no
-- closure refers to can be collected mid-run, after which every wrapper that still points at the
-- C++ scene it owned reads freed memory. That surfaces dozens of frames later as
-- "Component not registered before use" and a segfault, which looks exactly like an engine bug.
pcall(function() io.stdout:setvbuf("no") end)

local keep = {}
local function hold(object)
    keep[#keep + 1] = object
    return object
end

local scene = hold(Scene())
Engine.scene = scene

local sun = hold(Light(scene))
sun:setType(LightType.DIRECTIONAL)
sun.direction = Vector3(-0.30, -0.85, -0.40)
sun:setIntensity(1.5)

local camera = hold(Camera(scene))
camera:setPerspective(60.0, 1280.0 / 720.0, 0.1, 2000.0)
camera.autoResize = true
scene:setCamera(camera)
camera.position = Vector3(0, 11.2, 54.0)
camera.target = Vector3(0, 0, 0)

local terrain = hold(Terrain(scene))
terrain.size = 180
terrain.maxHeight = 8
terrain.resolution = 32
terrain.rootGridSize = 2
terrain.levels = 6
terrain:setHeightMap("height.png")
print("[terrainonly] built")

local d = { frames = 0 }
keep[#keep + 1] = d
d.onUpdate = function()
    d.frames = d.frames + 1
    if d.frames % 5 == 0 then print("[terrainonly] frame " .. d.frames) end
    local a = d.frames * 0.006
    camera.position = Vector3(math.sin(a) * 54.0, 11.2, math.cos(a) * 54.0)
    camera.target = Vector3(0, 0, 0)
end
d.keep = keep
RegisterEngineEvent(d, "onUpdate")
