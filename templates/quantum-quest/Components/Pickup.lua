-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Health pack, coin, ammo box: a radius check that hands something to whoever walks into it.
-- category: Interaction & Combat
-- Attach it to the pickup and drag the subject (usually the player) into the Subject slot. With a
-- Respawn time above zero it reappears on its own, so one placed box can be collected forever.
local Pickup = {
    properties = {
        { name = "subject", displayName = "Subject", type = "entity" },
        { name = "fallbackName", displayName = "Subject name if slot is empty", type = "string", default = "Player" },
        { name = "radius", displayName = "Pickup radius", type = "float", default = 1.6 },
        { name = "grantHealth", displayName = "Health to grant (component)", type = "Health" },
        { name = "healAmount", displayName = "Health amount", type = "float", default = 25.0 },
        { name = "value", displayName = "Score value", type = "int", default = 1 },
        { name = "respawnTime", displayName = "Respawn after (seconds, 0 = never)", type = "float", default = 0.0 },
        { name = "startTaken", displayName = "Start already collected", type = "bool", default = false },
        { name = "onPickup", displayName = "On pickup: entity", type = "entity" },
        { name = "onPickupMethod", displayName = "On pickup: method", type = "string", default = "onPickup" }
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

local function positionOf(scene, id)
    if not id then
        return nil
    end
    local ok, position = pcall(function()
        return Object(scene, id).position
    end)
    if ok and position then
        return position
    end
    return nil
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

function Pickup:warn(message)
    local now = Engine.systemTime or 0.0
    if now - (self.lastWarn or -1.0) < 1.0 then
        return
    end
    self.lastWarn = now
    print("[Pickup] " .. message)
end

function Pickup:init()
    RegisterEngineEvent(self, "onUpdate")

    self.radius = math.max(self.radius or 1.6, 0.05)
    self.total = 0
    self.waiting = 0.0
    self.warningShown, self.lastWarn = false, -1.0

    local ok, object = pcall(function()
        return Object(self.scene, self.entity)
    end)
    self.object = (ok and object) or nil

    self.taken = self.startTaken == true
    if self.taken then
        self:setVisible(false)
        self.waiting = math.max(self.respawnTime or 0.0, 0.0)
    end
end

function Pickup:setVisible(value)
    if self.object then
        self.object.visible = value
    end
end

function Pickup:collect()
    if self.taken then
        return false
    end

    local id = entityId(self.subject)
    if not id and self.fallbackName and self.fallbackName ~= "" then
        id = entityId(self.scene:findEntity(self.fallbackName))
    end

    local health = self.grantHealth
    if type(health) == "table" and type(health.heal) == "function" then
        health:heal(self.healAmount or 0.0, self.entity)
    elseif type(self.subject) == "table" and type(self.subject.heal) == "function" then
        self.subject:heal(self.healAmount or 0.0, self.entity)
    elseif type(health) == "userdata" or type(self.subject) == "userdata" then
        -- A slot only carries the method when the wired entity really has the script behind it.
        self:warn("wire this: a Health component on the entity in the Health or Subject slot")
    end

    self.total = self.total + 1
    self.taken = true
    self.waiting = math.max(self.respawnTime or 0.0, 0.0)
    self:setVisible(false)

    callMethod(self.onPickup, self.onPickupMethod, id, self.value or 0)
    return true
end

-- Any script can hand a pickup back early (a shop, a respawn room).
function Pickup:respawn()
    self.taken = false
    self.waiting = 0.0
    self:setVisible(true)
end

function Pickup:onUpdate()
    if not self.object then
        return
    end

    if self.taken then
        if self.waiting <= 0.0 then
            return
        end
        self.waiting = self.waiting - math.min(Engine.deltatime, 0.05)
        if self.waiting <= 0.0 then
            self:respawn()
        end
        return
    end

    local id = entityId(self.subject)
    if not id and self.fallbackName and self.fallbackName ~= "" then
        id = entityId(self.scene:findEntity(self.fallbackName))
    end

    local point = positionOf(self.scene, id)
    if not point then
        if not self.warningShown then
            self.warningShown = true
            print("[Pickup] No subject: drag the player into the Subject slot or match its name.")
        end
        return
    end

    local here = self.object.position
    local dx, dy, dz = point.x - here.x, point.y - here.y, point.z - here.z
    if dx * dx + dy * dy + dz * dz <= self.radius * self.radius then
        self:collect()
    end
end

return Pickup
