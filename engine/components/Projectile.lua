-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Straight or arcing shot that damages whatever it was pointed at.
-- category: Interaction & Combat
--
-- The engine exposes no overlap query to Lua, so a hit is a distance test against the entity in
-- the Target slot: drag an enemy in for a homing shot, or let a spawner assign `projectile.target`
-- frame by frame. When it expires it hides itself, which is what makes it reusable by a Spawner.
local Projectile = {
    properties = {
        { name = "speed", displayName = "Speed (m/s)", type = "float", default = 24.0 },
        { name = "gravity", displayName = "Gravity pull (m/s^2, 0 = off)", type = "float", default = 0.0 },
        { name = "damage", displayName = "Damage", type = "float", default = 10.0 },
        { name = "lifetime", displayName = "Lifetime (seconds)", type = "float", default = 3.0 },
        { name = "hitRadius", displayName = "Hit radius", type = "float", default = 0.6 },
        { name = "target", displayName = "Target", type = "entity" },
        { name = "spawner", displayName = "Pool that owns this shot", type = "Spawner" },
        { name = "fallbackName", displayName = "Target name if slot is empty", type = "string", default = "" },
        { name = "homing", displayName = "Home on the target", type = "bool", default = false },
        { name = "faceVelocity", displayName = "Face the flight direction", type = "bool", default = true },
        { name = "groundY", displayName = "Explode at this height (very low = off)", type = "float", default = -1000.0 },
        { name = "onHitEntity", displayName = "On hit: entity", type = "entity" },
        { name = "launchAtStart", displayName = "Fire on scene start", type = "bool", default = false },
        { name = "onHitMethod", displayName = "On hit: method", type = "string", default = "onHit" }
    }
}

local function clamp(value, low, high)
    if value < low then return low end
    if value > high then return high end
    return value
end

-- A slot reaches Lua as a script table, as an engine handle, or as the bare id the
-- inspector stored, and 0 is NULL_ENTITY: normalise all three shapes to one.
local function entityId(reference)
    if type(reference) == "number" then
        if reference == 0 then return nil end
        return reference
    end
    if reference and type(reference.entity) == "number" then
        if reference.entity == 0 then return nil end
        return reference.entity
    end
    return nil
end

local function positionOf(scene, reference)
    local id = entityId(reference)
    if not id then
        return nil
    end
    local ok, position = pcall(function()
        return Object(scene, id).position
    end)
    if ok and position then
        return position, id
    end
    return nil
end

local function angle(degrees)
    if Engine.useDegrees then
        return degrees
    end
    return math.rad(degrees)
end

local function callMethod(reference, methodName, first, second)
    if type(reference) ~= "table" or not methodName or methodName == "" then
        return false
    end
    local method = reference[methodName]
    if type(method) ~= "function" then
        return false
    end
    method(reference, first, second)
    return true
end

function Projectile:init()
    RegisterEngineEvent(self, "onUpdate")

    self.speed = self.speed or 24.0
    self.hitRadius = self.hitRadius or 0.6
    self.lifetime = self.lifetime or 3.0

    local ok, object = pcall(function()
        return Object(self.scene, self.entity)
    end)
    self.object = (ok and object) or nil

    self.direction = Vector3(0.0, 0.0, 1.0)
    if self.object then
        self.direction = self.object.rotation:zAxis():normalize()
    end

    -- Pooled shots announce themselves so the spawner can hand them out again: only a script table
    -- carries register(), so a bare entity handle behind the slot has to be named as a miswire.
    if type(self.spawner) == "table" and type(self.spawner.register) == "function" then
        self.spawner:register(self)
    elseif type(self.spawner) == "userdata" then print("[Projectile] The Spawner slot holds an entity without a Spawner script.") end

    self:reset()

    -- A shot placed in the scene and switched to fire on start is its own bullet; a pooled one
    -- waits for a Spawner to launch it.
    if self.launchAtStart and self.object then
        self:launch(self.object.position, self.direction, nil)
    end
end

-- The Target slot wins; the name is the fallback so a shot works with nothing wired.
function Projectile:targetPosition()
    local point = positionOf(self.scene, self.target)
    if point then
        return point
    end
    if not self.fallbackName or self.fallbackName == "" then
        return nil
    end
    return positionOf(self.scene, entityId(self.scene:findEntity(self.fallbackName)))
end

-- Called by a Spawner (or any script) to put the shot back in play from a muzzle.
function Projectile:launch(position, direction, target)
    if self.object then
        self.object.visible = true
        self.object.position = position
    end
    self.velocity = (direction or self.direction):normalize() * self.speed
    self.alive = true
    self.age = 0.0
    if target ~= nil then
        self.target = target
    end
end

function Projectile:reset()
    self.alive = false
    self.age = 0.0
    self.velocity = Vector3(0.0, 0.0, 0.0)
    if self.object then
        self.object.visible = false
    end
end

function Projectile:onHit()
    -- A Health component is the usual receiver; anything with takeDamage works.
    local victim = self.target
    if type(victim) == "table" and type(victim.takeDamage) == "function" then
        victim:takeDamage(self.damage, self.entity)
    end

    callMethod(self.onHitEntity, self.onHitMethod, entityId(victim), self.object and self.object.position)
    self:reset()
end

function Projectile:onUpdate()
    if not self.object or not self.alive then
        return
    end

    local dt = math.min(Engine.deltatime, 0.05)
    self.age = self.age + dt

    if (self.gravity or 0.0) > 0.0 then
        self.velocity = Vector3(self.velocity.x, self.velocity.y - self.gravity * dt, self.velocity.z)
    end

    if self.homing ~= false then
        local point = self:targetPosition()
        if point then
            local position = self.object.position
            local wanted = Vector3(point.x - position.x, point.y - position.y, point.z - position.z)
            if wanted:squaredLength() > 0.0001 then
                wanted = wanted:normalize()
                local turn = clamp(6.0 * dt, 0.0, 1.0)
                self.velocity = Vector3(
                    self.velocity.x + (wanted.x * self.speed - self.velocity.x) * turn,
                    self.velocity.y + (wanted.y * self.speed - self.velocity.y) * turn,
                    self.velocity.z + (wanted.z * self.speed - self.velocity.z) * turn
                )
            end
        end
    end

    local position = self.object.position
    position = Vector3(position.x + self.velocity.x * dt,
        position.y + self.velocity.y * dt,
        position.z + self.velocity.z * dt)
    self.object.position = position

    if self.faceVelocity ~= false then
        local length = math.sqrt(self.velocity.x * self.velocity.x + self.velocity.z * self.velocity.z)
        if length > 0.0001 then
            self.object.rotation = Quaternion(
                angle(-math.deg(math.atan(self.velocity.y, length))),
                angle(math.deg(math.atan(self.velocity.x, self.velocity.z))),
                0.0)
        end
    end

    local targetPoint = self:targetPosition()
    if targetPoint and targetPoint:squaredDistance(position) <= self.hitRadius * self.hitRadius then
        self:onHit()
        return
    end

    if position.y <= (self.groundY or -1000.0) or self.age >= self.lifetime then
        self:reset()
    end
end

return Projectile
