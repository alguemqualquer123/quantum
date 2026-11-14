-- (c) SR VINIX
-- SPDX-License-Identifier: MIT
-- Rain and snow that wet the world: one eased intensity closes in the fog, rains on the water
-- category: Environment & Motion
-- sim, ripples it, runs particles, ambience and wave gusts. Attach anywhere; every effect is a
-- slot or a fallback name, because Lua cannot query overlaps and targets must be wired.
local WeatherSystem = {
    properties = {
        { name = "type", displayName = "Precipitation (0 rain, 1 snow)", type = "int", default = 0 },
        { name = "intensity", displayName = "Intensity (0..1)", type = "float", default = 0.0 },
        { name = "targetIntensity", displayName = "Target intensity (0..1)", type = "float", default = 0.0 },
        { name = "changeSpeed", displayName = "Change speed (per second)", type = "float", default = 0.08 },
        { name = "fog", displayName = "Fog entity", type = "entity" },
        { name = "fallbackFogName", displayName = "Fog name if slot is empty", type = "string", default = "Fog" },
        { name = "sky", displayName = "Sky entity", type = "entity" },
        -- The storm range is what the weather drags the fog toward; the calm range is read off
        -- the Fog entity the first time the component looks at it, so the authored scene is the
        -- clear-weather state instead of a number hardcoded here.
        { name = "stormFogStart", displayName = "Fog start at full storm (m)", type = "float", default = 8.0 },
        { name = "stormFogEnd", displayName = "Fog end at full storm (m)", type = "float", default = 70.0 },
        { name = "stormFogColor", displayName = "Storm fog colour (black = keep the scene's)", type = "color3", default = { 0.0, 0.0, 0.0 } },
        { name = "skyDim", displayName = "Sky dimming at full storm (0..1)", type = "float", default = 0.45 },
        { name = "water", displayName = "Water entity", type = "entity" },
        { name = "fallbackWaterName", displayName = "Water name if slot is empty", type = "string", default = "Water" },
        { name = "rainAmount", displayName = "Rain into the water sim (per second)", type = "float", default = 0.35 },
        { name = "rainParticles", displayName = "Precipitation particles", type = "entity" },
        { name = "fallbackParticlesName", displayName = "Particles name if slot is empty", type = "string", default = "Rain" },
        { name = "ambientSound", displayName = "Weather ambience", type = "entity" },
        { name = "fallbackSoundName", displayName = "Sound name if slot is empty", type = "string", default = "WeatherSound" },
        { name = "waves", displayName = "WaterWaves component", type = "WaterWaves" },
        { name = "windDirection", displayName = "Wind direction (degrees)", type = "float", default = 35.0 },
        { name = "gustStrength", displayName = "Gust strength", type = "float", default = 0.8 },
        { name = "toggleKey", displayName = "Toggle key (0 = none)", type = "int", default = 0 },
        { name = "cycleKey", displayName = "Cycle key (0 = none)", type = "int", default = 0 },
        { name = "onStormEntity", displayName = "On storm: entity", type = "entity" },
        { name = "onStormMethod", displayName = "On storm: method", type = "string", default = "onStorm" },
        { name = "onClearEntity", displayName = "On clear: entity", type = "entity" },
        { name = "onClearMethod", displayName = "On clear: method", type = "string", default = "onClear" }
    }
}

local function clamp(v, lo, hi) if v < lo then return lo end; if v > hi then return hi end; return v end
-- An editor slot that was never touched reaches Lua as nil, a string, or a number that is not a
-- number; every tunable below reads through this so one unset property cannot stop the weather.
local function num(value, fallback)
    if type(value) == "number" and value == value then return value end
    return fallback
end
-- A slot reaches Lua as a script table, an engine handle or a bare id, and 0 is NULL_ENTITY.
local function entityId(reference)
    if type(reference) == "number" then return reference ~= 0 and reference or nil end
    if reference and type(reference.entity) == "number" then
        return reference.entity ~= 0 and reference.entity or nil
    end
    return nil
end
local function warn(self, message)
    if Engine.systemTime - self.lastWarn > 1.0 then
        self.lastWarn = Engine.systemTime; print("[WeatherSystem] " .. message)
    end
