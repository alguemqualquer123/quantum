-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Reusable burst emitter any component can fire: sparks, dust, impact poof, confetti. Attach it to
-- category: Audio & Particles
-- the entity that has the Particles component, then call self.burst:emit(point) from another script.
local ParticleBurst = {
    properties = {
        { name = "particles", displayName = "Particles (0 = this entity)", type = "entity" },
        { name = "color", displayName = "Color (r, g, b)", type = "color3", default = { 1.0, 0.8, 0.4 } },
        { name = "color2", displayName = "Color at the end of life (r, g, b)", type = "color3", default = { 0.2, 0.2, 0.2 } },
        { name = "count", displayName = "Particles per burst", type = "int", default = 32 },
        { name = "life", displayName = "Life (seconds)", type = "float", default = 0.7 },
        { name = "radius", displayName = "Emit radius", type = "float", default = 0.4 },
        { name = "shape", displayName = "Shape (0 = sphere, 1 = cone)", type = "int", default = 0 },
        { name = "coneAngle", displayName = "Cone angle (degrees)", type = "float", default = 25.0 },
        { name = "speed", displayName = "Launch speed", type = "float", default = 3.0 },
        { name = "gravity", displayName = "Gravity pull", type = "float", default = 9.0 },
        { name = "size", displayName = "Size (units)", type = "float", default = 0.2 },
        { name = "endSize", displayName = "Size at the end of life", type = "float", default = 0.02 },
        { name = "alpha", displayName = "Alpha", type = "float", default = 1.0 },
        { name = "timeScale", displayName = "Time scale (stretches life)", type = "float", default = 1.0 },
        { name = "loop", displayName = "Keep emitting", type = "bool", default = false },
        { name = "autoBurst", displayName = "Burst on key press", type = "bool", default = false },
        { name = "triggerKey", displayName = "Burst key (0 = off)", type = "int", default = 0 },
        { name = "followSubject", displayName = "Follow another entity", type = "bool", default = false },
        { name = "subject", displayName = "Subject to follow", type = "entity" },
        { name = "sound", displayName = "Sound on burst", type = "SoundOnEvent" },
        { name = "enabled", displayName = "Enabled", type = "bool", default = true }
    }
}

local function number(value, fallback) return (type(value) == "number" and value == value) and value or fallback end
local function clamp(value, low, high) return math.min(math.max(value, low), high) end
local function entityId(reference)
    if type(reference) == "number" then return reference ~= 0 and reference or nil end
    if reference and type(reference.entity) == "number" then return reference.entity ~= 0 and reference.entity or nil end
    return nil
end
-- emit() takes a Vector3, a script table, an engine handle or a bare entity id and reads a position.
local function asPoint(scene, reference)
    local ok, position = pcall(function()
        if type(reference) ~= "table" and type(reference) ~= "userdata" then return nil end
        if type(reference.x) == "number" then return reference end
        local id = entityId(reference)
        if not id then return nil end
        return Object(scene, id).position
    end)
    return (ok and position) or nil
end
-- The engine delivers a colour row as Vector3 userdata and the Inspector default as a list.
local function colorOf(value, fallback)
    local ok, out = pcall(function()
        if type(value) ~= "table" and type(value) ~= "userdata" then return nil end
        if type(value.x) == "number" then
            return Vector3(value.x, number(value.y, value.x), number(value.z, value.x))
        end
        if type(value[1]) == "number" then
            return Vector3(value[1], number(value[2], value[1]), number(value[3], value[1]))
        end
        return nil
    end)
    return (ok and out) or Vector3(fallback[1], fallback[2], fallback[3])
end

function ParticleBurst:warn(message)
    if Engine.systemTime - self.lastWarn > 1.0 then
        self.lastWarn = Engine.systemTime
        print("[ParticleBurst] " .. message)
    end
end

function ParticleBurst:init()
    RegisterEngineEvent(self, "onUpdate")
    self.enabled = self.enabled ~= false
    self.id = entityId(self.particles) or self.entity
    self.keyWasDown, self.lastWarn = false, 0.0
    local ok, handle = pcall(function() return Particles(self.scene, self.id) end)
    self.handle = (ok and handle) or nil
    if self.handle then
        self:applyConfig()
    else
        print("[ParticleBurst] The entity needs a Particles component.")
    end
