-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Floating bob and sway for pickups, power cores, anything that should look weightless. Attach it
-- category: Scene & HUD
-- to an entity with a Transform; the position it starts at becomes the resting point.
local Bobber = {
    properties = {
        { name = "amplitude", displayName = "Vertical amplitude", type = "float", default = 0.35 },
        { name = "period", displayName = "Period (seconds)", type = "float", default = 2.0 },
        { name = "phase", displayName = "Phase offset (0..1)", type = "float", default = 0.0 },
        { name = "swayAmplitude", displayName = "Horizontal sway", type = "float", default = 0.0 },
        { name = "swayAxis", displayName = "Sway axis (0 = X, 1 = Z)", type = "int", default = 0 },
        { name = "swayPhase", displayName = "Sway phase (0..1)", type = "float", default = 0.25 }
    }
}

function Bobber:init()
    RegisterEngineEvent(self, "onUpdate")

    self.amplitude = self.amplitude or 0.35
    self.period = math.max(self.period or 2.0, 0.05)

    self.warningShown = false
    self.elapsed = 0.0

    local ok, object = pcall(function()
        return Object(self.scene, self.entity)
    end)
    self.object = (ok and object) or nil

    if self.object then
        local position = self.object.position
        self.baseX, self.baseY, self.baseZ = position.x, position.y, position.z
    end
end

function Bobber:onUpdate()
    if not self.object then
        if not self.warningShown then
            self.warningShown = true
            print("[Bobber] The entity needs a Transform component.")
        end
        return
    end

    self.elapsed = self.elapsed + math.min(Engine.deltatime, 0.05)
    local t = self.elapsed / self.period + (self.phase or 0.0)

    local position = self.object.position
    position.y = self.baseY + math.sin(t * math.pi * 2.0) * self.amplitude

    local sway = (self.swayAmplitude or 0.0)
    if sway ~= 0.0 then
        local offset = math.sin((t + (self.swayPhase or 0.0)) * math.pi * 2.0) * sway
        if (self.swayAxis or 0) == 1 then
            position.z = self.baseZ + offset
        else
            position.x = self.baseX + offset
        end
    end

    self.object.position = position
end

return Bobber
