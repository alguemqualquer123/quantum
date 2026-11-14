-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Animation state machine for a skinned character: idle, walk, run, aim, shoot and dead states
-- driven by the owner's horizontal speed plus an optional trigger set. An empty clip name simply
-- skips that state, so a model with a single clip still animates through the fallback.
-- category: Animation & Effects
local PlayerAnimator = {
    properties = {
        { name = "model", displayName = "Model entity (empty = this entity)", type = "entity" },
        { name = "modelName", displayName = "Model name if slot is empty", type = "string", default = "Ch46_nonPBR" },
        { name = "idleClip", displayName = "Idle clip", type = "string", default = "" },
        { name = "walkClip", displayName = "Walk clip", type = "string", default = "" },
        { name = "runClip", displayName = "Run clip", type = "string", default = "" },
        { name = "aimClip", displayName = "Aim clip (held)", type = "string", default = "" },
        { name = "shootClip", displayName = "Shoot clip", type = "string", default = "" },
        { name = "deathClip", displayName = "Death clip (empty = stop)", type = "string", default = "" },
        { name = "fallbackClip", displayName = "Fallback clip", type = "string", default = "Take 001" },
        { name = "walkSpeed", displayName = "Walk above this speed (m/s)", type = "float", default = 0.2 },
        { name = "runSpeed", displayName = "Run above this speed (m/s)", type = "float", default = 6.0 },
        { name = "fadeTime", displayName = "Crossfade time (s)", type = "float", default = 0.15 },
        { name = "shootTime", displayName = "Shoot hold time (s)", type = "float", default = 0.35 },
        { name = "keyAim", displayName = "Aim key (0 = right mouse)", type = "int", default = 0 },
        { name = "keyShoot", displayName = "Shoot key (0 = left mouse)", type = "int", default = 0 },
        { name = "keyDeath", displayName = "Death toggle key (0 = K)", type = "int", default = 0 }
    }
}

local function clamp(value, low, high)
    if value < low then return low end
    if value > high then return high end
    return value
end

-- 0 is GLFW's unknown key, so an untouched int property can fall back to the documented default.
local function defaultKey(value, fallback)
    if not value or value == 0 then
        return fallback
    end
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

-- Mouse codes stop at 7; anything above is a keyboard code, and isMousePressed rejects those.
local function pressed(code)
    if not code then
        return false
    end
    if code >= 0 and code <= 7 then
        return Input.isMousePressed(code)
    end
    return Input.isKeyPressed(code)
end

function PlayerAnimator:init()
    RegisterEngineEvent(self, "onUpdate")

    self.modelName = self.modelName or "Ch46_nonPBR"
    self.fallbackClip = self.fallbackClip or "Take 001"
    self.walkSpeed = self.walkSpeed or 0.2
    self.runSpeed = self.runSpeed or 6.0
    self.fadeTime = self.fadeTime or 0.15
    self.shootTime = self.shootTime or 0.35

    self.keyAim = defaultKey(self.keyAim, Input.MOUSE_BUTTON_RIGHT)
    self.keyShoot = defaultKey(self.keyShoot, Input.MOUSE_BUTTON_LEFT)
    self.keyDeath = defaultKey(self.keyDeath, Input.KEY_K)

    self.speed = 0.0
    self.shootTimer = 0.0
    self.aiming = false
    self.dead = false
    self.previousShoot = false
    self.previousDeath = false
    self.lastX = nil
    self.lastZ = nil
    self.modelId = nil
    self.modelHandle = nil
    self.clipCache = {}
    self.failedClips = {}
    self.currentClip = nil
    self.deathFrozen = false
    self.warningShown = false
end

function PlayerAnimator:resolveModelId()
    local id = entityId(self.model)
    if id then
        return id
    end
    if self.modelName and self.modelName ~= "" then
        local byName = self.scene:findEntity(self.modelName)
        if byName and byName ~= NULL_ENTITY then
            return byName
        end
    end
    return self.entity
end

-- The id is resolved again each frame because the editor keeps entity ids out of Lua, while
-- recreating the handle only on change keeps the per-clip lookup cache valid.
function PlayerAnimator:updateModel()
    local id = self:resolveModelId()
    if id == self.modelId then
        return
    end

    self.modelId = id
    self.modelHandle = nil
    self.clipCache = {}
    self.failedClips = {}
    self.currentClip = nil
    self.deathFrozen = false

    if not id then
        return
    end

    local ok, handle = pcall(function()
        return Model(self.scene, id)
    end)
    if ok and handle then
        self.modelHandle = handle
    end
