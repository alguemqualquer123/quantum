-- (c) SR VINIX
-- SPDX-License-Identifier: MIT
-- Crash bisect: the same orbit as main.lua with no terrain in the scene. If this survives, the
-- terrain is what the crash needs.
pcall(function() io.stdout:setvbuf("no") end)

local keep = {}
local function hold(o) keep[#keep+1]=o return o end
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
print("[orbitonly] built")

local d = { frames = 0, keep = keep }
d.onUpdate = function()
    d.frames = d.frames + 1
    if d.frames % 5 == 0 then print("[orbitonly] frame " .. d.frames) end
    local a = d.frames * 0.006
    camera.position = Vector3(math.sin(a) * 54.0, 11.2, math.cos(a) * 54.0)
    camera.target = Vector3(0, 0, 0)
end
RegisterEngineEvent(d, "onUpdate")