end

-- The engine has no particle time scale, so timeScale stretches life and every ramp along with it.
function ParticleBurst:applyConfig()
    local particles = self.handle
    local life = math.max(number(self.life, 0.7) * math.max(number(self.timeScale, 1.0), 0.01), 0.05)
    local speed = number(self.speed, 3.0)
    local radius = math.max(number(self.radius, 0.4), 0.001)
    local size = math.max(number(self.size, 0.2), 0.001)
    local alpha = clamp(number(self.alpha, 1.0), 0.0, 1.0)
    particles:setLifeInitializer(life)
    if (self.shape or 0) == 1 then
        particles:setConePositionInitializer(number(self.coneAngle, 25.0), radius * 2.0)
    else
        particles:setSpherePositionInitializer(radius)
    end
    particles:setVelocityInitializer(Vector3(0.0, speed, 0.0))
    particles:setVelocityModifier(0.0, life, Vector3(0.0, speed, 0.0), Vector3(0.0, 0.0, 0.0))
    particles:setAccelerationInitializer(Vector3(0.0, -math.max(number(self.gravity, 9.0), 0.0), 0.0))
    local from = colorOf(self.color, { 1.0, 0.8, 0.4 })
    local to = colorOf(self.color2, { 0.2, 0.2, 0.2 })
    particles:setColorInitializer(from)
    particles:clearColorGradient()
    particles:addColorGradientStop(0.0, from)
    particles:addColorGradientStop(life, to)
    particles:setAlphaInitializer(alpha)
    particles:setAlphaModifier(0.0, life, alpha, 0.0)
    particles:setSizeInitializer(size)
    particles:setSizeModifier(0.0, life, size, math.max(number(self.endSize, 0.02), 0.0))
    particles.loop = self.loop == true
    particles.emitter = self.loop == true
end

function ParticleBurst:setEmitter(point)
    local id = self.id
    pcall(function() Object(self.scene, id).position = point end)
end

function ParticleBurst:emit(pointOrEntity, scale)
    if not self.enabled or not self.handle then return false end
    local point = asPoint(self.scene, pointOrEntity)
    if point then self:setEmitter(point) end
    local count = clamp(math.floor(number(self.count, 32) * number(scale, 1.0)), 1, 4096)
    pcall(function() self.handle:addBurst(0.0, count) end)
    -- The sound slot only carries methods when the wired entity really has the SoundOnEvent script;
    -- a bare entity handle behind it means the wiring is wrong, not that the burst failed.
    local sound = self.sound
    if type(sound) == "table" and type(sound.play) == "function" then
        pcall(function() sound:play(point) end)
    elseif type(sound) == "userdata" then
        self:warn("wire this: a SoundOnEvent component on the entity in the Sound slot")
    end
    return true
end

-- Particles has no stop(): clearing the queued bursts and the emitter flag is what silence means.
function ParticleBurst:stop()
    if not self.handle then return false end
    self.handle.emitter = false
    pcall(function() self.handle:clearBursts() end)
    return true
end

function ParticleBurst:reset()
    if not self.handle then return false end
    pcall(function() self.handle:clearBursts() self.handle:reset() end)
    self:applyConfig()
    return true
end

function ParticleBurst:setColor(value)
    self.color = colorOf(value, { 1.0, 0.8, 0.4 })
    if self.handle then self.handle:setColorInitializer(self.color) end
end
function ParticleBurst:setCount(value) self.count = clamp(math.floor(number(value, 32)), 1, 4096) end
function ParticleBurst:setEnabled(value)
    self.enabled = value == true
    if not self.enabled then self:stop() end
end

function ParticleBurst:onUpdate()
    if not self.enabled then return end
    if self.followSubject then
        local point = asPoint(self.scene, self.subject)
        if point then self:setEmitter(point) end
    end
    local key = number(self.triggerKey, 0)
    if self.autoBurst and key > 0 then
        local down = Input.isKeyPressed(key)
        if down and not self.keyWasDown then self:emit(nil, 1.0) end
        self.keyWasDown = down
    end
end

return ParticleBurst