end
local function callHook(target, method)
    -- A hook slot only holds a script table when the target really carries that script.
    if type(target) ~= "table" or not method or method == "" then return end
    pcall(function()
        local fn = target[method]
        if type(fn) == "function" then fn(target) end
    end)
end
-- Bind an entity slot (or fallback name) to a cached engine handle.
local makers = { Water = Water, Fog = Fog, Particles = Particles, Sound = Sound, SkyBox = SkyBox }
local function bind(self, cache, slot, fallbackName, class)
    if self[cache] then return self[cache] end
    local id = entityId(slot)
    if not id and fallbackName and fallbackName ~= "" then id = entityId(self.scene:findEntity(fallbackName)) end
    if not id then return nil end
    local ok, handle = pcall(function() return makers[class](self.scene, id) end)
    if ok and handle then self[cache] = handle; return handle end
    return nil
end
local function keyPressed(self, keyName, flagName)
    local key = self[keyName]
    if type(key) ~= "number" or key <= 0 then return false end
    local down = Input.isKeyPressed(key)
    local edge = down and not self[flagName]
    self[flagName] = down
    return edge
end

function WeatherSystem:init()
    RegisterEngineEvent(self, "onUpdate")
    self.intensity = clamp(self.intensity or 0.0, 0.0, 1.0)
    self.targetIntensity = clamp(self.targetIntensity or 0.0, 0.0, 1.0)
    self.changeSpeed = math.max(self.changeSpeed or 0.08, 0.001)
    self.lastWarn, self.lastDisturb, self.lastGust = 0.0, 0.0, 0.0
    self.soundOn, self.wasStorm, self.mode = false, false, 0
    self.previousToggle, self.previousCycle, self.wasActive = false, false, self.intensity > 0.01
end
function WeatherSystem:setIntensity(value)
    if type(value) == "number" then self.targetIntensity = clamp(value, 0.0, 1.0) end
end
function WeatherSystem:rain() self.type = 0; self.targetIntensity = 1.0 end
function WeatherSystem:snow() self.type = 1; self.targetIntensity = 1.0 end
function WeatherSystem:clear(seconds)
    self.targetIntensity = 0.0
    if type(seconds) == "number" and seconds > 0.05 then self.changeSpeed = clamp(1.0 / seconds, 0.001, 10.0) end
end
function WeatherSystem:getIntensity() return self.intensity end

