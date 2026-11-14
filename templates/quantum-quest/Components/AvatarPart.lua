-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Hang one model off another: hair onto a head, a helmet onto a skull, a sword onto a hand.
-- The engine keeps no socket table, but every glTF node - the joints included - becomes a named
-- entity under the model, so the bone name *is* the socket name and the Inspector can point a
-- part at it without any C++ help.
-- category: Characters
-- Put this on the entity that should follow the body, drag the body into `body` and write the
-- bone (or any node) name in `bone`.
local AvatarPart = {
    properties = {
        { name = "body", displayName = "Body model", type = "entity" },
        { name = "bodyName", displayName = "Body name if the slot is empty", type = "string", default = "Player" },
        { name = "bone", displayName = "Bone or node name", type = "string", default = "Head" },
        { name = "aliases", displayName = "More names to try, comma separated", type = "string", default = "" },
        { name = "offset", displayName = "Offset in bone space (m)", type = "vector3", default = { 0.0, 0.0, 0.0 } },
        { name = "rotationOffset", displayName = "Rotation offset (degrees)", type = "vector3", default = { 0.0, 0.0, 0.0 } },
        { name = "scaleFactor", displayName = "Scale multiplier", type = "float", default = 1.0 },
        { name = "mode", displayName = "Mode (attach = reparent, follow = copy transform)", type = "string", default = "attach" },
        { name = "retryAfter", displayName = "Seconds between tries (0 = a single try)", type = "float", default = 0.5 },
        { name = "syncEveryFrame", displayName = "Re-apply the transform every frame", type = "bool", default = false }
    }
}

local function num(value, fallback)
    if type(value) == "number" and value == value then return value end
    return fallback
end

-- A slot reaches Lua as a script table, as an engine handle, or as the bare id the inspector
-- stored, and 0 is NULL_ENTITY: normalise all three shapes to one.
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

-- The engine delivers a vector row as Vector3 userdata and the Inspector default as a table; the
-- pcall keeps an entity handle from raising when it has no such channel.
local function asVec3(value, fallback)
    local ok, out = pcall(function()
        if type(value) ~= "table" and type(value) ~= "userdata" then return fallback end
        if type(value.x) == "number" then return Vector3(value.x, value.y or 0.0, value.z or 0.0) end
        if type(value[1]) == "number" then return Vector3(value[1], value[2] or 0.0, value[3] or 0.0) end
        return fallback
    end)
    return ok and out or fallback
end

