-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Pool spawner for shots, effects or anything reusable. Lua cannot instantiate an entity, so the
-- category: Interaction & Combat
-- pool is authored: put the pooled entities under this one (duplicate the projectile as many times
-- as the pool needs), give each of them the Projectile script and drop this entity into their
-- Spawner slot. They register themselves at start and this script launches the idle ones.
local Spawner = {
    properties = {
        { name = "interval", displayName = "Spawn interval (seconds)", type = "float", default = 0.8 },
        { name = "batch", displayName = "Spawned per interval", type = "int", default = 1 },
        { name = "maxActive", displayName = "Maximum active (0 = all)", type = "int", default = 0 },
        { name = "autoSpawn", displayName = "Spawn by itself", type = "bool", default = false },
        { name = "loop", displayName = "Keep spawning", type = "bool", default = true },
        { name = "spawnKey", displayName = "Spawn key (0 = none)", type = "int", default = 0 },
        { name = "spawnPoint", displayName = "Spawn point entity", type = "entity" },
        { name = "target", displayName = "Assigned target", type = "entity" },
        { name = "coneAngle", displayName = "Spread inside the pool (degrees)", type = "float", default = 0.0 },
        { name = "usePointRotation", displayName = "Follow the spawn point facing", type = "bool", default = true }
    }
}

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

local function frameOf(scene, reference, fallbackEntity)
    local id = entityId(reference) or fallbackEntity
    local ok, object = pcall(function()
        return Object(scene, id)
    end)
    if not ok or not object then
        return nil
    end
    return object
end

function Spawner:init()
    RegisterEngineEvent(self, "onUpdate")

    self.interval = math.max(self.interval or 0.8, 0.01)
    self.batch = math.max(self.batch or 1, 1)

    self.members = {}
    self.elapsed = 0.0
    self.previousKey = false
    self.round = 0
    self.warningShown = false
end

-- Called by a pooled script that holds this entity in a Spawner slot. An entity carries one
-- instance table per script, so the pool is keyed by entity: two scripts on the same entity must
-- not give the pool two slots.
function Spawner:register(member)
    self.members = self.members or {}
    local id = entityId(member)
    for _, existing in ipairs(self.members) do
        if entityId(existing) == id then
            return
        end
    end
    table.insert(self.members, member)
end

-- Called by a pooled script that is finished (a shot that expired, an effect that ran out).
function Spawner:recycle(member)
    local id = entityId(member)
    if not id then
        return false
    end
    for _, existing in ipairs(self.members or {}) do
        if entityId(existing) == id then
            existing.alive = false
            if type(existing.reset) == "function" then
                existing:reset()
            end
            return true
        end
    end
    return false
end

function Spawner:idleCount()
    local idle = 0
    for _, member in ipairs(self.members) do
        if not member.alive then
            idle = idle + 1
        end
    end
    return idle
end

function Spawner:activeCount()
    local active = 0
    for _, member in ipairs(self.members) do
        if member.alive then
            active = active + 1
        end
    end
    return active
end

-- Called by any script holding this entity in a Spawner slot.
function Spawner:spawn(count)
    local object = frameOf(self.scene, self.spawnPoint, self.entity)
    if not object then
        if not self.warningShown then
            self.warningShown = true
            print("[Spawner] The spawn point needs a Transform component.")
        end
        return 0
    end

    local position = object.position
    local forward = object.rotation:zAxis():normalize()
    if self.usePointRotation == false then
        forward = Vector3(0.0, 0.0, 1.0)
    end

    local wanted = math.max(count or 1, 1)
    if (self.maxActive or 0) > 0 then
        wanted = math.min(wanted, self.maxActive - self:activeCount())
    end

    local spawned = 0
    for _, member in ipairs(self.members) do
        if spawned >= wanted then
            break
        end
        if not member.alive and type(member.launch) == "function" then
            local direction = forward
            local spread = self.coneAngle or 0.0
            if spread > 0.0 then
                -- Fan the pool around the spawn point's up axis so a spray reads as one volley.
                local t = (spawned + 0.5) / wanted - 0.5
                local yaw = math.rad(t * spread)
                local cos, sin = math.cos(yaw), math.sin(yaw)
                direction = Vector3(forward.x * cos - forward.z * sin, forward.y,
                    forward.x * sin + forward.z * cos)
            end
            member:launch(position, direction, self.target)
            spawned = spawned + 1
        end
    end

    self.round = self.round + 1
    return spawned
end

function Spawner:onUpdate()
    local dt = math.min(Engine.deltatime, 0.05)

    if self.spawnKey and self.spawnKey > 0 then
        local pressed = Input.isKeyPressed(self.spawnKey)
        if pressed and not self.previousKey then
            self:spawn(self.batch)
        end
        self.previousKey = pressed
    end

    if self.autoSpawn == false then
        return
    end

    self.elapsed = self.elapsed + dt
    if self.elapsed < self.interval then
        return
    end
    self.elapsed = 0.0

    local spawned = self:spawn(self.batch)
    if spawned == 0 and self.loop == false then
        self.autoSpawn = false
    end
end

return Spawner
