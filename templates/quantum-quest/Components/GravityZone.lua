-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- A box or sphere volume with its own gravity: it rewrites gravityFactor for the bodies inside and
-- category: Physics
-- restores them on the way out. Attach it to the zone entity and list the bodies in the slots.
local GravityZone = {
    properties = {
        { name = "direction", displayName = "Gravity direction", type = "vector3", default = { 0.0, -1.0, 0.0 } },
        { name = "strength", displayName = "Strength (m/s^2)", type = "float", default = 9.81 },
        { name = "factor", displayName = "Gravity factor", type = "float", default = 1.0 },
        { name = "shape", displayName = "Shape (0 = box, 1 = sphere)", type = "int", default = 0 },
        { name = "size", displayName = "Box size (m)", type = "vector3", default = { 8.0, 8.0, 8.0 } },
        { name = "radius", displayName = "Sphere radius (m)", type = "float", default = 6.0 },
        { name = "pull", displayName = "Also apply the force directly", type = "bool", default = false },
        { name = "affectDynamicsOnly", displayName = "Dynamic bodies only", type = "bool", default = true },
        { name = "subject", displayName = "Body (0 = this entity)", type = "entity" },
        { name = "targetA", displayName = "Target A", type = "entity" },
        { name = "targetB", displayName = "Target B", type = "entity" },
        { name = "targetC", displayName = "Target C", type = "entity" },
        { name = "targetD", displayName = "Target D", type = "entity" },
        { name = "targetNames", displayName = "Target names (comma separated)", type = "string", default = "" },
        { name = "visualHint", displayName = "Image or Model to tint while active", type = "entity" }
    }
}

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
        if type(value[1]) == "number" then return Vector3(value[1], num(value[2], 0.0), num(value[3], 0.0)) end
        return fallback
    end)
    return ok and out or fallback
end

function GravityZone:init()
    RegisterEngineEvent(self, "onFixedUpdate")
    self.dir = asVec3(self.direction, Vector3(0.0, -1.0, 0.0)):normalize()
    self.halfSize = asVec3(self.size, Vector3(8.0, 8.0, 8.0)) * 0.5
    self.radius = math.max(0.05, num(self.radius, 6.0))
    self.activeCount, self.lastWarn, self.hintOn = 0, -1.0, nil

    -- Lua cannot enumerate the scene and has no overlap query, so the zone works on the bodies the
    -- inspector lists: the subject, the four slots and any names written in Target names.
    local seen, list = {}, {}
    local function add(id)
        if id and not seen[id] then
            seen[id] = true
            list[#list + 1] = { id = id, saved = nil }
        end
    end
    local slots = { self.subject, self.targetA, self.targetB, self.targetC, self.targetD }
    for index = 1, #slots do add(entityId(slots[index])) end
    if type(self.targetNames) == "string" and self.targetNames ~= "" then
        for word in self.targetNames:gmatch("[^,;]+") do
            add(entityId(self.scene:findEntity(word:match("^%s*(.-)%s*$"))))
        end
    end

    for index = 1, #list do
        local entry = list[index]
        local ok, body = pcall(function() return Body3D(self.scene, entry.id) end)
        entry.body = (ok and body) or nil
        if not entry.body then
            self:warn("entity " .. tostring(entry.id) .. " has no Body3D component")
        end
    end
    self.candidates = list
    if #list == 0 then self:warn("no targets: fill a Target slot or the Target names list") end
end

function GravityZone:warn(message)
    local now = num(Engine.systemTime, 0.0)
    if now - num(self.lastWarn, -1.0) < 1.0 then return end
    self.lastWarn = now
    print("[GravityZone] " .. message)
end

function GravityZone:center()
    local ok, position = pcall(function() return Object(self.scene, self.entity).position end)
    if ok and position then return position end
    return Vector3(0.0, 0.0, 0.0)
end

function GravityZone:inside(position, center)
    if self.shape == 1 then
        return position:squaredDistance(center) <= self.radius * self.radius
    end
    return math.abs(position.x - center.x) <= self.halfSize.x
        and math.abs(position.y - center.y) <= self.halfSize.y
        and math.abs(position.z - center.z) <= self.halfSize.z
end

function GravityZone:contains(reference)
    local id = entityId(reference) or reference
    for index = 1, #self.candidates do
        local entry = self.candidates[index]
        if entry.id == id and entry.saved ~= nil then return true end
    end
    return false
end

function GravityZone:getActiveCount() return self.activeCount end

function GravityZone:setFactor(value)
    self.factor = num(value, num(self.factor, 1.0))
    for index = 1, #self.candidates do
        local entry = self.candidates[index]
        if entry.body and entry.saved ~= nil then
            pcall(function() entry.body.gravityFactor = self.factor end)
        end
    end
end

function GravityZone:setDirection(value) self.dir = asVec3(value, self.dir):normalize() end

function GravityZone:tintHint(active)
    if self.hintOn == active then return end
    self.hintOn = active
    local id = entityId(self.visualHint)
    if not id then return end
    local tint = active and 0.45 or 1.0
    -- Only an Image answers this call; a Model in the slot makes the pcall fail quietly.
    pcall(function() Image(self.scene, id).color = Vector4(tint, tint, tint, 1.0) end)
end

function GravityZone:onFixedUpdate()
    local center, count = self:center(), 0
    local factor, strength = num(self.factor, 1.0), num(self.strength, 9.81)

    for index = 1, #self.candidates do
        local entry = self.candidates[index]
        local body = entry.body
        if body then
            local ok, position = pcall(function() return body:getCenterOfMassPosition() end)
            local inside = (ok and position) and self:inside(position, center) or false
            local isDynamic = true
            pcall(function() isDynamic = (body.type == BodyType.DYNAMIC) end)
            if self.affectDynamicsOnly ~= false and not isDynamic then inside = false end

            if inside then
                if entry.saved == nil then
                    local saved = 1.0
                    pcall(function() saved = num(body.gravityFactor, 1.0) end)
                    entry.saved = saved
                end
                pcall(function()
                    body.gravityFactor = factor
                    if self.pull then
                        body:applyForce(self.dir * (strength * math.max(num(body.mass, 0.0), 0.01)))
                    end
                end)
                count = count + 1
            elseif entry.saved ~= nil then
                local saved = entry.saved
                entry.saved = nil
                pcall(function() body.gravityFactor = saved end)
            end
        end
    end
    self.activeCount = count
    self:tintHint(count > 0)
end

return GravityZone
