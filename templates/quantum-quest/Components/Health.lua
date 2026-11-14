-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Health for anything that can be hit. Attach it to the entity, then wire the death and value
-- category: Interaction & Combat
-- changes at your own pace: every hook is optional and other components call takeDamage by
-- holding this entity in a slot declared with type = "Health".
local Health = {
    properties = {
        { name = "maxHealth", displayName = "Maximum health", type = "float", default = 100.0 },
        { name = "startingHealth", displayName = "Starting health (0 = full)", type = "float", default = 0.0 },
        { name = "invulnerableTime", displayName = "Invulnerable after a hit (seconds)", type = "float", default = 0.2 },
        { name = "regenRate", displayName = "Regeneration (per second, 0 = off)", type = "float", default = 0.0 },
        { name = "regenDelay", displayName = "Regeneration delay after a hit (seconds)", type = "float", default = 4.0 },
        { name = "deathAction", displayName = "On death (0 = nothing, 1 = hide, 2 = destroy)", type = "int", default = 1 },
        { name = "onDeath", displayName = "On death: entity", type = "entity" },
        { name = "onDeathMethod", displayName = "On death: method", type = "string", default = "onDeath" },
        { name = "onChange", displayName = "On change: entity", type = "entity" },
        { name = "onChangeMethod", displayName = "On change: method", type = "string", default = "onValueChange" }
    }
}

local function clamp(value, low, high)
    if value < low then return low end
    if value > high then return high end
    return value
end

-- A hook slot reaches Lua as the target entity's script table only when that entity really carries
-- an enabled Lua script; otherwise it is a bare entity handle with no such member, and calling
-- through a table would raise, so this returns false quietly.
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

function Health:init()
    RegisterEngineEvent(self, "onUpdate")

    self.maxHealth = math.max(self.maxHealth or 100.0, 1.0)
    self.invulnerableTime = self.invulnerableTime or 0.2
    self.regenDelay = self.regenDelay or 4.0

    self.current = (self.startingHealth and self.startingHealth > 0.0)
        and clamp(self.startingHealth, 0.0, self.maxHealth) or self.maxHealth
    self.percent = 1.0
    self.alive = true
    self.sinceHit = self.regenDelay
    self.sinceInvulnerable = 0.0

    self.hud = nil
    self.bar = nil

    local ok, object = pcall(function()
        return Object(self.scene, self.entity)
    end)
    self.object = (ok and object) or nil
end

function Health:getPercent()
    return self.percent
end

function Health:isAlive()
    return self.alive
end

function Health:takeDamage(amount, source)
    if not self.alive or (amount or 0.0) <= 0.0 then
        return false
    end
    if self.sinceInvulnerable < self.invulnerableTime then
        return false
    end

    self.sinceInvulnerable = 0.0
    self.sinceHit = 0.0
    self:setHealth(self.current - amount, source)
    return true
end

function Health:heal(amount, source)
    if not self.alive then
        return false
    end
    self:setHealth(self.current + math.max(amount or 0.0, 0.0), source)
    return true
end

function Health:kill(source)
    self:setHealth(0.0, source)
end

function Health:setHealth(value, source)
    local previous = self.current
    self.current = clamp(value, 0.0, self.maxHealth)
    self.percent = self.current / self.maxHealth

    if self.current ~= previous then
        callMethod(self.onChange, self.onChangeMethod, self.percent, self.current)
    end

    if self.current <= 0.0 and self.alive then
        self.alive = false
        self:onDie(source)
    end
end

function Health:onDie(source)
    callMethod(self.onDeath, self.onDeathMethod, self.entity, source)

    if self.deathAction == 2 then
        self.scene:destroyEntity(self.entity)
    elseif self.deathAction == nil or self.deathAction == 1 then
        if self.object then
            self.object.visible = false
        end
    end
end

function Health:onUpdate()
    if not self.alive then
        return
    end

    local dt = math.min(Engine.deltatime, 0.05)
    self.sinceHit = self.sinceHit + dt
    self.sinceInvulnerable = self.sinceInvulnerable + dt

    local rate = self.regenRate or 0.0
    if rate > 0.0 and self.current < self.maxHealth and self.sinceHit >= self.regenDelay then
        self:setHealth(self.current + rate * dt, nil)
    end
end

return Health
