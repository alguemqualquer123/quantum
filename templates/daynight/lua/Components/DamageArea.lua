-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Hazard volume: spikes, lava, acid, a gas cloud. Attach it to the hazard, size the box or sphere
-- category: Interaction & Combat
-- around it, then drag the victim into Subject and its Health component into the Health slot.
local DamageArea = {
    properties = {
        { name = "shape", displayName = "Shape (0 = box, 1 = sphere)", type = "int", default = 0 },
        { name = "size", displayName = "Box size (x, y, z)", type = "vector3", default = { x = 2.0, y = 1.0, z = 2.0 } },
        { name = "radius", displayName = "Sphere radius", type = "float", default = 2.0 },
        { name = "subject", displayName = "Subject", type = "entity" },
        { name = "fallbackName", displayName = "Subject name if slot is empty", type = "string", default = "Player" },
        { name = "health", displayName = "Health to damage", type = "Health" },
        { name = "damagePerSecond", displayName = "Damage per second", type = "float", default = 10.0 },
        { name = "tickSeconds", displayName = "Damage every (seconds)", type = "float", default = 0.5 },
        { name = "body", displayName = "Body to push (empty = subject)", type = "entity" },
        { name = "knockback", displayName = "Knockback impulse (0 = none)", type = "float", default = 4.0 },
        { name = "pushUp", displayName = "Upward part of the impulse", type = "float", default = 1.5 },
        { name = "onlyWhenMoving", displayName = "Only while the subject moves", type = "bool", default = false },
        { name = "enabled", displayName = "Enabled", type = "bool", default = true },
        { name = "sound", displayName = "Sound entity", type = "entity" },
        { name = "volume", displayName = "Sound volume", type = "float", default = 0.6 },
        { name = "soundEvery", displayName = "Sound every (seconds)", type = "float", default = 1.0 },
        { name = "particles", displayName = "Particles entity", type = "entity" },
        { name = "image", displayName = "Tint image entity", type = "entity" },
        { name = "colorHint", displayName = "Tint colour", type = "color3", default = { x = 1.0, y = 0.35, z = 0.1 } }
    }
}

-- A slot reaches Lua as a script table, an engine handle or the bare stored id (0 = NULL_ENTITY).
local function entityId(reference)
    if type(reference) == "number" then return reference ~= 0 and reference or nil end
    if type(reference) ~= "table" and type(reference) ~= "userdata" then return nil end
    return reference.entity ~= 0 and reference.entity or nil
end
local function slotId(scene, reference, fallbackName)
    local id = entityId(reference)
    if not id and fallbackName and fallbackName ~= "" then id = entityId(scene:findEntity(fallbackName)) end
    return id
end
local function positionOf(scene, id)
    if not id then return nil end
    local ok, position = pcall(function() return Object(scene, id).position end)
    return (ok and position) or nil
end
-- The engine delivers a vector row as Vector3 userdata and the Inspector default as a table; the
-- pcall keeps an entity handle from raising when it has no such channel.
local function axis(vector, name, fallback)
    local ok, value = pcall(function()
        if type(vector) ~= "table" and type(vector) ~= "userdata" then return nil end
        return tonumber(vector[name])
    end)
    return (ok and value) or fallback
end
-- Handles are cheap but not free: keep one per resolved id until the slot points elsewhere.
local function widget(scene, cache, key, reference, class)
    local id = entityId(reference)
    if not id then return nil end
    local entry = cache[key]
    if not entry or entry.id ~= id then
        local ok, handle = pcall(class, scene, id)
        if not ok or not handle then return nil end
        entry = { id = id, handle = handle }
        cache[key] = entry
    end
    return entry.handle
end

function DamageArea:init()
    RegisterEngineEvent(self, "onUpdate")
    self.radius = math.max(tonumber(self.radius) or 2.0, 0.05)
    self.tickSeconds = math.max(tonumber(self.tickSeconds) or 0.5, 0.05)
    self.rate = math.max(tonumber(self.damagePerSecond) or 0.0, 0.0)
    self.handles, self.lastWarn = {}, 0.0
    self.sinceTick, self.sinceSound, self.insideNow = self.tickSeconds, 0.0, false
    local ok, object = pcall(function() return Object(self.scene, self.entity) end)
    self.object = (ok and object) or nil
end

function DamageArea:warn(message)
    if Engine.systemTime - self.lastWarn > 1.0 then
        self.lastWarn = Engine.systemTime
        print("[DamageArea] " .. message)
    end
end

