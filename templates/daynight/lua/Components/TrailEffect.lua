-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Speed-reactive particle trail: reads an entity's position each frame and feeds a Particles
-- category: Screen Effects
-- component with a rate, emitter switch, life and puff size scaled by how fast the subject moves.
local TrailEffect = {
    properties = {
        { name = "particles", displayName = "Particles entity (blank = my own)", type = "entity" },
        { name = "followSubject", displayName = "Follow another entity", type = "bool", default = false },
        { name = "subject", displayName = "Subject entity", type = "entity" },
        { name = "subjectName", displayName = "Subject name if slot is empty", type = "string", default = "Player" },
        { name = "ratePerSpeed", displayName = "Particles per second, per m/s", type = "float", default = 12.0 },
        { name = "maxRate", displayName = "Maximum rate (per second)", type = "int", default = 240 },
        { name = "minSpeed", displayName = "Speed that starts the trail (m/s)", type = "float", default = 0.5 },
        { name = "maxSpeed", displayName = "Speed that saturates it (m/s)", type = "float", default = 18.0 },
        { name = "baseLife", displayName = "Particle life at min speed (seconds)", type = "float", default = 0.25 },
        { name = "lifeGain", displayName = "Extra life at max speed (seconds)", type = "float", default = 0.4 },
        { name = "baseSize", displayName = "Particle size at min speed", type = "float", default = 0.12 },
        { name = "sizeGain", displayName = "Extra size at max speed", type = "float", default = 0.2 },
        { name = "speedSmoothing", displayName = "Speed smoothing (1/s, 0 = raw)", type = "float", default = 10.0 },
        { name = "worldSpace", displayName = "Emit in world space", type = "bool", default = false },
        { name = "burstCount", displayName = "burst() particle count", type = "int", default = 20 },
        { name = "enabled", displayName = "Enabled", type = "bool", default = true }
    }
}

local function num(value, fallback) return type(value) == "number" and value or fallback end

local function clamp(value, low, high)
    if value < low then return low end
    if value > high then return high end
    return value
end

local function entityId(value)
    if type(value) == "number" then return value end
    if type(value) == "table" or type(value) == "userdata" then return num(value.entity, 0) end
    return 0
end

-- Positions reach Lua as Vector3; only the numeric channels are trusted before the distance maths.
local function pointOf(object)
    if object == nil then return nil end
    local p = object.position
    if type(p) ~= "table" and type(p) ~= "userdata" then return nil end
    return Vector3(num(p.x, 0.0), num(p.y, 0.0), num(p.z, 0.0))
end

function TrailEffect:init()
    RegisterEngineEvent(self, "onUpdate")
    self.rate, self.speed, self.smoothed = 0, 0.0, 0.0
    self.previous, self.writtenLife, self.writtenSize = nil, -1.0, -1.0
    self.trailObject, self.subjectObject, self.lastWarn = nil, nil, -1.0
    local id = entityId(self.particles)
    if id <= 0 then id = entityId(self.entity) end
    local ok, handle = pcall(function() return Particles(self.scene, id) end)
    self.handle = (ok and handle) or nil
    if self.handle and self.worldSpace then self.handle.localSpace = false end
end

function TrailEffect:warn(text)
    local time = num(Engine.systemTime, 0.0)
    if time - self.lastWarn < 1.0 then return end
    self.lastWarn = time
    print("[TrailEffect] " .. text)
end

function TrailEffect:objectOf(value, fallbackName)
    local id = entityId(value)
    if id <= 0 and fallbackName and fallbackName ~= "" then
        local ok, found = pcall(function() return self.scene:findEntity(fallbackName) end)
        id = ok and num(found, 0) or 0
    end
    if id <= 0 then return nil end
    local ok, object = pcall(function() return Object(self.scene, id) end)
    return (ok and object) or nil
end

function TrailEffect:resolveObjects()
    if self.trailObject == nil then self.trailObject = self:objectOf(self.particles or self.entity, nil) end
    if not self.followSubject then
        self.subjectObject = self.trailObject
    elseif self.subjectObject == nil then
        self.subjectObject = self:objectOf(self.subject, self.subjectName or "Player")
    end
end

-- Rewrites only what moved: rate, emitter, life and size are component state that rebuild the system.
function TrailEffect:drive()
    local minSpeed = num(self.minSpeed, 0.5)
    local maxSpeed = math.max(num(self.maxSpeed, 18.0), minSpeed + 0.01)
    local span = clamp((self.smoothed - minSpeed) / (maxSpeed - minSpeed), 0.0, 1.0)
    local rate = math.floor(clamp(self.smoothed * math.max(num(self.ratePerSpeed, 12.0), 0.0),
        0.0, math.max(num(self.maxRate, 240), 0)))
    local moving = self.smoothed >= minSpeed and self.enabled ~= false

    if self.handle.rate ~= rate then self.handle.rate = rate; self.rate = rate end
    if self.handle.emitter ~= moving then self.handle.emitter = moving end

    local life = num(self.baseLife, 0.25) + span * math.max(num(self.lifeGain, 0.4), 0.0)
    if math.abs(life - self.writtenLife) > 0.01 then
        self.handle:setLifeInitializer(math.max(life, 0.01))
        self.writtenLife = life
    end
    local size = num(self.baseSize, 0.12) + span * math.max(num(self.sizeGain, 0.2), 0.0)
    if math.abs(size - self.writtenSize) > 0.005 then
        self.handle:setSizeInitializer(math.max(size, 0.01))
        self.writtenSize = size
    end
end

function TrailEffect:onUpdate()
    local dt = math.min(num(Engine.deltatime, 1.0 / 60.0), 0.05)
    if self.handle == nil then
        self:warn("attach it to an entity holding a Particles component")
        return
    end
    self:resolveObjects()
    local point = pointOf(self.subjectObject)
    if point == nil then
        self:warn(self.followSubject and "subject entity not found" or "no position to read")
        return
    end

    local raw = self.previous and dt > 0.0 and self.previous:distance(point) / dt or 0.0
    self.previous = point
    local smoothing = num(self.speedSmoothing, 10.0)
    self.smoothed = smoothing > 0.0 and self.smoothed + (raw - self.smoothed) * clamp(smoothing * dt, 0.0, 1.0) or raw
    self.speed = raw
    self:drive()
end

function TrailEffect:setEnabled(value)
    self.enabled = value ~= false
    if self.handle then self.handle.emitter = self.enabled end
end

function TrailEffect:setRate(value)
    if self.handle then self.handle.rate = math.max(math.floor(num(value, 0)), 0) end
end

function TrailEffect:burst()
    if self.handle == nil then return end
    pcall(function()
        self.handle:addBurst(0.0, math.max(math.floor(num(self.burstCount, 20)), 1))
    end)
end

function TrailEffect:getSpeed() return self.speed end

function TrailEffect:clear()
    self.smoothed, self.previous, self.writtenLife, self.writtenSize = 0.0, nil, -1.0, -1.0
    if self.handle == nil then return end
    pcall(function()
        self.handle:clearBursts()
        self.handle:reset()
    end)
end

return TrailEffect
