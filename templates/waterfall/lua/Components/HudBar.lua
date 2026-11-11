-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Fills a Progressbar widget from a value owned by another entity: health, stamina, reload, boss
-- category: Scene & HUD
-- life. Attach it to the UI entity that holds the Progressbar component, drag the owner into the
-- Source slot and name the field it publishes (a Health component publishes percent).
local HudBar = {
    properties = {
        { name = "source", displayName = "Source", type = "entity" },
        { name = "field", displayName = "Field or method", type = "string", default = "percent" },
        { name = "minValue", displayName = "Value shown empty", type = "float", default = 0.0 },
        { name = "maxValue", displayName = "Value shown full", type = "float", default = 1.0 },
        { name = "smoothing", displayName = "Fill speed (0 = instant)", type = "float", default = 0.0 },
        { name = "invert", displayName = "Fill from the other side", type = "bool", default = false },
        { name = "hideWhenEmpty", displayName = "Hide when the source has no such field", type = "bool", default = false }
    }
}

-- A slot holds the target entity's first enabled script only when that entity has one, so the value
-- is read straight off that table; a bare entity handle publishes nothing and the field is nil. A
-- method name works the same way, which keeps getPercent() and friends usable.
local function readField(source, field)
    if type(source) ~= "table" or not field or field == "" then
        return nil
    end
    local value = source[field]
    if type(value) == "function" then
        local ok, result = pcall(function()
            return value(source)
        end)
        return ok and result or nil
    end
    return value
end

local function clamp(value, low, high)
    if value < low then return low end
    if value > high then return high end
    return value
end

function HudBar:init()
    RegisterEngineEvent(self, "onUpdate")

    self.minValue = self.minValue or 0.0
    self.maxValue = self.maxValue or 1.0
    if self.maxValue <= self.minValue then
        self.maxValue = self.minValue + 1.0
    end

    self.current = 0.0
    self.started = false
    self.warningShown = false

    local ok, bar = pcall(function()
        return Progressbar(self.scene, self.entity)
    end)
    self.bar = (ok and bar) or nil

    local okObject, object = pcall(function()
        return Object(self.scene, self.entity)
    end)
    self.object = (okObject and object) or nil
end

local function ratio(value, low, high)
    return clamp((value - low) / (high - low), 0.0, 1.0)
end

-- Called by any script that would rather push a value than publish a field.
function HudBar:show(value)
    if type(value) ~= "number" then
        return
    end

    local wanted = ratio(value, self.minValue, self.maxValue)
    if self.invert == true then
        wanted = 1.0 - wanted
    end

    local speed = self.smoothing or 0.0
    if speed <= 0.0 or not self.started then
        self.current = wanted
        self.started = true
    else
        -- Frame-rate independent, so a bar drains at the same speed on any machine.
        local dt = math.min(Engine.deltatime, 0.05)
        local alpha = 1.0 - math.exp(-speed * dt)
        self.current = self.current + (wanted - self.current) * alpha
    end

    if self.bar then
        self.bar.value = self.current
    end
end

function HudBar:onUpdate()
    if not self.bar then
        if not self.warningShown then
            self.warningShown = true
            print("[HudBar] This entity has no Progressbar component, so there is nothing to fill.")
        end
        return
    end

    local value = readField(self.source, self.field)
    if value == nil then
        if self.object then
            self.object.visible = self.hideWhenEmpty ~= true
        end
        if not self.warningShown then
            self.warningShown = true
            print("[HudBar] The source does not publish \"" .. tostring(self.field)
                .. "\": a Health component publishes percent, current and maxHealth.")
        end
        return
    end

    if self.object then
        self.object.visible = true
    end
    self:show(value)
end

return HudBar