function DamageArea:setRate(value)
    self.rate = math.max(tonumber(value) or self.rate, 0.0)
    return self.rate
end
function DamageArea:setEnabled(value) self.enabled = value == true end
function DamageArea:isActive() return self.enabled == true and self.rate > 0.0 end
function DamageArea:inside() return self.insideNow end

-- No overlap queries from Lua, so the volume is tested against the subject position by hand. The
-- box stays axis aligned to the world: a rotated hazard should use the sphere.
function DamageArea:overlaps(centre, point)
    local dx, dy, dz = point.x - centre.x, point.y - centre.y, point.z - centre.z
    if self.shape == 1 then return math.sqrt(dx * dx + dy * dy + dz * dz) <= self.radius end
    return math.abs(dx) <= math.max(axis(self.size, "x", 2.0) * 0.5, 0.01)
        and math.abs(dy) <= math.max(axis(self.size, "y", 1.0) * 0.5, 0.01)
        and math.abs(dz) <= math.max(axis(self.size, "z", 2.0) * 0.5, 0.01)
end

-- Travel is estimated from the transform, because a Lua script cannot read a body's velocity
-- unless the project put one in the slot.
function DamageArea:isSubjectMoving(point, dt)
    local moving = true
    if self.lastPoint and dt > 0.0001 then moving = point:distance(self.lastPoint) / dt > 0.05 end
    self.lastPoint = Vector3(point.x, point.y, point.z)
    return moving
end

function DamageArea:push(point, centre)
    local knockback = tonumber(self.knockback) or 0.0
    local id = entityId(self.body) or slotId(self.scene, self.subject, self.fallbackName)
    if knockback <= 0.0 or not id then return end
    local ok, body = pcall(function() return Body3D(self.scene, id) end)
    if not ok or not body then return end
    local dx, dz = point.x - centre.x, point.z - centre.z
    local flat = math.sqrt(dx * dx + dz * dz)
    if flat < 0.0001 then dx, dz, flat = 0.0, 1.0, 1.0 end
    local up = tonumber(self.pushUp) or 0.0
    pcall(function() body:applyImpulse(Vector3(dx / flat * knockback, up, dz / flat * knockback)) end)
end

function DamageArea:visuals(active, dt)
    local particles = widget(self.scene, self.handles, "particles", self.particles, Particles)
    if particles then particles.emitter = active end
    local image = widget(self.scene, self.handles, "image", self.image, Image)
    if image then
        image.color = Vector4(axis(self.colorHint, "x", 1.0), axis(self.colorHint, "y", 0.35), axis(self.colorHint, "z", 0.1), 1.0)
        image.alpha = active and 1.0 or 0.0
    end
    self.sinceSound = self.sinceSound + dt
    if not active or self.sinceSound < math.max(tonumber(self.soundEvery) or 1.0, 0.05) then return end
    self.sinceSound = 0.0
    local sound = widget(self.scene, self.handles, "sound", self.sound, Sound)
    if sound then
        sound.volume = math.max(tonumber(self.volume) or 0.6, 0.0)
        pcall(function() sound:play() end)
    end
end

function DamageArea:onUpdate()
    if not self.object then self:warn("this entity needs a Transform component") return end
    local dt = math.min(Engine.deltatime, 0.05)
    local point = positionOf(self.scene, slotId(self.scene, self.subject, self.fallbackName))
    if not point then
        self.insideNow = false
        self:visuals(false, dt)
        self:warn("wire this: drag the player into the Subject slot")
        return
    end
    local centre = self.object.position
    self.insideNow = self:overlaps(centre, point)
    if self.onlyWhenMoving == true and self.insideNow and not self:isSubjectMoving(point, dt) then
        self.insideNow = false
    end
    self:visuals(self.insideNow and self:isActive(), dt)
    if not self.insideNow or not self:isActive() then return end
    self.sinceTick = self.sinceTick + dt
    if self.sinceTick < self.tickSeconds then return end
    self.sinceTick, self.lastPoint = 0.0, Vector3(point.x, point.y, point.z)
    self:push(point, centre)
    -- The Health component owns the invulnerability window, so this only repeats the call.
    local health = self.health
    if type(health) == "table" and type(health.takeDamage) == "function" then
        pcall(function() health:takeDamage(self.rate * self.tickSeconds, self.entity) end)
    elseif type(health) == "userdata" then
        -- A script slot carries takeDamage only when the wired entity really runs that script.
        self:warn("wire this: a Health component in the Health slot")
    end
end

return DamageArea
