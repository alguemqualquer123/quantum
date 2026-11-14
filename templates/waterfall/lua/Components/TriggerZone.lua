-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Zone that reports when an entity gets close or leaves: checkpoints, doors, dialogue triggers,
-- category: Interaction & Combat
-- pickup radius. Attach it to the zone entity and drag who it watches into the Subject slot.
local TriggerZone = {
    properties = {
        { name = "subject", displayName = "Subject", type = "entity" },
        { name = "fallbackName", displayName = "Subject name if slot is empty", type = "string", default = "Player" },
        { name = "radius", displayName = "Radius", type = "float", default = 3.0 },
        { name = "height", displayName = "Height limit (0 = ignore height)", type = "float", default = 0.0 },
        { name = "inside", displayName = "Start already inside", type = "bool", default = false },
        { name = "onlyOnce", displayName = "Fire once only", type = "bool", default = false },
        { name = "onEnter", displayName = "On enter: entity", type = "entity" },
        { name = "onEnterMethod", displayName = "On enter: method", type = "string", default = "onEnter" },
        { name = "onExit", displayName = "On exit: entity", type = "entity" },
        { name = "onExitMethod", displayName = "On exit: method", type = "string", default = "onExit" },
        { name = "onStay", displayName = "Every frame inside: entity", type = "entity" },
        { name = "onStayMethod", displayName = "Every frame inside: method", type = "string", default = "" }
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

local function callMethod(reference, methodName, entity)
    if type(reference) ~= "table" or not methodName or methodName == "" then
        return false
    end
    local method = reference[methodName]
    if type(method) ~= "function" then
        return false
    end
    method(reference, entity)
    return true
end

function TriggerZone:init()
    RegisterEngineEvent(self, "onUpdate")

    self.radius = self.radius or 3.0
    self.insideZone = self.inside == true
    self.fired = false

    local ok, object = pcall(function()
        return Object(self.scene, self.entity)
    end)
    self.object = (ok and object) or nil
    self.warningShown = false
end

function TriggerZone:isInside()
    if not self.object then
        return false
    end

    local id = entityId(self.subject)
    if not id and self.fallbackName and self.fallbackName ~= "" then
        id = entityId(self.scene:findEntity(self.fallbackName))
    end

    local point = positionOf(self.scene, id)
    if not point then
        return false
    end

    local centre = self.object.position
    local dx, dy, dz = point.x - centre.x, point.y - centre.y, point.z - centre.z
    local horizontal = math.sqrt(dx * dx + dz * dz)
    if horizontal > self.radius then
        return false
    end
    if (self.height or 0.0) > 0.0 and math.abs(dy) > self.height then
        return false
    end
    return true
end

function TriggerZone:onUpdate()
    if self.onlyOnce and self.fired then
        return
    end

    local nowInside = self:isInside()

    if nowInside and not self.insideZone then
        self.insideZone = true
        self.fired = true
        callMethod(self.onEnter, self.onEnterMethod, self.entity)
    elseif not nowInside and self.insideZone then
        self.insideZone = false
        callMethod(self.onExit, self.onExitMethod, self.entity)
    end

    if nowInside then
        callMethod(self.onStay, self.onStayMethod, self.entity)
    end
end

return TriggerZone
