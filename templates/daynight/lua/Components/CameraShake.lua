-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Trauma based camera shake, the usual game-feel answer to an explosion or a landing.
-- category: Camera & Controllers
--
-- It produces an offset (self.offset / self.roll) every frame. Attached to the camera entity with
-- ApplyToCamera on, it displaces that camera by itself. Wired into FollowCamera's or OrbitCamera's
-- Shake slot instead, turn ApplyToCamera off so the two scripts do not fight over the pose.
--
-- Other scripts reach it by holding this entity in a slot declared with type = "CameraShake" and
-- calling addTrauma(amount).
local CameraShake = {
    properties = {
        { name = "applyToCamera", displayName = "Apply to my own camera", type = "bool", default = true },
        { name = "maxTrauma", displayName = "Maximum trauma", type = "float", default = 1.0 },
        { name = "traumaDecay", displayName = "Trauma decay per second", type = "float", default = 1.6 },
        { name = "frequency", displayName = "Shakes per second", type = "float", default = 22.0 },
        { name = "amplitude", displayName = "Maximum offset (metres)", type = "float", default = 0.35 },
        { name = "rotationAmplitude", displayName = "Maximum roll (degrees)", type = "float", default = 2.0 },
        { name = "testKey", displayName = "Preview key (0 = space)", type = "int", default = 0 },
        { name = "testAmount", displayName = "Preview trauma", type = "float", default = 0.6 }
    }
}

local function clamp(value, low, high)
    if value < low then return low end
    if value > high then return high end
    return value
end

-- Deterministic hash noise: the engine ships no noise helper for Lua, and a shake only needs
-- decorrelated channels, not a smooth field.
local function noise(seed)
    local x = math.sin(seed * 12.9898 + 78.233) * 43758.5453
    return (x - math.floor(x)) * 2.0 - 1.0
end

function CameraShake:init()
    RegisterEngineEvent(self, "onUpdate")

    self.applyToCamera = self.applyToCamera ~= false
    self.maxTrauma = self.maxTrauma or 1.0
    self.traumaDecay = self.traumaDecay or 1.6
    self.frequency = self.frequency or 22.0
    self.amplitude = self.amplitude or 0.35
    self.rotationAmplitude = self.rotationAmplitude or 2.0
    self.testAmount = self.testAmount or 0.6
    -- 0 is GLFW's unknown key, so an untouched property can default to space.
    if not self.testKey or self.testKey == 0 then
        self.testKey = Input.KEY_SPACE
    end

    self.trauma = 0.0
    self.elapsed = 0.0
    self.offset = Vector3(0.0, 0.0, 0.0)
    self.roll = 0.0
    self.previousTest = false
    self.base = nil
end

-- Callable from any other component that holds this entity in a CameraShake slot.
function CameraShake:addTrauma(amount)
    self.trauma = clamp(self.trauma + (amount or 1.0), 0.0, self.maxTrauma)
end

function CameraShake:onUpdate()
    local dt = math.min(Engine.deltatime, 0.05)

    local pressed = Input.isKeyPressed(self.testKey)
    if pressed and not self.previousTest then
        self:addTrauma(self.testAmount)
    end
    self.previousTest = pressed

    if self.trauma <= 0.0 and self.base == nil then
        return
    end

    local camera
    if self.applyToCamera then
        local ok, handle = pcall(function()
            return Camera(self.scene, self.entity)
        end)
        camera = (ok and handle) or nil
        -- The unshaken pose is captured on the first shaking frame, so the offset never
        -- accumulates against the position this script wrote last frame.
        if camera and not self.base then
            self.base = {position = camera.position, target = camera.target}
        end
    end

    self.trauma = clamp(self.trauma - self.traumaDecay * dt, 0.0, self.maxTrauma)
    self.elapsed = self.elapsed + dt

    local shake = self.trauma * self.trauma
    local phase = self.elapsed * self.frequency

    self.offset = Vector3(
        noise(phase) * self.amplitude * shake,
        noise(phase + 91.7) * self.amplitude * shake,
        noise(phase + 173.3) * self.amplitude * shake
    )
    self.roll = noise(phase + 251.5) * self.rotationAmplitude * shake

    if self.trauma <= 0.0 then
        self.offset = Vector3(0.0, 0.0, 0.0)
        self.roll = 0.0

        if camera and self.base then
            camera.position = Vector3(self.base.position.x, self.base.position.y, self.base.position.z)
            if self.base.target then
                camera.target = self.base.target
            end
        end
        self.base = nil
        return
    end

    if camera and self.base then
        local target = self.base.target
        camera.position = Vector3(
            self.base.position.x + self.offset.x,
            self.base.position.y + self.offset.y,
            self.base.position.z + self.offset.z
        )
        if target then
            camera.target = Vector3(
                target.x + self.offset.x * 0.4,
                target.y + self.offset.y * 0.4,
                target.z + self.offset.z * 0.4
            )
        end
    end
end

return CameraShake
