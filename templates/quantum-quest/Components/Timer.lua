-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Repeating method call. Attach it anywhere and drag the entity whose script should be called;
-- category: Scene & HUD
-- other scripts can also start and stop it by holding this entity in a Timer slot.
local Timer = {
    properties = {
        { name = "interval", displayName = "Interval (seconds)", type = "float", default = 1.0 },
        { name = "firstDelay", displayName = "First call delay (seconds)", type = "float", default = -1.0 },
        { name = "times", displayName = "Number of calls (0 = forever)", type = "int", default = 0 },
        { name = "jitter", displayName = "Random interval add-up (seconds)", type = "float", default = 0.0 },
        { name = "autostart", displayName = "Start on scene load", type = "bool", default = true },
        { name = "target", displayName = "Target entity", type = "entity" },
        { name = "method", displayName = "Method to call", type = "string", default = "onTick" }
    }
}

local function callMethod(reference, methodName, timer)
    if type(reference) ~= "table" or not methodName or methodName == "" then
        return false
    end
    local method = reference[methodName]
    if type(method) ~= "function" then
        return false
    end
    method(reference, timer)
    return true
end

function Timer:init()
    RegisterEngineEvent(self, "onUpdate")

    self.interval = math.max(self.interval or 1.0, 0.01)
    self.elapsed = 0.0
    self.calls = 0
    self.waiting = self.firstDelay and self.firstDelay > 0.0 and self.firstDelay or self.interval
    self.running = self.autostart ~= false
    self.warningShown = false
end

function Timer:start()
    self.running = true
end

function Timer:stop()
    self.running = false
end

function Timer:reset()
    self.calls = 0
    self.elapsed = 0.0
    self.waiting = self.interval
end

function Timer:onUpdate()
    if not self.running then
        return
    end

    local limit = self.times or 0
    if limit > 0 and self.calls >= limit then
        self.running = false
        return
    end

    self.elapsed = self.elapsed + math.min(Engine.deltatime, 0.05)
    if self.elapsed < self.waiting then
        return
    end

    self.elapsed = 0.0
    local jitter = self.jitter or 0.0
    self.waiting = self.interval + (jitter > 0.0 and math.random() * jitter or 0.0)

    if not callMethod(self.target, self.method, self.entity) and not self.warningShown then
        self.warningShown = true
        print("[Timer] No method \"" .. tostring(self.method) .. "\" on the target entity.")
    end
    self.calls = self.calls + 1
end

return Timer
