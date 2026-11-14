-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- A crate or rock that sleeps until something comes near, falls under real gravity and hurts on the
-- category: Physics
-- landing. Attach it to the falling entity (or point Subject at it) and wire the effect slots.
local FallingObject = {
    properties = {
        { name = "subject", displayName = "Falling body (0 = this entity)", type = "entity" },
        { name = "wakeDistance", displayName = "Wake distance (m)", type = "float", default = 4.0 },
        { name = "wakeAfter", displayName = "Wake after (seconds, 0 = immediately)", type = "float", default = 0.0 },
        { name = "gravityFactor", displayName = "Gravity factor", type = "float", default = 1.0 },
        { name = "initialVelocity", displayName = "Launch velocity (m/s)", type = "vector3", default = { 0.0, 0.0, 0.0 } },
        { name = "antiTunnel", displayName = "Continuous collision (anti tunnel)", type = "bool", default = true },
        { name = "startAsleep", displayName = "Start asleep", type = "bool", default = false },
        { name = "impactDelta", displayName = "Speed drop that counts as impact (m/s)", type = "float", default = 6.0 },
        { name = "damagePerSpeed", displayName = "Damage per m/s", type = "float", default = 6.0 },
        { name = "minDamage", displayName = "Minimum damage", type = "float", default = 0.0 },
        { name = "maxDamage", displayName = "Maximum damage", type = "float", default = 100.0 },
        { name = "target", displayName = "Health to damage", type = "Health" },
        { name = "suspects", displayName = "Victim inside the radius (0 = none)", type = "entity" },
        { name = "impactRadius", displayName = "Impact radius for that victim (m)", type = "float", default = 2.0 },
        { name = "shake", displayName = "CameraShake", type = "CameraShake" },
        { name = "impact", displayName = "ImpactEffect", type = "ImpactEffect" },
        { name = "sound", displayName = "Impact sound", type = "entity" },
        { name = "respawnTime", displayName = "Fall again after (seconds, 0 = off)", type = "float", default = 0.0 },
        { name = "freezeOnLand", displayName = "Freeze on landing", type = "bool", default = false }
    }
}

local function num(value, fallback) return type(value) == "number" and value or fallback end
local function clamp(value, low, high) return math.min(math.max(value, low), high) end

local function entityId(reference)
    local id = (type(reference) == "number") and reference
        or ((reference and type(reference.entity) == "number") and reference.entity or 0)
    return id ~= 0 and id or nil
end

-- The engine delivers a vector row as Vector3 userdata and the inspector default as a table; the
-- pcall keeps an entity handle from raising when it has no such channel.
local function asVec3(value, fallback)
    local ok, out = pcall(function()
        if type(value) ~= "table" and type(value) ~= "userdata" then return fallback end
        if type(value.x) == "number" then return Vector3(value.x, num(value.y, 0.0), num(value.z, 0.0)) end
        return type(value[1]) == "number" and Vector3(value[1], num(value[2], 0.0), num(value[3], 0.0)) or fallback
    end)
    return ok and out or fallback
end

local function positionOf(scene, reference)
    local id = entityId(reference)
    if not id then return nil end
    local ok, position = pcall(function() return Object(scene, id).position end)
    return ok and position or nil
end

function FallingObject:init()
    RegisterEngineEvent(self, "onFixedUpdate")
    self.wakeDistance = math.max(0.0, num(self.wakeDistance, 4.0))
    self.wakeAfter = math.max(0.0, num(self.wakeAfter, 0.0))
    self.impactRadius = math.max(0.1, num(self.impactRadius, 2.0))
    self.respawnTime = math.max(0.0, num(self.respawnTime, 0.0))
    self.impactDelta = math.max(0.1, num(self.impactDelta, 6.0))
    self.launch = asVec3(self.initialVelocity, Vector3(0.0, 0.0, 0.0))
    self.prevSpeed, self.falling, self.done, self.lastWarn = 0.0, false, false, -1.0
    self.startTime, self.impactAt = num(Engine.systemTime, 0.0), -1.0
    self.bodyId = entityId(self.subject) or self.entity
    local ok, body = pcall(function() return Body3D(self.scene, self.bodyId) end)
    self.body = (ok and body) or nil
    if not self.body then
        self:warn("the entity needs a Body3D component with shapes")
        return
    end
    self.home = positionOf(self.scene, self.bodyId)
    pcall(function()
        self.body.gravityFactor = num(self.gravityFactor, 1.0)
        if self.antiTunnel ~= false then self.body.motionQuality = Body3DMotionQuality.LINEAR_CAST end
        self.body.allowSleeping = self.startAsleep == true
        if self.startAsleep then self.body:deactivate() else self.body:activate() end
    end)
