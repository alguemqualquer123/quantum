-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- One call for "something got hit": particles, sound, camera shake and a screen flash from a single
-- category: Screen Effects
-- play(strength, point). Attach it to the entity that takes the hits; unwired slots cost a warning.
local ImpactEffect = {
    properties = {
        { name = "preset", displayName = "Preset (0 generic, 1 heavy, 2 splash, 3 magic)", type = "int", default = 0 },
        { name = "burst", displayName = "ParticleBurst slot", type = "ParticleBurst" },
        { name = "particles", displayName = "Particles entity (fallback)", type = "entity" },
        { name = "sound", displayName = "SoundOnEvent slot", type = "SoundOnEvent" },
        { name = "shake", displayName = "CameraShake slot", type = "CameraShake" },
        { name = "flash", displayName = "ScreenFade slot", type = "ScreenFade" },
        { name = "burstCount", displayName = "Particle count override (0 = preset)", type = "int", default = 0 },
        { name = "burstScale", displayName = "Burst size multiplier", type = "float", default = 1.0 },
        { name = "shakeScale", displayName = "Trauma per unit of strength", type = "float", default = 1.0 },
        { name = "soundScale", displayName = "Volume per unit of strength", type = "float", default = 1.0 },
        { name = "flashAlpha", displayName = "Flash alpha override (0 = preset)", type = "float", default = 0.0 },
        { name = "flashTime", displayName = "Flash time (seconds)", type = "float", default = 0.12 },
        { name = "minStrength", displayName = "Minimum strength", type = "float", default = 0.15 },
        { name = "maxStrength", displayName = "Maximum strength", type = "float", default = 1.0 },
        { name = "cooldownSeconds", displayName = "Cooldown (seconds)", type = "float", default = 0.06 },
        { name = "enabled", displayName = "Enabled", type = "bool", default = true },
        { name = "autoPlayOnTrigger", displayName = "Preview on key press", type = "bool", default = false },
        { name = "triggerKey", displayName = "Preview key (0 = off)", type = "int", default = 0 }
    }
}

-- A preset swaps counts, colours, trauma and volume in one field.
local PRESETS = {
    { count = 24, scale = 1.0, trauma = 0.4, volume = 1.0, alpha = 0.25, color = { 1.0, 0.85, 0.6 } },
    { count = 60, scale = 1.8, trauma = 0.9, volume = 1.2, alpha = 0.5, color = { 1.0, 0.45, 0.15 } },
    { count = 40, scale = 1.2, trauma = 0.25, volume = 0.8, alpha = 0.2, color = { 0.55, 0.85, 1.0 } },
    { count = 36, scale = 1.4, trauma = 0.35, volume = 0.9, alpha = 0.35, color = { 0.65, 0.4, 1.0 } }
}

local function num(value, fallback) return type(value) == "number" and value or fallback end

local function clamp(value, low, high)
    if value < low then return low end
    if value > high then return high end
    return value
end

local function entityId(value)
    if type(value) == "number" then return value end
    if type(value) == "table" or type(value) == "userdata" then return num(value.entity, 0) end
    return 0
end

function ImpactEffect:init()
    RegisterEngineEvent(self, "onUpdate")
    self.hitCount, self.strength = 0, 0.0
    self.lastImpactTime, self.lastWarn = -1000.0, -1.0
    local id = entityId(self.particles)
    if id <= 0 then id = entityId(self.entity) end
    local ok, handle = pcall(function() return Particles(self.scene, id) end)
    self.handle = (ok and handle) or nil
end

function ImpactEffect:config() return PRESETS[clamp(math.floor(num(self.preset, 0)) + 1, 1, #PRESETS)] end

function ImpactEffect:warn(text)
    local time = num(Engine.systemTime, 0.0)
    if time - self.lastWarn < 1.0 then return end
    self.lastWarn = time
    print("[ImpactEffect] " .. text)
end

-- Every channel runs through pcall, so a slot pointing at the wrong component degrades into a
-- warning and the remaining channels still play.
function ImpactEffect:try(name, fn)
    local ok, err = pcall(fn)
    if not ok then self:warn(name .. ": " .. tostring(err)) end
end

function ImpactEffect:play(strength, point)
    if self.enabled == false then return false end
    local time = num(Engine.systemTime, 0.0)
    if time - self.lastImpactTime < math.max(num(self.cooldownSeconds, 0.0), 0.0) then return false end

    local low = num(self.minStrength, 0.15)
    self.strength = clamp(num(strength, 1.0), low, math.max(num(self.maxStrength, 1.0), low))
    self.lastImpactTime = time
    self.hitCount = self.hitCount + 1

    local config = self:config()
    local value = self.strength
    local where = (type(point) == "table" or type(point) == "userdata") and point or Vector3(0.0, 0.0, 0.0)
    local wired = false

    -- A script slot answers only when the wired entity really carries that script; a bare entity
    -- handle has none of these methods, and unwired channels leave `wired` false for the warning.
    if type(self.burst) == "table" and type(self.burst.emit) == "function" then
        wired = true
        local scale = config.scale * value * math.max(num(self.burstScale, 1.0), 0.01)
        self:try("burst", function() self.burst:emit(where, scale) end)
    elseif self.handle then
        wired = true
        local base = math.floor(num(self.burstCount, 0))
        if base <= 0 then base = config.count end
        local count = math.max(math.floor(base * value), 1)
        local color = config.color
        self:try("particles", function()
            self.handle:setColorInitializer(Vector3(color[1], color[2], color[3]))
            self.handle:addBurst(0.0, count)
        end)
    end

    if type(self.sound) == "table" and type(self.sound.play) == "function" then
        wired = true
        local volume = clamp(config.volume * value * math.max(num(self.soundScale, 1.0), 0.0), 0.0, 1.0)
        self:try("sound", function()
            if type(self.sound.setVolume) == "function" then self.sound:setVolume(volume) else self.sound.volume = volume end
            self.sound:play(where)
        end)
    end

    if type(self.shake) == "table" and type(self.shake.addTrauma) == "function" then
        wired = true
        local trauma = value * config.trauma * math.max(num(self.shakeScale, 1.0), 0.0)
        self:try("shake", function() self.shake:addTrauma(trauma) end)
    end

    if type(self.flash) == "table" and type(self.flash.flashTo) == "function" then
        wired = true
        local alpha = num(self.flashAlpha, 0.0)
        if alpha <= 0.0 then alpha = config.alpha * value end
        local color = Vector4(config.color[1], config.color[2], config.color[3], 1.0)
        self:try("flash", function()
            self.flash:flashTo(color, clamp(alpha, 0.0, 1.0), num(self.flashTime, 0.12))
        end)
    end

    if not wired then
        self:warn("no channel wired: fill a burst, particles, sound, shake or flash slot")
    end
    return true
end

function ImpactEffect:setEnabled(value) self.enabled = value ~= false end
function ImpactEffect:getStrength() return self.strength end
function ImpactEffect:hits() return self.hitCount end

function ImpactEffect:onUpdate()
    if self.autoPlayOnTrigger == false or self.enabled == false then return end
    local key = num(self.triggerKey, 0)
    if key <= 0 then return end
    if Input.isKeyPressed(key) then
        local ok, object = pcall(function() return Object(self.scene, self.entity) end)
        self:play(1.0, ok and object and object.position or nil)
    end
end

return ImpactEffect
