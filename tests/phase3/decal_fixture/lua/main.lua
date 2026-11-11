-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Runtime counterpart of scenes/main.scene. The engine runner executes Lua entry points;
-- keeping this tiny scene here lets the visual harness capture the actual decal pass while
-- the YAML remains the editor-authored source of truth for doctor/serialization checks.
local keep = {}

local scene = Scene()
scene.backgroundColor = Vector4(0.035, 0.045, 0.07, 1.0)
scene.lightState = true
scene.depthPrepassEnabled = true
scene.renderDistance = 100.0
scene.ssaoEnabled = false
scene.ssrEnabled = false
scene.ssgiEnabled = false
Engine.scene = scene
keep.scene = scene

local receiver = Shape(scene)
receiver.name = "DecalTarget"
receiver:createPlane(4.0, 4.0)
receiver.position = Vector3(0.0, 0.0, 0.0)
receiver.color = Vector4(0.18, 0.22, 0.28, 1.0)
receiver.receiveLights = true
receiver.receiveShadows = true
keep.receiver = receiver

local camera = Camera(scene)
camera.name = "CaptureCamera"
camera.position = Vector3(0.0, 4.5, 5.0)
camera.target = Vector3(0.0, 0.0, 0.0)
camera:setPerspective(60.0, 16.0 / 9.0, 0.1, 100.0)
camera.autoResize = true
scene:setCamera(camera)
keep.camera = camera

local light = Light(scene)
light.name = "KeyLight"
light:setType(LightType.DIRECTIONAL)
light.direction = Vector3(-0.35, -0.8, -0.45)
light.color = Vector3(1.0, 0.92, 0.82)
light.intensity = 3.0
keep.light = light

local decal = Decal(scene)
decal.name = "DecalVolume"
decal.enabled = true
decal.position = Vector3(0.0, 0.12, 0.0)
decal.color = Vector3(0.95, 0.06, 0.025)
decal.size = Vector3(2.4, 0.3, 2.4)
decal.opacity = 0.94
decal.depthFade = 0.08
keep.decal = decal
