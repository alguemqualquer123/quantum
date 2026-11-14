-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Full screen fade, blackout and hit flash: attach it to any entity and it drives an entity holding an
-- category: Screen Effects
-- Image (slot, or the one named "Fade"), falling back to a user post-process float uniform on demand.
local ScreenFade = {
    properties = {
        { name = "image", displayName = "Fade image entity", type = "entity" },
        { name = "fallbackImageName", displayName = "Image entity name if slot is empty", type = "string", default = "Fade" },
        { name = "usePostProcess", displayName = "Use a post-process pass instead", type = "bool", default = false },
        { name = "passIndex", displayName = "Post-process pass (0-based)", type = "int", default = 0 },
        { name = "uniform", displayName = "Uniform name", type = "string", default = "fade" },
        { name = "coverColor", displayName = "Cover colour (r, g, b, a)", type = "color4", default = { 0.0, 0.0, 0.0, 1.0 } },
        { name = "startBlack", displayName = "Start covered", type = "bool", default = false },
        { name = "autoFadeIn", displayName = "Fade in automatically", type = "bool", default = true },
        { name = "fadeInTime", displayName = "Fade in (seconds)", type = "float", default = 0.6 },
        { name = "fadeOutTime", displayName = "Fade out (seconds)", type = "float", default = 0.4 },
        { name = "flashTime", displayName = "Flash time (seconds)", type = "float", default = 0.18 },
        { name = "blockInput", displayName = "Block UI input while covered", type = "bool", default = false },
        { name = "skipKey", displayName = "Skip key (0 = off)", type = "int", default = 0 },
        { name = "onStart", displayName = "On start: entity", type = "entity" },
        { name = "onStartMethod", displayName = "On start: method", type = "string", default = "onFadeStart" },
        { name = "onComplete", displayName = "On complete: entity", type = "entity" },
        { name = "onCompleteMethod", displayName = "On complete: method", type = "string", default = "onFadeComplete" }
    }
}

local COVER, FLASH = { 0.0, 0.0, 0.0, 1.0 }, { 1.0, 0.85, 0.6, 1.0 }

local function num(value, fallback) return type(value) == "number" and value or fallback end

local function clamp(value, low, high)
    local held = num(value, low)
    return held < low and low or (held > high and high or held)
end

-- A color4 property reaches Lua as a vector (x, y, z, w) or, from the properties table, as a plain
-- {r, g, b, a} list; both shapes are read here.
local function colorOf(value, fallback)
    if type(value) == "table" and type(value[1]) == "number" then
        return Vector4(value[1], value[2], value[3], value[4])
    end
    if type(value) == "table" or type(value) == "userdata" then
        return Vector4(num(value.x, fallback[1]), num(value.y, fallback[2]), num(value.z, fallback[3]), num(value.w, fallback[4]))
    end
    return Vector4(fallback[1], fallback[2], fallback[3], fallback[4])
end

-- Health.lua's hook idiom: the slot holds the target script table only when the wired entity really
-- carries that script, so a bare entity handle has no method to call and this returns false quietly.
local function callMethod(reference, methodName, first, second)
    if type(reference) ~= "table" or not methodName or methodName == "" then return false end
    local method = reference[methodName]
    if type(method) ~= "function" then return false end
    return pcall(method, reference, first, second)
end

function ScreenFade:init()
    RegisterEngineEvent(self, "onUpdate")
    self.level = self.startBlack and 1.0 or 0.0
    self.base, self.target, self.from = self.level, self.level, self.level
    self.duration, self.elapsed, self.flashBack = 0.001, 0.0, 0.1
    self.done, self.flashing, self.color = true, false, nil
    self.img, self.uiBlocked = nil, false
    self.autoPending, self.nextLookup = self.startBlack, 0.0
    self.pass = math.max(math.floor(num(self.passIndex, 0)), 0)
end

function ScreenFade:resolveImage()
    if num(Engine.systemTime, 0.0) < self.nextLookup then return end
    self.nextLookup = num(Engine.systemTime, 0.0) + 0.5
    local name = (self.fallbackImageName and self.fallbackImageName ~= "") and self.fallbackImageName or "Fade"
    local ok, found = pcall(function() return self.scene:findEntity(name) end)
    local slot = self.image
    local id = tonumber(slot) or ((type(slot) == "table" or type(slot) == "userdata") and tonumber(slot.entity)) or 0
    if id <= 0 then id = ok and num(found, 0) or 0 end
    if id <= 0 then return end
    local built, handle = pcall(function() return Image(self.scene, id) end)
    if built and handle then self.img = handle end
