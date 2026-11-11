-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Reusable wind force controller for dynamic Body2D entities. It samples the same
-- deterministic scene wind field used by the 3D controller and applies relative drag.
local WindForceController2D = {
    properties = {
        { name = "strength", displayName = "Wind response", type = "float", default = 1.0 },
        { name = "area", displayName = "Projected area (m2)", type = "float", default = 1.0 },
        { name = "drag", displayName = "Drag coefficient", type = "float", default = 1.0 },
        { name = "heightFade", displayName = "Height fade (metres, 0 = off)", type = "float", default = 0.0 },
        { name = "maxSpeed", displayName = "Max speed (m/s, 0 = unlimited)", type = "float", default = 0.0 }
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
        print("[WindForceController2D] " .. message)
    end
end

local function sampleWind(scene, position, seconds)
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

function WindForceController2D:init()
    RegisterEngineEvent(self, "onFixedUpdate")
    self.strength = math.max(self.strength or 1.0, 0.0)
    self.area = math.max(self.area or 1.0, 0.0)
    self.drag = math.max(self.drag or 1.0, 0.0)
    self.heightFade = math.max(self.heightFade or 0.0, 0.0)
    self.maxSpeed = math.max(self.maxSpeed or 0.0, 0.0)
    self.lastWarn = 0.0
    local ok, body = pcall(function() return Body2D(self.scene, self.entity) end)
    self.body = (ok and body) or nil
end

function WindForceController2D:onFixedUpdate()
    if not self.body then
        warn(self, "The entity needs a dynamic Body2D component.")
        return
    end

    local position = self.body.position
    local wind = sampleWind(self.scene, position, Engine.systemTime)
    local speed = wind:length()
    if speed <= 0.0001 then return end

    local mass = self.body:getMass()
    if type(mass) ~= "number" or mass <= 0.0 then mass = 1.0 end
    local response = self.strength * self.area * self.drag
    if self.heightFade > 0.0 then
        response = response * clamp(1.0 - math.abs(position.y) / self.heightFade, 0.0, 1.0)
    end

    local velocity = self.body.linearVelocity
    local relative = Vector2(wind.x - velocity.x, wind.y - velocity.y)
    self.body:applyForceToCenter(relative * (response * mass), true)

    if self.maxSpeed > 0.0 then
        local currentSpeed = velocity:length()
        if currentSpeed > self.maxSpeed then
            self.body.linearVelocity = velocity * (self.maxSpeed / currentSpeed)
        end
    end
end

return WindForceController2D