-- `aliases` is the fallback chain a rigger needs: the same head is "Head" in one pack,
-- "head_01" in another and "mixamorig:Head" in a third.
local function candidateNames(self)
    local list = {}
    local function add(value)
        local name = tostring(value or ""):gsub("^%s+", ""):gsub("%s+$", "")
        if name ~= "" then
            for _, existing in ipairs(list) do
                if existing == name then return end
            end
            list[#list + 1] = name
        end
    end
    add(self.bone)
    for raw in string.gmatch(tostring(self.aliases or ""), "[^,]+") do
        add(raw)
    end
    if #list == 0 then add("Head") end
    return list
end

local function warn(self, message)
    if Engine.systemTime - (self.lastWarn or -1.0) > 1.0 then
        self.lastWarn = Engine.systemTime
        print("[AvatarPart] " .. message)
    end
end

function AvatarPart:object()
    local ok, object = pcall(function()
        return Object(self.scene, self.entity)
    end)
    if ok and object then return object end
    return nil
end

-- Model:getBone raises on an unknown name, and an exception crossing the Lua boundary takes the
-- process down, so the bone is resolved through the name index instead: scoped to the body, so a
-- part and a body that both carry a "Head" cannot pick the wrong one.
function AvatarPart:findBone()
    local bodyId = entityId(self.body)
    if not bodyId and self.bodyName and self.bodyName ~= "" then
        bodyId = entityId(self.scene:findEntity(self.bodyName))
    end
    if not bodyId then
        warn(self, "no body model: fill the Body slot or set a body name")
        return nil, nil
    end

    for _, name in ipairs(candidateNames(self)) do
        local ok, found = pcall(function()
            return self.scene:findEntityInParent(name, bodyId)
        end)
        local id = ok and entityId(found) or nil
        if id then
            return id, name
        end
    end
    return bodyId, nil
end

-- The local transform the part was authored with, read once before any offset is written, so a
-- re-attach or a sync cannot compound the offset onto itself.
function AvatarPart:capture(object)
    if self.captured then return end
    self.bindPosition = object.position
    self.bindRotation = object.rotation
    self.bindScale = object.scale
    self.captured = true
end

function AvatarPart:apply(object, boneId)
    local offset = asVec3(self.offset, Vector3(0.0, 0.0, 0.0))
    local factor = num(self.scaleFactor, 1.0)
    local rotation = self.bindRotation
    if self.rotationOffset then
        local e = asVec3(self.rotationOffset, Vector3(0.0, 0.0, 0.0))
        local ok, shifted = pcall(function()
            return rotation * Quaternion(e.x, e.y, e.z)
        end)
        if ok and shifted then rotation = shifted end
    end

    if self.mode == "follow" then
        local ok, bone = pcall(function()
            return Object(self.scene, boneId)
        end)
        if not ok or not bone then return false end
        local worldPosition = bone:getWorldPosition()
        local worldRotation = bone:getWorldRotation()
        object.position = worldPosition + (worldRotation * offset)
        object.rotation = worldRotation * rotation
        object.scale = self.bindScale * factor
        return true
    end

    object.position = self.bindPosition + offset
    object.rotation = rotation
    object.scale = self.bindScale * factor
    return true
end

function AvatarPart:attach()
    local object = self:object()
    if not object then
        if not self.transformWarning then
            self.transformWarning = true
            print("[AvatarPart] The entity needs a Transform component.")
        end
        return false
    end

    local boneId, resolved = self:findBone()
    if not boneId then return false end
    if not resolved then
        warn(self, "bone not found under the body: " .. tostring(self.bone))
        return false
    end

    self:capture(object)
    if self.mode ~= "follow" then
        local ok, err = pcall(function()
            local bone = Object(self.scene, boneId)
            bone:addChild(self.entity)
        end)
        if not ok then
            warn(self, "could not reparent onto " .. resolved .. ": " .. tostring(err))
            return false
        end
    end
    if not self:apply(object, boneId) then return false end

    self.boneName = resolved
    self.boneEntity = boneId
    self.attached = true
    return true
end

function AvatarPart:init()
    RegisterEngineEvent(self, "onUpdate")

    self.attached = false
    self.captured = false
    self.released = false
    self.boneName = nil
    self.boneEntity = nil
    self.lastWarn = -1.0
    self.nextTry = 0.0

    -- The body's nodes only exist once its model has been read, and that is asynchronous, so the
    -- first try usually fails and the component keeps asking until the bone shows up.
    self:attach()
end

function AvatarPart:onUpdate()
    if self.attached then
        -- In "follow" the copied transform *is* the attachment, so it has to be rewritten every
        -- frame: the single apply done by attach() reads the bone before the load has animated it,
        -- and the part would be left sitting on the bind-pose origin forever. `syncEveryFrame` only
        -- means anything for "attach", where reparenting already carries the part.
        if self.syncEveryFrame or self.mode == "follow" then
            local object = self:object()
            if object then self:apply(object, self.boneEntity) end
        end
        return
    end

    if self.released then return end

    local wait = num(self.retryAfter, 0.5)
    if wait <= 0.0 then return end
    if Engine.systemTime < self.nextTry then return end
    self.nextTry = Engine.systemTime + wait
    self:attach()
end

-- Other scripts (a hit, a jump, a door) can knock the part loose again after it moved on its own.
-- The retry timer has to be stopped here: without the flag the next onUpdate would read
-- "not attached, bone unknown yet" and quietly re-attach the part the caller just released.
function AvatarPart:detach()
    if not self.attached then return false end
    local object = self:object()
    if object then
        pcall(function() object:removeParent() end)
        if self.captured then
            object.position = self.bindPosition
            object.rotation = self.bindRotation
            object.scale = self.bindScale
        end
    end
    self.attached = false
    self.released = true
    self.boneEntity = nil
    return true
end

function AvatarPart:rebind()
    self.captured = false
    self.attached = false
    self.released = false
    return self:attach()
end

return AvatarPart