function WeatherSystem:onUpdate()
    local dt = math.min(Engine.deltatime, 0.05)
    if keyPressed(self, "toggleKey", "previousToggle") then
        self.targetIntensity = self.targetIntensity > 0.5 and 0.0 or 1.0
    elseif keyPressed(self, "cycleKey", "previousCycle") then
        -- Cycle walks clear -> rain -> snow -> clear so one key demos the whole system.
        self.mode = ((self.mode or 0) + 1) % 3
        if self.mode == 0 then self:clear(6.0) elseif self.mode == 1 then self:rain() else self:snow() end
    end

    local diff = self.targetIntensity - self.intensity
    if math.abs(diff) > 0.0005 then
        local step = math.min(self.changeSpeed * dt, math.abs(diff))
        self.intensity = clamp(self.intensity + (diff > 0.0 and step or -step), 0.0, 1.0)
    end
    if self.intensity > 0.75 and not self.wasStorm then
        self.wasStorm = true; callHook(self.onStormEntity, self.onStormMethod)
    elseif self.intensity < 0.5 then self.wasStorm = false end
    local active = self.intensity > 0.01
    if self.wasActive and not active then callHook(self.onClearEntity, self.onClearMethod) end
    self.wasActive = active
    local fog = bind(self, "fogHandle", self.fog, self.fallbackFogName, "Fog")
    local water = bind(self, "waterHandle", self.water, self.fallbackWaterName, "Water")
    local parts = bind(self, "particlesHandle", self.rainParticles, self.fallbackParticlesName, "Particles")
    local sound = bind(self, "soundHandle", self.ambientSound, self.fallbackSoundName, "Sound")
    local sky = bind(self, "skyHandle", self.sky, nil, "SkyBox")
    pcall(function()
        if fog then
            -- The clear-weather range is the scene's own, read once before the storm moves it.
            -- Re-reading every frame would chase the value this block just wrote.
            if not self.fogBase then
                local ok, startValue, endValue = pcall(function()
                    return fog.linearStart, fog.linearEnd
                end)
                if ok then self.fogBase = { startValue, endValue } end
            end
            local calmStart, calmEnd = self.fogBase and self.fogBase[1], self.fogBase and self.fogBase[2]
            local baseColor = self.fogBase and self.fogBase[3]
            if not calmStart then calmStart, calmEnd = 25.0, 220.0 end
            local i = self.intensity
            fog:setLinearStartEnd(calmStart + (num(self.stormFogStart, 8.0) - calmStart) * i,
                                  calmEnd + (num(self.stormFogEnd, 70.0) - calmEnd) * i)
            local tint = self.stormFogColor
            if tint and (num(tint[1] or tint.x, 0.0) > 0.0 or num(tint[2] or tint.y, 0.0) > 0.0
                    or num(tint[3] or tint.z, 0.0) > 0.0) then
                if not baseColor then
                    local ok, existing = pcall(function() return fog.color end)
                    baseColor = ok and existing or nil
                    if self.fogBase then self.fogBase[3] = baseColor end
                end
                if baseColor then
                    fog.color = Vector3(
                        num(baseColor.x, 0.7) + ((num(tint[1] or tint.x, 0.0)) - num(baseColor.x, 0.7)) * i,
                        num(baseColor.y, 0.7) + ((num(tint[2] or tint.y, 0.0)) - num(baseColor.y, 0.7)) * i,
                        num(baseColor.z, 0.7) + ((num(tint[3] or tint.z, 0.0)) - num(baseColor.z, 0.7)) * i)
                end
            end
        end
        if parts then parts.rate = math.floor(300 * self.intensity); parts.emitter = active end
        if sound then sound.volume = 0.85 * self.intensity end
        if water then
            water.simRain = self.type == 1 and 0.0 or (self.rainAmount or 0.35) * self.intensity
            -- Rain drops punch ripples a few times per second, scaled by intensity, not per frame.
            if self.intensity > 0.05 and Engine.systemTime - self.lastDisturb >= 1.0 / (self.intensity * 3.0) then
                self.lastDisturb = Engine.systemTime
                local p, s = water.position, water.size
                water:addDisturbance(Vector3(p.x + (math.random() - 0.5) * s.x, p.y,
                    p.z + (math.random() - 0.5) * s.y), 0.9, 0.12 * self.intensity)
            end
        end
        -- Sky dim relative to the colour the sky held when the storm began, so a DayNightCycle
        -- driving the same sky is not overwritten with an absolute value.
        if sky and active then
            if not self.skyBase then
                local ok, base = pcall(function() return sky.color end); self.skyBase = ok and base or nil
            end
            if self.skyBase then
                local b, d = self.skyBase, 1.0 - num(self.skyDim, 0.45) * self.intensity
                sky.color = Vector4(b.x * d, b.y * d, b.z * d, b.w or 1.0)
            end
        end
    end)

    if sound and (self.intensity > 0.03) ~= self.soundOn then
        self.soundOn = not self.soundOn; pcall(function() if self.soundOn then sound:play() else sound:pause() end end)
    end
    local waves = self.waves
    if type(waves) == "table" then
        if type(waves.setWindDirection) == "function" then pcall(function() waves:setWindDirection(self.windDirection) end) end
        if active and Engine.systemTime - self.lastGust >= 9.0 / self.intensity then
            self.lastGust = Engine.systemTime
            if type(waves.gust) == "function" then pcall(function() waves:gust(self.intensity * self.gustStrength) end) end
        end
    elseif type(waves) == "userdata" then
        warn(self, "wire this: the entity in the WaterWaves slot has no WaterWaves script")
    end
    if not (fog or water or parts or sound or sky) and type(self.waves) ~= "table" then
        warn(self, "Nothing wired: drag Fog/Water/Particles/Sound entities into the slots, "
            .. "or name them Fog/Water/Rain/WeatherSound.")
    end
end

return WeatherSystem
