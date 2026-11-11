-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Turn to face another entity: turrets, enemies that notice you, a mirror that watches the player.
-- category: Scene & HUD
-- Attach it to the entity that should rotate and drag what it looks at into the Target slot.
local LookAt = {
    properties = {
        { name = "target", displayName = "Target", type = "entity" },
        { name = "fallbackName", displayName = "Target name if slot is empty", type = "string", default = "Player" },
        { name = "turnSpeed", displayName = "Turn speed (degrees/s, 0 = instant)", type = "float", default = 180.0 },
        { name = "onlyYaw", displayName = "Stay upright (yaw only)", type = "bool", default = true },
        { name = "flip", displayName = "Backwards (180 degrees)", type = "bool", default = false },
        { name = "lookOffset", displayName = "Look height offset", type = "float", default = 1.0 }
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
        return position
    end
    return nil
end

local function angle(degrees)
    if Engine.useDegrees then
        return degrees
    end
    return math.rad(degrees)
end

local function shortestArc(from, to)
    local delta = (to - from) % 360.0
    if delta > 180.0 then
        delta = delta - 360.0
    end
    return delta
end

function LookAt:init()
    RegisterEngineEvent(self, "onUpdate")

    self.turnSpeed = self.turnSpeed or 180.0
    self.currentYaw = 0.0
    self.currentPitch = 0.0
    self.started = false
    self.warningShown = false
end

function LookAt:onUpdate()
    local ok, object = pcall(function()
        return Object(self.scene, self.entity)
    end)
    if not ok or not object then
        if not self.warningShown then
            self.warningShown = true
            print("[LookAt] The entity needs a Transform component.")
        end
        return
    end

    local id = entityId(self.target)
    if not id and self.fallbackName and self.fallbackName ~= "" then
        id = entityId(self.scene:findEntity(self.fallbackName))
    end

    local from = object.position
    local to = positionOf(self.scene, id)
    if not to or (to.x == from.x and to.z == from.z and to.y == from.y) then
        return
    end
    to.y = to.y + (self.lookOffset or 0.0)

    local dx, dy, dz = to.x - from.x, to.y - from.y, to.z - from.z
    local horizontal = math.sqrt(dx * dx + dz * dz)

    local wantedYaw = math.deg(math.atan(dx, dz)) + (self.flip and 180.0 or 0.0)
    local wantedPitch = horizontal > 0.0001 and math.deg(math.atan(dy, horizontal)) or 0.0

    if not self.started then
        self.currentYaw, self.currentPitch = wantedYaw, wantedPitch
        self.started = true
    end

    local dt = math.min(Engine.deltatime, 0.05)
    if self.turnSpeed and self.turnSpeed > 0.0 then
        local step = self.turnSpeed * dt
        self.currentYaw = self.currentYaw + clamp(shortestArc(self.currentYaw, wantedYaw), -step, step)
        self.currentPitch = self.currentPitch + clamp(shortestArc(self.currentPitch, wantedPitch), -step, step)
    else
        self.currentYaw, self.currentPitch = wantedYaw, wantedPitch
    end

    if self.onlyYaw ~= false then
        object.rotation = Quaternion(0.0, angle(self.currentYaw), 0.0)
    else
        object.rotation = Quaternion(angle(-self.currentPitch), angle(self.currentYaw), 0.0)
    end
end

return LookAt
