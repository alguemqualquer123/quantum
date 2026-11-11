-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- A body swinging from a real Jolt joint: a pivot for a rope, a distance joint for a rigid rod.
-- category: Physics
-- Attach it to the entity that carries a Joint3D component and drop the anchor and the body in.
local Rope = {
    properties = {
        { name = "anchor", displayName = "Anchor entity", type = "entity" },
        { name = "fallbackAnchorName", displayName = "Anchor name if slot is empty", type = "string", default = "Anchor" },
        { name = "subject", displayName = "Body to swing (0 = this entity)", type = "entity" },
        { name = "length", displayName = "Rope length (m)", type = "float", default = 4.0 },
        { name = "minLength", displayName = "Minimum length (m)", type = "float", default = 0.5 },
        { name = "maxLength", displayName = "Maximum length (m)", type = "float", default = 30.0 },
        { name = "anchorOffset", displayName = "Anchor offset (m)", type = "vector3", default = { 0.0, 0.0, 0.0 } },
        { name = "ropeLike", displayName = "Rope instead of rigid rod", type = "bool", default = true },
        { name = "damping", displayName = "Damping per step (0..1)", type = "float", default = 0.05 },
        { name = "bounce", displayName = "Bounce when it goes taut (0..1)", type = "float", default = 0.0 },
        { name = "breakLength", displayName = "Break length (m, 0 = never)", type = "float", default = 0.0 }
    }
}

local function clamp(value, low, high) return math.min(math.max(value, low), high) end
local function num(value, fallback) return type(value) == "number" and value or fallback end

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

local function positionOf(scene, id)
    local ok, position = pcall(function() return Object(scene, id).position end)
    return ok and position or nil
end

function Rope:init()
    RegisterEngineEvent(self, "onFixedUpdate")
    self.minLength = math.max(0.05, num(self.minLength, 0.5))
    self.maxLength = math.max(self.minLength, num(self.maxLength, 30.0))
    self.length = clamp(num(self.length, 4.0), self.minLength, self.maxLength)
    self.damping = clamp(num(self.damping, 0.05), 0.0, 1.0)
    self.bounce = clamp(num(self.bounce, 0.0), 0.0, 1.0)
    self.breakLength = math.max(0.0, num(self.breakLength, 0.0))
    self.offset = asVec3(self.anchorOffset, Vector3(0.0, 0.0, 0.0))
    self.stretch, self.lastWarn, self.attached, self.nextTry = 1.0, -1.0, false, 0.0

    local ok, jointOrError = pcall(function() return Joint3D(self.scene, self.entity) end)
    self.joint = (ok and jointOrError) or nil
    if not self.joint then
        self:warn("this entity needs a Joint3D component for the pivot: " .. tostring(jointOrError))
    end
    self:attach(self.anchor, self.subject)
end

function Rope:warn(message)
    local now = num(Engine.systemTime, 0.0)
    if now - num(self.lastWarn, -1.0) < 1.0 then return end
    self.lastWarn = now
    print("[Rope] " .. message)
end

function Rope:anchorPoint()
    local point = (self.anchorId and positionOf(self.scene, self.anchorId)) or positionOf(self.scene, self.entity)
    if point then return point + self.offset end
    return nil
end

function Rope:attach(anchorEntity, bodyEntity)
    self.anchorId = entityId(anchorEntity) or entityId(self.scene:findEntity(self.fallbackAnchorName))
    self.bodyId = entityId(bodyEntity) or entityId(self.subject) or self.entity
    local ok, body = pcall(function() return Body3D(self.scene, self.bodyId) end)
    self.body = (ok and body) or nil
    if not self.body then
        self:warn("the subject has no Body3D component, so there is nothing to swing")
        return false
    end
    -- Jolt builds a joint only between two bodies, and loadPointJoint3D checks both signatures, so a
    -- anchor without a static Body3D leaves the rope silently unattached.
    local anchored, anchorBody = pcall(function() return Body3D(self.scene, self.anchorId) end)
    self.anchorBody = (anchored and anchorBody) or nil
    if not self.anchorBody then
        self:warn("the anchor needs a static Body3D: a plain entity cannot hold a joint")
        return false
    end
    return self:applyJoint()
end

function Rope:applyJoint()
    if not self.joint or not self.body or not self.anchorId then return false end
    local point = self:anchorPoint()
    if not point then
        self:warn("the anchor entity has no Object component to read a position from")
        return false
    end
    -- The set*Joint calls only store the request; the solver builds it on the next fixed step and
    -- retries while needUpdateJoint is set, so there is no success value to read here.
    local ok, err = pcall(function()
        if self.ropeLike ~= false then
            self.joint:setPointJoint(self.anchorId, self.bodyId, point)
        else
            self.joint:setDistanceJoint(self.anchorId, self.bodyId)
        end
    end)
    if not ok then self:warn("the joint refused to build: " .. tostring(err)) end
    self.attached = ok == true
    return self.attached
end

-- Lua cannot destroy a 3D joint (destroyJoint3D takes a Joint3DComponent, which is not bound), so
-- releasing means the script stops enforcing the length and the pivot stays as it was last built.
function Rope:detach()
    self.attached = false
end

function Rope:setLength(value)
    self.length = clamp(num(value, self.length), self.minLength, self.maxLength)
    -- A distance joint keeps the distance it was built with, so only a rod needs rebuilding.
    if self.ropeLike == false then self:applyJoint() end
end

function Rope:reel(delta)
    self:setLength(self.length + num(delta, 0.0))
end

function Rope:getTension() return self.stretch end

function Rope:onFixedUpdate()
    if not self.attached then
        -- Retrying every step would re-issue the joint request forever; give the scene a moment between tries.
        local now = num(Engine.systemTime, 0.0)
        if now - num(self.nextTry, 0.0) < 0.5 then return end
        self.nextTry = now + 0.5
        if not self:applyJoint() then return end
    end
    local dt = clamp(num(Engine.deltatime, 1.0 / 60.0), 0.0005, 0.05)
    local ok, com = pcall(function() return self.body:getCenterOfMassPosition() end)
    local point = self:anchorPoint()
    if not ok or not com or not point then return end

    local dx, dy, dz = com.x - point.x, com.y - point.y, com.z - point.z
    local distance = math.sqrt(dx * dx + dy * dy + dz * dz)
    if distance < 0.0001 then self.stretch = 0.0 return end
    self.stretch = distance / math.max(self.length, 0.001)

    if self.breakLength > 0.0 and distance > self.breakLength then
        self:detach()
        self:warn(string.format("the rope broke at %.2f m", distance))
        return
    end

    -- No joint in Jolt is a one-way rope: the pivot holds the anchor, and this clamp is what stops
    -- the line from also yanking the body back inward.
    if distance > self.length then
        local inv = 1.0 / distance
        local dir = Vector3(dx * inv, dy * inv, dz * inv)
        local velocity = self.body.linearVelocity
        local outward = velocity:dotProduct(dir)
        if outward > 0.0 then
            self.body.linearVelocity = velocity - dir * (outward * (1.0 + self.bounce))
        end
        local closing = clamp((distance - self.length) / dt, 0.0, 60.0)
        local impulse = dir * (-closing * math.max(num(self.body.mass, 0.0), 0.01))
        pcall(function() self.body:applyImpulse(impulse) end)
    end

    if self.damping > 0.0 then
        self.body.linearVelocity = self.body.linearVelocity * (1.0 - self.damping)
    end
end

return Rope
