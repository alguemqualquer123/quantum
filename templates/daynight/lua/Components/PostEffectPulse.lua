-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Breathes a user post-process float uniform (vignette, tint, distortion) on a curve, or drives it
-- category: Screen Effects
-- from a Health entity so the effect strengthens as health drops. Attach it to any entity.
--
-- The shipped chain takes one float per uniform call and no new passes: no HDR, no bloom from Lua.
local PostEffectPulse = {
    properties = {
        { name = "passIndex", displayName = "Post-process pass (0-based)", type = "int", default = 0 },
        { name = "uniform", displayName = "Uniform name", type = "string", default = "intensity" },
        { name = "from", displayName = "Value at level 0", type = "float", default = 0.0 },
        { name = "to", displayName = "Value at level 1", type = "float", default = 1.0 },
        { name = "speed", displayName = "Cycles per second", type = "float", default = 0.5 },
        { name = "curve", displayName = "Curve (0 linear, 1 sine, 2 ease)", type = "int", default = 1 },
        { name = "auto", displayName = "Run continuously", type = "bool", default = true },
        { name = "syncFromScene", displayName = "Read the uniform back once on start", type = "bool", default = false },
        { name = "secondUniform", displayName = "Second uniform (blank = off)", type = "string", default = "" },
        { name = "secondFrom", displayName = "Second value at level 0", type = "float", default = 0.0 },
        { name = "secondTo", displayName = "Second value at level 1", type = "float", default = 0.5 },
        { name = "health", displayName = "Health source", type = "Health" },
        { name = "followHealth", displayName = "Follow health instead of animating", type = "bool", default = false },
        { name = "lowHealthThreshold", displayName = "Critical health threshold (0..1)", type = "float", default = 0.3 },
        { name = "onCritical", displayName = "On critical: entity", type = "entity" },
        { name = "onCriticalMethod", displayName = "On critical: method", type = "string", default = "onCritical" }
    }
}

local function num(value, fallback) return type(value) == "number" and value or fallback end

-- Every numeric property can arrive unwired or non-numeric, so the clamp takes the low bound for nil.
local function clamp(value, low, high)
    local held = num(value, low)
    return held < low and low or (held > high and high or held)
end

-- Health.lua's hook idiom: the slot holds the target script table only when the wired entity really
-- carries that script, so a bare entity handle finds no method here and returns false quietly.
local function callMethod(reference, methodName, first, second)
    if type(reference) ~= "table" or not methodName or methodName == "" then return false end
    local method = reference[methodName]
    if type(method) ~= "function" then return false end
    return pcall(method, reference, first, second)
end

-- All three map a 0..1 phase onto a 0..1 level: 0 triangle, 1 sine, 2 eased triangle.
local function shape(kind, phase)
    local triangle = 1.0 - math.abs(2.0 * phase - 1.0)
    if kind == 0 then return triangle end
    if kind == 2 then return triangle * triangle * (3.0 - 2.0 * triangle) end
    return 0.5 - 0.5 * math.cos(phase * math.pi * 2.0)
end

function PostEffectPulse:init()
    RegisterEngineEvent(self, "onUpdate")
    self.passIndex = math.max(math.floor(num(self.passIndex, 0)), 0)
    self.uniform = (self.uniform and self.uniform ~= "") and self.uniform or "intensity"
    self.speed = clamp(num(self.speed, 0.5), 0.0, 20.0)
    self.curve = math.floor(clamp(num(self.curve, 1), 0, 2))
    self.level, self.running = 0.5, self.auto ~= false
    self.once, self.elapsed, self.critical, self.lastWarn = 0.0, 0.0, false, -1.0
    if self.followHealth then self.running = false end

    local getter = self.scene.getPostProcessUniform
    if self.syncFromScene and type(getter) == "function" then
        local ok, value = pcall(getter, self.scene, self.passIndex, self.uniform)
        -- The binding answers with a Vector4 whose x channel carries the float written in.
        if ok and (type(value) == "userdata" or type(value) == "table") then
            self.level = clamp(num(value.x, self.level), 0.0, 1.0)
        end
    end
end

function PostEffectPulse:write(name, value)
    local setter = self.scene.setPostProcessUniform
    return type(setter) == "function" and pcall(setter, self.scene, self.passIndex, name, value)
end

function PostEffectPulse:apply()
    local low, high = num(self.from, 0.0), num(self.to, 1.0)
    local ok = self:write(self.uniform, low + (high - low) * self.level)
    if self.secondUniform and self.secondUniform ~= "" then
        local secondLow, secondHigh = num(self.secondFrom, 0.0), num(self.secondTo, 0.5)
        ok = self:write(self.secondUniform, secondLow + (secondHigh - secondLow) * self.level) and ok
    end
    return ok
end

function PostEffectPulse:note(text)
    local time = num(Engine.systemTime, 0.0)
    if time - self.lastWarn < 1.0 then return end
    self.lastWarn = time
    print("[PostEffectPulse] " .. text)
end

function PostEffectPulse:setLevel(value)
    self.level = clamp(num(value, self.level), 0.0, 1.0)
    self:apply()
end

function PostEffectPulse:setTarget(fromValue, toValue)
    self.from, self.to = num(fromValue, self.from), num(toValue, self.to)
    self:apply()
end

-- One-shot ramp up and back down for anything that wants a jolt without owning a timer.
function PostEffectPulse:pulse(seconds) self.running, self.once, self.elapsed = true, math.max(num(seconds, 0.4), 0.02), 0.0 end

function PostEffectPulse:stop() self.running, self.once = false, 0.0 end
function PostEffectPulse:getLevel() return self.level end

-- A Health instance publishes percent; getPercent() is the fallback for a script with only a method.
function PostEffectPulse:readHealth()
    if type(self.health) ~= "table" then return nil end
    local percent = num(self.health.percent, nil)
    if percent == nil and type(self.health.getPercent) == "function" then
        local ok, value = pcall(self.health.getPercent, self.health)
        percent = ok and num(value, nil) or nil
    end
    return percent
end

function PostEffectPulse:follow()
    local percent = self:readHealth()
    if percent == nil then
        self:note("wire a Health entity or turn off follow health")
        return false
    end
    self.level = clamp(1.0 - percent, 0.0, 1.0)
    local threshold = clamp(num(self.lowHealthThreshold, 0.3), 0.0, 1.0)
    if percent <= threshold and not self.critical then
        self.critical = true
        callMethod(self.onCritical, self.onCriticalMethod, percent, self.entity)
    elseif percent > threshold + 0.05 then
        self.critical = false
    end
    return true
end

function PostEffectPulse:onUpdate()
    if self.followHealth then
        if not self:follow() then return end
    elseif not self.running then
        return
    elseif self.once > 0.0 then
        self.elapsed = self.elapsed + math.min(num(Engine.deltatime, 1.0 / 60.0), 0.05)
        local phase = clamp(self.elapsed / self.once, 0.0, 1.0)
        self.level = math.sin(phase * math.pi)
        if phase >= 1.0 then self.once, self.running, self.level = 0.0, self.auto ~= false, 0.0 end
    else
        local cycles = num(Engine.systemTime, 0.0) * self.speed
        self.level = shape(self.curve, cycles - math.floor(cycles))
    end
    if not self:apply() then
        self:note("pass " .. tostring(self.passIndex) .. " rejects uniform " .. tostring(self.uniform))
    end
end

return PostEffectPulse
