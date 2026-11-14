-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Writes a number or a string into a Text widget: health, ammo, coins, distance. Attach it to the
-- category: Scene & HUD
-- UI entity that holds the Text component and drag the entity that owns the value into the Source
-- slot. The Field name is anything that component publishes (a Health component publishes percent,
-- current and maxHealth) or any method it exposes; other components can push with show() instead.
local HudValue = {
    properties = {
        { name = "source", displayName = "Source", type = "entity" },
        { name = "field", displayName = "Field or method", type = "string", default = "percent" },
        { name = "asPercent", displayName = "Treat the value as 0..1 and show a percentage", type = "bool", default = false },
        { name = "decimals", displayName = "Decimal places", type = "int", default = 0 },
        { name = "prefix", displayName = "Prefix", type = "string", default = "" },
        { name = "suffix", displayName = "Suffix", type = "string", default = "" },
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

function HudValue:init()
    RegisterEngineEvent(self, "onUpdate")

    self.decimals = math.max(self.decimals or 0, 0)
    self.lastText = nil
    self.warningShown = false

    local ok, text = pcall(function()
        return Text(self.scene, self.entity)
    end)
    self.text = (ok and text) or nil

    local okObject, object = pcall(function()
        return Object(self.scene, self.entity)
    end)
    self.object = (okObject and object) or nil
end

function HudValue:format(value)
    if type(value) == "string" then
        return value
    end
    if type(value) ~= "number" then
        return nil
    end
    local number = value
    if self.asPercent == true then
        number = number * 100.0
    end
    return string.format("%." .. self.decimals .. "f", number)
end

-- Called by any script that would rather push a value than publish a field.
function HudValue:show(value)
    local body = self:format(value)
    if not body then
        return
    end
    local text = (self.prefix or "") .. body .. (self.suffix or "")
    if self.text and text ~= self.lastText then
        self.text.text = text
        self.lastText = text
    end
end

function HudValue:onUpdate()
    if not self.text then
        if not self.warningShown then
            self.warningShown = true
            print("[HudValue] This entity has no Text component, so there is nothing to write.")
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
            print("[HudValue] The source does not publish \"" .. tostring(self.field)
                .. "\": a Health component publishes percent, current and maxHealth.")
        end
        return
    end

    if self.object then
        self.object.visible = true
    end
    self:show(value)
end

return HudValue
