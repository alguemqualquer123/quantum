-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Keep this fixture deliberately static. A moving camera makes a screenshot compare
-- measure the capture time instead of the renderer, especially for shadow cascades.
local keep = {}

local scene = Scene()
scene.backgroundColor = Vector4(0.025, 0.035, 0.06, 1.0)
scene.lightState = true
scene.depthPrepassEnabled = true
scene.renderDistance = 100.0
scene.ssaoEnabled = false
scene.ssrEnabled = false
scene.ssgiEnabled = false
scene.globalIlluminationIntensity = 0.8
Engine.scene = scene
keep.scene = scene

local ground = Shape(scene)
ground.name = "Ground"
ground:createPlane(14.0, 10.0)
ground.position = Vector3(0.0, 0.0, 0.0)
ground.color = Vector4(0.20, 0.24, 0.31, 1.0)
ground.receiveLights = true
ground.castShadows = true
ground.receiveShadows = true
keep.ground = ground

local function makeBox(name, position, size, color)
    local object = Shape(scene)
    object.name = name
    object:createBox(size.x, size.y, size.z)
    object.position = position
    object.color = color
    object.receiveLights = true
    object.castShadows = true
    object.receiveShadows = true
    keep[name] = object
    return object
end

makeBox("RedBlock", Vector3(-2.4, 1.0, 0.2), Vector3(1.8, 2.0, 1.8),
         Vector4(0.72, 0.10, 0.08, 1.0))
makeBox("GoldBlock", Vector3(0.0, 0.65, -0.25), Vector3(1.4, 1.3, 1.4),
         Vector4(0.82, 0.54, 0.08, 1.0))
makeBox("BlueBlock", Vector3(2.25, 0.85, 0.35), Vector3(1.6, 1.7, 1.6),
         Vector4(0.08, 0.28, 0.78, 1.0))

local sphere = Shape(scene)
sphere.name = "Sphere"
sphere:createSphere(0.9, 32, 20)
sphere.position = Vector3(0.0, 1.05, 2.25)
sphere.color = Vector4(0.10, 0.68, 0.50, 1.0)
sphere.receiveLights = true
sphere.castShadows = true
sphere.receiveShadows = true
keep.sphere = sphere

local camera = Camera(scene)
camera.name = "CaptureCamera"
camera.position = Vector3(0.0, 5.6, 10.5)
camera.target = Vector3(0.0, 1.0, 0.0)
camera:setPerspective(52.0, 16.0 / 9.0, 0.1, 100.0)
camera.autoResize = true
scene:setCamera(camera)
keep.camera = camera

local sun = Light(scene)
sun.name = "KeyLight"
sun:setType(LightType.DIRECTIONAL)
sun.direction = Vector3(-0.35, -0.8, -0.45)
sun.color = Vector3(1.0, 0.90, 0.78)
sun.intensity = 2.6
sun:setShadows(true)
keep.sun = sun

local fill = Light(scene)
fill.name = "FillLight"
fill:setType(LightType.POINT)
fill.position = Vector3(0.0, 4.0, 4.0)
fill.color = Vector3(0.20, 0.35, 1.0)
fill.intensity = 1.2
fill.range = 12.0
keep.fill = fill
