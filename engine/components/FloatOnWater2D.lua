-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Buoyancy controller for a dynamic Body2D. The surface is a configurable 2D water plane
-- with an optional deterministic travelling wave; forces use the scene's Box2D gravity.
-- category: Water & Shore
local FloatOnWater2D = {
    properties = {
        { name = "waterLevel", displayName = "Water level (world units)", type = "float", default = 0.0 },
        { name = "hullDepth", displayName = "Hull depth (world units)", type = "float", default = 64.0 },
        { name = "buoyancy", displayName = "Buoyancy strength (1 = neutral)", type = "float", default = 1.0 },
        { name = "waterDrag", displayName = "Linear drag in water (1/s)", type = "float", default = 2.0 },
        { name = "waveAmplitude", displayName = "Wave amplitude (world units)", type = "float", default = 0.0 },
        { name = "waveLength", displayName = "Wave length (world units)", type = "float", default = 256.0 },
        { name = "waveSpeed", displayName = "Wave speed (radians/s)", type = "float", default = 1.0 },
        { name = "sinkPoint", displayName = "Sink point (submerged fraction 0..1)", type = "float", default = 0.9 },
        { name = "maxSpeed", displayName = "Max speed (world units/s, 0 = unlimited)", type = "float", default = 0.0 },
        { name = "active", displayName = "Active", type = "bool", default = true }
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
        print("[FloatOnWater2D] " .. message)
    end
end

function FloatOnWater2D:init()
    RegisterEngineEvent(self, "onFixedUpdate")
    self.hullDepth = math.max(self.hullDepth or 64.0, 0.001)
    self.buoyancy = math.max(self.buoyancy or 1.0, 0.0)
    self.waterDrag = math.max(self.waterDrag or 2.0, 0.0)
    self.waveLength = math.max(self.waveLength or 256.0, 0.001)
    self.waveAmplitude = math.max(self.waveAmplitude or 0.0, 0.0)
    self.sinkPoint = clamp(self.sinkPoint or 0.9, 0.0, 1.0)
    self.maxSpeed = math.max(self.maxSpeed or 0.0, 0.0)
    self.submerged = 0.0
    self.floating = false
    self.sunk = false
    self.lastWarn = 0.0
    local ok, body = pcall(function() return Body2D(self.scene, self.entity) end)
    self.body = (ok and body) or nil
end

function FloatOnWater2D:getSubmerged()
    return self.submerged or 0.0
end

function FloatOnWater2D:onFixedUpdate()
    if self.active == false then return end
    if not self.body then
        warn(self, "The entity needs a dynamic Body2D component.")
        return
    end

    local position = self.body.position
    local phase = position.x / self.waveLength + Engine.systemTime * (self.waveSpeed or 0.0)
    local surfaceY = self.waterLevel + math.sin(phase) * self.waveAmplitude
    self.submerged = clamp((surfaceY - position.y) / self.hullDepth + 0.5, 0.0, 1.0)

    local nowFloating = self.submerged > 0.05
    self.floating = nowFloating
    self.sunk = self.sinkPoint < 1.0 and self.submerged > self.sinkPoint
    if not nowFloating then return end

    local mass = self.body:getMass()
    if type(mass) ~= "number" or mass <= 0.0 then mass = 1.0 end
    local gravity = self.scene.gravity2D
    local lift = self.buoyancy * self.submerged
    if self.sunk then lift = lift * clamp(1.0 - (self.submerged - self.sinkPoint) * 2.0, 0.15, 1.0) end

    local force = Vector2(-gravity.x, -gravity.y) * (mass * lift)
    local velocity = self.body.linearVelocity
    force = force - velocity * (self.waterDrag * self.submerged * mass)
    local ok = pcall(function() self.body:applyForceToCenter(force, true) end)
    if not ok then
        warn(self, "Unable to apply buoyancy; check that the Body2D is dynamic.")
        return
    end

    if self.maxSpeed > 0.0 then
        local speed = velocity:length()
        if speed > self.maxSpeed then
            self.body.linearVelocity = velocity * (self.maxSpeed / speed)
        end
    end
end

return FloatOnWater2D
