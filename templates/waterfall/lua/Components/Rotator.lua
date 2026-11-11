-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Constant spin, the classic rotating pickup. Attach it to any entity with a Transform.
-- category: Scene & HUD
local Rotator = {
    properties = {
        { name = "speedX", displayName = "Spin around X (degrees/s)", type = "float", default = 0.0 },
        { name = "speedY", displayName = "Spin around Y (degrees/s)", type = "float", default = 90.0 },
        { name = "speedZ", displayName = "Spin around Z (degrees/s)", type = "float", default = 0.0 },
        { name = "startX", displayName = "Initial angle X (degrees)", type = "float", default = 0.0 },
        { name = "startY", displayName = "Initial angle Y (degrees)", type = "float", default = 0.0 },
        { name = "startZ", displayName = "Initial angle Z (degrees)", type = "float", default = 0.0 }
    }
}

local function angle(degrees)
    if Engine.useDegrees then
        return degrees
    end
    return math.rad(degrees)
end

function Rotator:init()
    RegisterEngineEvent(self, "onUpdate")

    self.angleX = self.startX or 0.0
    self.angleY = self.startY or 0.0
    self.angleZ = self.startZ or 0.0
    self.warningShown = false
end

function Rotator:onUpdate()
    local ok, object = pcall(function()
        return Object(self.scene, self.entity)
    end)
    if not ok or not object then
        if not self.warningShown then
            self.warningShown = true
            print("[Rotator] The entity needs a Transform component.")
        end
        return
    end

    local dt = math.min(Engine.deltatime, 0.05)
    self.angleX = self.angleX + (self.speedX or 0.0) * dt
    self.angleY = self.angleY + (self.speedY or 0.0) * dt
    self.angleZ = self.angleZ + (self.speedZ or 0.0) * dt

    -- Lua has no bound quaternion product, so the spin is accumulated as authored euler angles
    -- and re-applied whole; the order is the engine default (ZYX).
    object.rotation = Quaternion(angle(self.angleX), angle(self.angleY), angle(self.angleZ))
end

return Rotator