end

function PlayerAnimator:warn(message)
    if self.warningShown then
        return
    end
    self.warningShown = true
    print("[PlayerAnimator] " .. message)
end

function PlayerAnimator:updateMovementSpeed(dt)
    local ok, position = pcall(function()
        return Object(self.scene, self.entity).position
    end)
    if not ok or not position then
        return
    end

    if self.lastX and dt > 0.0001 then
        local dx = position.x - self.lastX
        local dz = position.z - self.lastZ
        local instant = math.sqrt(dx * dx + dz * dz) / dt
        local rate = clamp(dt * 10.0, 0.0, 1.0)
        self.speed = self.speed + (instant - self.speed) * rate
    end

    self.lastX = position.x
    self.lastZ = position.z
end

function PlayerAnimator:updateInput(dt)
    self.aiming = pressed(self.keyAim)

    local shootDown = pressed(self.keyShoot)
    if shootDown and not self.previousShoot then
        self.shootTimer = self.shootTime
    end
    self.previousShoot = shootDown

    local deathDown = pressed(self.keyDeath)
    if deathDown and not self.previousDeath then
        self.dead = not self.dead
        self.currentClip = nil
        self.deathFrozen = false
    end
    self.previousDeath = deathDown

    if self.shootTimer > 0.0 then
        self.shootTimer = math.max(self.shootTimer - dt, 0.0)
    end
end

-- findAnimation has no "count" or "exists" query, so a miss is cached as a failure instead of
-- being retried and logged on every frame.
function PlayerAnimator:findClip(name)
    if not self.modelHandle or not name or name == "" then
        return nil
    end
    if self.failedClips[name] then
        return nil
    end

    local cached = self.clipCache[name]
    if cached then
        return cached
    end

    local ok, found = pcall(function()
        return self.modelHandle:findAnimation(name)
    end)
    if ok and found and found ~= 0 then
        self.clipCache[name] = found
        return found
    end

    self.failedClips[name] = true
    return nil
end

function PlayerAnimator:playClip(name)
    if not name or name == "" then
        return false
    end
    if self.currentClip == name then
        return true
    end

    local clip = self:findClip(name)
    if not clip then
        return false
    end

    -- Older engine builds only expose the one-argument overload.
    local ok = pcall(function()
        self.modelHandle:playAnimation(clip, self.fadeTime)
    end)
    if not ok then
        ok = pcall(function()
            self.modelHandle:playAnimation(clip)
        end)
    end
    if not ok then
        self.failedClips[name] = true
        return false
    end

    self.currentClip = name
    return true
end

function PlayerAnimator:playFallback()
    if self.fallbackClip and self.fallbackClip ~= "" then
        return self:playClip(self.fallbackClip)
    end
    return false
end

function PlayerAnimator:desiredClip()
    if self.shootTimer > 0.0 and self:findClip(self.shootClip) then
        return self.shootClip
    end
    if self.aiming and self:findClip(self.aimClip) then
        return self.aimClip
    end
    if self.speed >= self.runSpeed and self:findClip(self.runClip) then
        return self.runClip
    end
    if self.speed > self.walkSpeed and self:findClip(self.walkClip) then
        return self.walkClip
    end
    return self.idleClip
end

function PlayerAnimator:updateClip()
    if not self.modelHandle then
        return
    end

    if self.dead then
        if self.deathClip and self.deathClip ~= "" and self:playClip(self.deathClip) then
            return
        end
        -- No pause API, so a death pose is held by stopping the mixer once.
        if not self.deathFrozen then
            self.deathFrozen = true
            self.currentClip = nil
            pcall(function()
                self.modelHandle:stopAnimations(self.fadeTime)
            end)
        end
        return
    end

    local clip = self:desiredClip()
    if clip and clip ~= "" and self:playClip(clip) then
        return
    end
    if not self:playFallback() then
        self:warn("No animation clip was found on '" .. tostring(self.modelName) .. "'.")
    end
end

function PlayerAnimator:onUpdate()
    local dt = math.min(Engine.deltatime, 0.05)
    self:updateModel()
    self:updateMovementSpeed(dt)
    self:updateInput(dt)
    self:updateClip()
end

return PlayerAnimator