end

-- The user post-process chain carries float uniforms only, so the whole fade is a single 0..1 value.
-- setPostProcessUniform returns nothing, and the engine logs an (ERROR) line every call against a
-- pass the scene does not have, so the pass list is consulted instead of the write's outcome.
function ScreenFade:passAvailable()
    local now = num(Engine.systemTime, 0.0)
    if self.passAt and now - self.passAt < 0.5 then return self.passOk end
    self.passAt = now
    local ok, count = pcall(function() return #self.scene.postProcessPasses end)
    self.passOk = ok and type(count) == "number" and count > num(self.pass, 0)
    return self.passOk
end

function ScreenFade:apply()
    local level = clamp(self.level, 0.0, 1.0)
    local wired = false
    if self.img == nil and not self.usePostProcess then self:resolveImage() end
    if self.img then
        self.img.color = colorOf(self.color or self.coverColor, self.color and FLASH or COVER)
        self.img.alpha = level
        self.img.visible = level > 0.001
        wired = true
    elseif self.usePostProcess and self:passAvailable() then
        local setter = self.scene.setPostProcessUniform
        wired = type(setter) == "function" and pcall(setter, self.scene, self.pass, self.uniform or "fade", level)
    end
    -- Nothing here changes by itself, so the setup miss is stated once instead of on a timer: a fade
    -- wired to no target stays unwired until someone edits the scene.
    if not wired and not self.warnedUnwired then
        self.warnedUnwired = true
        print("[ScreenFade] wire an image entity, or name a post-process pass the scene really has " ..
            "(postProcessPasses)")
    end
    local covered = level > 0.998
    if self.blockInput and covered ~= self.uiBlocked then
        local gate = self.scene.setEnableUIEvents
        if type(gate) == "function" and pcall(gate, self.scene, not covered) then self.uiBlocked = covered end
    end
end

function ScreenFade:_start(target, duration, flashing)
    self.from, self.target = self.level, clamp(num(target, self.level), 0.0, 1.0)
    self.duration, self.elapsed, self.done = math.max(num(duration, 0.3), 0.001), 0.0, false
    self.flashing = flashing == true
    if not flashing then self.base, self.color = self.target, nil end
    callMethod(self.onStart, self.onStartMethod, self.target, self.entity)
end

function ScreenFade:fadeIn(seconds) self:_start(0.0, num(seconds, self.fadeInTime)) end
function ScreenFade:fadeOut(seconds) self:_start(1.0, num(seconds, self.fadeOutTime)) end
function ScreenFade:isDone() return self.done == true end

function ScreenFade:flashTo(color, alpha, seconds)
    local time = math.max(num(seconds, self.flashTime), 0.02)
    self.color, self.flashBack = color, time * 0.65
    self:_start(clamp(num(alpha, 0.6), 0.0, 1.0), time * 0.35, true)
end

function ScreenFade:setLevel(value)
    self.level = clamp(num(value, self.level), 0.0, 1.0)
    self.base, self.target, self.done = self.level, self.level, true
    self.flashing, self.color, self.autoPending = false, nil, false
    self:apply()
end

function ScreenFade:complete()
    self.done, self.flashing, self.color = true, false, nil
    callMethod(self.onComplete, self.onCompleteMethod, self.level, self.entity)
end

function ScreenFade:skip()
    self.level, self.base, self.target = self.base, self.base, self.base
    self:complete()
end

function ScreenFade:onUpdate()
    if self.autoPending then
        self.autoPending = false
        if self.autoFadeIn then self:fadeIn(self.fadeInTime) end
    end
    local skipKey = num(self.skipKey, 0)
    if skipKey > 0 and not self.done and Input.isKeyPressed(skipKey) then
        self:skip()
        return
    end
    if not self.done then
        self.elapsed = self.elapsed + math.min(num(Engine.deltatime, 1.0 / 60.0), 0.05)
        local t = clamp(self.elapsed / self.duration, 0.0, 1.0)
        self.level = self.from + (self.target - self.from) * t
        if t >= 1.0 then if self.flashing then self:_start(self.base, self.flashBack) else self:complete() end end
    end
    self:apply()
end

return ScreenFade
