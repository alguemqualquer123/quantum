-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Self-destructs after a while: arrows, sparks, muzzle flashes, effects made of plain entities.
-- category: Interaction & Combat
-- Attach it to the entity that was born and must die. When the entity also holds a Projectile that
-- belongs to a Spawner pool, it hands itself back to the pool instead of being destroyed, so a shot
-- that never hits anything still frees its slot.
local Lifetime = {
    properties = {
        { name = "seconds", displayName = "Lifetime (seconds)", type = "float", default = 3.0 },
        { name = "spawner", displayName = "Pool to return to", type = "Spawner" },
        { name = "onExpire", displayName = "On expire: entity", type = "entity" },
        { name = "onExpireMethod", displayName = "On expire: method", type = "string", default = "onExpire" },
        { name = "hide", displayName = "Hide instead of destroy", type = "bool", default = false },
        { name = "pauseWhenHidden", displayName = "Count only while visible", type = "bool", default = true }
    }
}

local function callMethod(reference, methodName, first)
    if type(reference) ~= "table" or not methodName or methodName == "" then
        return false
    end
    local method = reference[methodName]
    if type(method) ~= "function" then
        return false
    end
    method(reference, first)
    return true
end

function Lifetime:init()
    RegisterEngineEvent(self, "onUpdate")

    self.seconds = math.max(self.seconds or 3.0, 0.01)
    self.elapsed = 0.0
    self.expired = false
    self.counting = true

    local ok, object = pcall(function()
        return Object(self.scene, self.entity)
    end)
    self.object = (ok and object) or nil
end

function Lifetime:reset()
    self.elapsed = 0.0
    self.expired = false
    self.counting = true
end

function Lifetime:expire()
    if self.expired then
        return
    end
    self.expired = true
    self.counting = false

    callMethod(self.onExpire, self.onExpireMethod, self.entity)

    -- A pool owns this entity, so it must survive: ask the pool to mark the slot idle. Only a
    -- script table carries recycle(), so a bare entity handle is named instead of silently lost.
    if type(self.spawner) == "table" and type(self.spawner.recycle) == "function" then
        if self.spawner:recycle(self) then
            return
        end
    elseif type(self.spawner) == "userdata" and Engine.systemTime - (self.lastWarn or -1.0) > 1.0 then
        self.lastWarn = Engine.systemTime
        print("[Lifetime] The Spawner slot holds an entity without a Spawner script: this is destroyed, not recycled")
    end

    if self.hide == true then
        if self.object then
            self.object.visible = false
        end
    else
        self.scene:destroyEntity(self.entity)
    end
end

function Lifetime:onUpdate()
    if self.expired then
        return
    end

    if self.pauseWhenHidden ~= false and self.object then
        local visible = self.object.visible
        if not visible then
            self.counting = false
            return
        end
        if not self.counting then
            -- It just came out of a pool: restart the clock instead of finishing what the
            -- previous shot already used.
            self.elapsed = 0.0
            self.counting = true
        end
    end

    local dt = math.min(Engine.deltatime, 0.05)
    self.elapsed = self.elapsed + dt
    if self.elapsed >= self.seconds then
        self:expire()
    end
end

return Lifetime