end

function FallingObject:warn(message)
    local now = num(Engine.systemTime, 0.0)
    if now - num(self.lastWarn, -1.0) < 1.0 then return end
    self.lastWarn = now
    print("[FallingObject] " .. message)
end

-- Lua has no overlap query and a velocity collapse never says what was hit, so the victim must be
-- wired: the Health slot first, else the one entity in Suspects if it sits inside the radius.
function FallingObject:victims(point, damage)
    if type(self.target) == "table" and type(self.target.takeDamage) == "function" then
        pcall(function() self.target:takeDamage(damage, self.entity) end)
        return true
    end
    local suspect = positionOf(self.scene, self.suspects)
    if suspect and point and suspect:squaredDistance(point) <= self.impactRadius * self.impactRadius
        and type(self.suspects) == "table" and type(self.suspects.takeDamage) == "function" then
        pcall(function() self.suspects:takeDamage(damage, self.entity) end)
        return true
    end
    if type(self.target) == "userdata" or type(self.suspects) == "userdata" then
        self:warn("wire this: the entity in the Health slot needs a Health component")
    end
    return false
end

function FallingObject:fireImpact(speed, point)
    self.done = true
    local damage = clamp(speed * num(self.damagePerSpeed, 6.0), num(self.minDamage, 0.0),
        num(self.maxDamage, 100.0))
    self:victims(point, damage)
    if type(self.shake) == "table" and type(self.shake.addTrauma) == "function" then
        pcall(function() self.shake:addTrauma(clamp(speed / 20.0, 0.05, 1.0)) end)
    end
    -- ImpactEffect may not be installed in this project, so it is only ever reached through pcall.
    if type(self.impact) == "table" and type(self.impact.play) == "function" then
        pcall(function() self.impact:play(damage / math.max(num(self.maxDamage, 100.0), 1.0), point) end)
    end
    local soundId = entityId(self.sound)
    if soundId then pcall(function() Sound(self.scene, soundId):play() end) end
    if self.freezeOnLand then
        local zero = Vector3(0.0, 0.0, 0.0)
        -- A landed body is deactivated, not retyped: Body3D:setType reaches Jolt's SetMotionType
        -- with Activate, which asserts on a body that is already in the broadphase.
        pcall(function()
            self.body.linearVelocity, self.body.angularVelocity = zero, zero
            self.body:deactivate()
        end)
    end
end

function FallingObject:restart()
    self.done, self.falling, self.prevSpeed, self.impactAt = false, false, 0.0, -1.0
    self.startTime = num(Engine.systemTime, 0.0)
    if not self.body then return end
    local home, asleep = self.home, self.startAsleep
    pcall(function()
        if home then Object(self.scene, self.bodyId).position = home end
        self.body.linearVelocity = Vector3(0.0, 0.0, 0.0)
        if asleep then self.body:deactivate() else self.body:activate() end
    end)
end

function FallingObject:onFixedUpdate()
    if not self.body then return end
    local now = num(Engine.systemTime, 0.0)
    if self.done then
        if self.respawnTime > 0.0 and now - self.impactAt >= self.respawnTime then self:restart() end
        return
    end
    if not self.falling then
        if self.wakeAfter > 0.0 and now - self.startTime < self.wakeAfter then return end
        if self.startAsleep then
            local own = positionOf(self.scene, self.bodyId)
            local watcher = positionOf(self.scene, self.suspects) or positionOf(self.scene, self.target)
            if not watcher or not own or watcher:distance(own) > self.wakeDistance then return end
            pcall(function() self.body:activate() end)
        end
        self.falling = true
        self.prevSpeed = self.launch:length()
        pcall(function() self.body.linearVelocity = self.launch end)
        return
    end

    -- Jolt writes the post-collision velocity before the script runs, so a landing is the frame the
    -- speed collapses in; there is no contact callback that reaches a Lua component.
    local speed = self.body.linearVelocity:length()
    if self.prevSpeed - speed > self.impactDelta and self.prevSpeed > self.impactDelta then
        local ok, point = pcall(function() return self.body:getCenterOfMassPosition() end)
        self.impactAt = now
        self:fireImpact(self.prevSpeed, (ok and point) or positionOf(self.scene, self.bodyId))
        return
    end
    self.prevSpeed = speed
end

return FallingObject
