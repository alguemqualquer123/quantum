-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Reusable wind force controller for dynamic Body3D entities. Unlike WindZone, this
-- controller samples the scene wind field directly, so it needs no manually maintained
-- list of bodies and remains deterministic in headless and shipped builds.
local WindForceController = {
    properties = {
        { name = "strength", displayName = "Wind response", type = "float", default = 1.0 },
        { name = "area", displayName = "Projected area (m2)", type = "float", default = 1.0 },
        { name = "drag", displayName = "Drag coefficient", type = "float", default = 1.0 },
        { name = "heightFade", displayName = "Height fade (metres, 0 = off)", type = "float", default = 0.0 },
        { name = "maxSpeed", displayName = "Max speed (m/s, 0 = unlimited)", type = "float", default = 0.0 },
        { name = "localDirection", displayName = "Local facing affects area", type = "bool", default = false }
    }
}

local function clamp(value, low, high)
    if value < low then return low end
    if value > high then return high end
    return value
end

local function warn(self, message)
    if Engine.systemTime - self.lastWarn > 1.0 then
        self.lastWarn = Engine.systemTime
        print("[WindForceController] " .. message)
    end
end

local function sampleWind(scene, position, seconds)
    -- Wind is a runtime utility and is absent from the tiny offline kit harness.
    -- Keep the fallback useful for validation and for projects built without the utility binding.
    if Wind and type(Wind.sample) == "function" then
        local ok, value = pcall(function() return Wind.sample(scene, position, seconds) end)
        if ok and value then return value end
    end
    local enabled, speed, direction = true, 0.0, 0.0
    pcall(function() enabled = scene.windEnabled end)
    pcall(function() speed = scene.windSpeed or 0.0 end)
    pcall(function() direction = scene.windDirection or 0.0 end)
    if enabled == false then return Vector2(0.0, 0.0) end
    local radians = math.rad(direction)
    return Vector2(math.cos(radians) * speed, -math.sin(radians) * speed)
end

function WindForceController:init()
    RegisterEngineEvent(self, "onFixedUpdate")
    self.strength = math.max(self.strength or 1.0, 0.0)
    self.area = math.max(self.area or 1.0, 0.0)
    self.drag = math.max(self.drag or 1.0, 0.0)
    self.heightFade = math.max(self.heightFade or 0.0, 0.0)
    self.maxSpeed = math.max(self.maxSpeed or 0.0, 0.0)
    self.lastWarn = 0.0
    local ok, body = pcall(function() return Body3D(self.scene, self.entity) end)
    self.body = (ok and body) or nil
end

function WindForceController:onFixedUpdate()
    if not self.body then
        warn(self, "The entity needs a dynamic Body3D component.")
        return
    end

    local dt = math.min(Engine.deltatime, 1.0 / 30.0)
    local position = self.body.position
    local wind = sampleWind(self.scene, Vector2(position.x, position.z), Engine.systemTime)
    local speed = wind:length()
    if speed <= 0.0001 then return end

    local mass = self.body.mass
    if type(mass) ~= "number" or mass <= 0.0 then mass = 1.0 end
    local response = self.strength * self.area * self.drag

    if self.heightFade > 0.0 then
        response = response * clamp(1.0 - math.abs(position.y) / self.heightFade, 0.0, 1.0)
    end

    if self.localDirection then
        local forward = self.body.rotation * Vector3(0.0, 0.0, 1.0)
        local facing = math.abs(forward.x * wind.x + forward.z * wind.y) / speed
        response = response * facing
    end

    local velocity = self.body.linearVelocity
    local relative = Vector3(wind.x - velocity.x, -velocity.y, wind.y - velocity.z)
    self.body:applyForce(relative * (response * mass))

    if self.maxSpeed > 0.0 then
        local horizontal = Vector3(velocity.x, 0.0, velocity.z)
        local horizontalSpeed = horizontal:length()
        if horizontalSpeed > self.maxSpeed then
            local scale = self.maxSpeed / horizontalSpeed
            self.body.linearVelocity = Vector3(velocity.x * scale, velocity.y, velocity.z * scale)
        end
    end
end

return WindForceController
