-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Interaction feedback on water: punches a disturbance into the shallow-field sim when the
-- category: Water & Shore
-- watched entity slaps down or wakes through it, and fires splash sound/particle slots.
local WaterRipples = {
    properties = {
        { name = "water", displayName = "Water", type = "entity" },
        { name = "fallbackName", displayName = "Water name if slot is empty", type = "string", default = "Water" },
        { name = "follow", displayName = "Entity to watch (0 = this one)", type = "entity" },
        { name = "radius", displayName = "Disturbance radius (metres)", type = "float", default = 1.2 },
        { name = "minSpeed", displayName = "Minimum speed to disturb (m/s)", type = "float", default = 1.5 },
        { name = "strength", displayName = "Disturbance strength (0..1)", type = "float", default = 0.6 },
        { name = "trail", displayName = "Leave a wake trail", type = "bool", default = true },
        { name = "trailInterval", displayName = "Trail interval (seconds)", type = "float", default = 0.12 },
        { name = "splashInterval", displayName = "Splash cooldown (seconds)", type = "float", default = 0.4 },
        { name = "splashSound", displayName = "Splash sound entity", type = "entity" },
        { name = "splashParticles", displayName = "Splash particles entity", type = "entity" },
        { name = "burstCount", displayName = "Splash burst count (particles)", type = "int", default = 24 }
    }
}

local function clamp(v, lo, hi) if v < lo then return lo end if v > hi then return hi end return v end

local function entityId(ref)
    if type(ref) == "number" then return ref ~= 0 and ref or nil end
    if ref and type(ref.entity) == "number" then return ref.entity ~= 0 and ref.entity or nil end
    return nil
end

local function warn(self, message)
    if Engine.systemTime - self.lastWarn > 1.0 then
        self.lastWarn = Engine.systemTime
        print("[WaterRipples] " .. message)
    end
end

function WaterRipples:init()
    RegisterEngineEvent(self, "onUpdate")

    self.radius = math.max(self.radius or 1.2, 0.05)
    self.minSpeed = math.max(self.minSpeed or 1.5, 0.01)
    self.strength = clamp(self.strength or 0.6, 0.0, 1.0)
    self.trailInterval = math.max(self.trailInterval or 0.12, 0.01)
    self.splashInterval = math.max(self.splashInterval or 0.4, 0.01)
    self.burstCount = math.max(self.burstCount or 24, 1)

    self.previousY = nil
    self.wasBelow = false
    self.lastTrail = 0.0
    self.lastSplash = -100.0
    self.lastWarn = 0.0
    self.effectWarned = false
end

function WaterRipples:resolveWater()
    if self.waterHandle then
        return self.waterHandle
    end
    local id = entityId(self.water)
    if not id and self.fallbackName and self.fallbackName ~= "" then
        id = entityId(self.scene:findEntity(self.fallbackName))
    end
    if not id then
        return nil
    end
    local ok, water = pcall(function()
        return Water(self.scene, id)
    end)
    if ok and water then
        self.waterHandle = water
    end
    return self.waterHandle
end

function WaterRipples:fireSplash(position)
    local id = entityId(self.splashSound)
    if id then
        local ok = pcall(function()
            Sound(self.scene, id):play()
        end)
        if not ok and not self.effectWarned then
            self.effectWarned = true
            warn(self, "Splash sound slot has no playable Sound component.")
        end
    end
    local particlesId = entityId(self.splashParticles)
    if particlesId then
        local ok = pcall(function()
            Particles(self.scene, particlesId):addBurst(0.0, self.burstCount)
        end)
        if not ok and not self.effectWarned then
            self.effectWarned = true
            warn(self, "Splash particles slot has no Particles component.")
        end
    end
end

function WaterRipples:onUpdate()
    local water = self:resolveWater()
    if not water then
        warn(self, "No water: drag the Water entity into the Water slot.")
        return
    end

    local watched = entityId(self.follow) or self.entity
    local ok, object = pcall(function()
        return Object(self.scene, watched)
    end)
    if not ok or not object then
        warn(self, "The watched entity has no transform: check the Follow slot.")
        return
    end

    local dt = math.min(Engine.deltatime, 0.05)
    local position = object.position
    local surfaceY = water:getHeight(position.x, position.z)
    local below = position.y < surfaceY

    -- Prefer the solver's own velocity; fall back to finite differences for kinematic movers.
    local vertical, horizontal = 0.0, 0.0
    local okBody, body = pcall(function()
        return Body3D(self.scene, watched)
    end)
    if okBody and body then
        local velocity = body.linearVelocity
        vertical = velocity.y
        horizontal = math.sqrt(velocity.x * velocity.x + velocity.z * velocity.z)
    elseif self.previousY then
        vertical = (position.y - self.previousY) / math.max(dt, 0.0001)
    end
    self.previousY = position.y

    -- A splash only counts on the frame the body crosses the plane going down, fast enough.
    if self.wasBelow == false and below and vertical < -self.minSpeed
        and Engine.systemTime - self.lastSplash > self.splashInterval then
        self.lastSplash = Engine.systemTime
        local power = clamp(self.strength * (-vertical / self.minSpeed) * 0.5, 0.05, 1.0)
        pcall(function()
            water:addDisturbance(Vector3(position.x, surfaceY, position.z), self.radius * 1.4, power)
        end)
        self:fireSplash(position)
    end

    if below and self.trail and horizontal > self.minSpeed * 0.5
        and Engine.systemTime - self.lastTrail > self.trailInterval then
        self.lastTrail = Engine.systemTime
        local power = clamp(self.strength * (horizontal / self.minSpeed) * 0.25, 0.02, 0.6)
        pcall(function()
            water:addDisturbance(Vector3(position.x, surfaceY, position.z), self.radius, power)
        end)
    end

    self.wasBelow = below
end

return WaterRipples
