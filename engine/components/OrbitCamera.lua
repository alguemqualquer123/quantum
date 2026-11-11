-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Turntable camera for a showcase, a character select screen or a menu. Attach it to the entity
-- category: Camera & Controllers
-- that holds the Camera component and drag whatever it should frame into the Target slot.
local OrbitCamera = {
    properties = {
        { name = "target", displayName = "Target", type = "entity" },
        { name = "fallbackName", displayName = "Target name if slot is empty", type = "string", default = "Player" },
        { name = "radius", displayName = "Radius", type = "float", default = 5.0 },
        { name = "height", displayName = "Height above the target", type = "float", default = 1.5 },
        { name = "lookHeight", displayName = "Look height", type = "float", default = 1.0 },
        { name = "autoSpin", displayName = "Auto spin (degrees per second)", type = "float", default = 20.0 },
        { name = "mouseLook", displayName = "Orbit with mouse", type = "bool", default = true },
        { name = "sensitivity", displayName = "Mouse sensitivity", type = "float", default = 0.25 },
        { name = "minPitch", displayName = "Minimum pitch (degrees)", type = "float", default = -10.0 },
        { name = "maxPitch", displayName = "Maximum pitch (degrees)", type = "float", default = 60.0 },
        { name = "smoothing", displayName = "Zoom smoothing", type = "float", default = 6.0 },
        { name = "zoomSpeed", displayName = "Scroll zoom speed", type = "float", default = 1.0 },
        { name = "shake", displayName = "Shake", type = "CameraShake" }
    }
}

local function clamp(value, low, high)
    if value < low then return low end
    if value > high then return high end
    return value
end

local function entityPosition(scene, reference)
    local id = nil
    if type(reference) == "number" then
        id = reference
    elseif reference and type(reference.entity) == "number" then
        id = reference.entity
    end
    if id == 0 then
        id = nil
    end
    if not id then
        return nil
    end
    local ok, position = pcall(function()
        return Object(scene, id).position
    end)
    if ok and position then
        return position
    end
    return nil
end

function OrbitCamera:init()
    RegisterEngineEvent(self, "onUpdate")

    self.radius = self.radius or 5.0
    self.height = self.height or 1.5
    self.lookHeight = self.lookHeight or 1.0
    self.autoSpin = self.autoSpin or 20.0
    self.sensitivity = self.sensitivity or 0.25
    self.minPitch = self.minPitch or -10.0
    self.maxPitch = self.maxPitch or 60.0
    self.smoothing = self.smoothing or 6.0
    self.zoomSpeed = self.zoomSpeed or 1.0

    self.yaw = 0.0
    self.pitch = 20.0
    self.currentRadius = self.radius
    self.previousMouse, self.lastWarn = nil, 0.0
    self.previousScroll = Input.getMouseScroll().y
end

function OrbitCamera:onUpdate()
    local ok, camera = pcall(function()
        return Camera(self.scene, self.entity)
    end)
    if not ok or not camera then
        return
    end

    local point = entityPosition(self.scene, self.target)
    if not point and self.fallbackName and self.fallbackName ~= "" then
        point = entityPosition(self.scene, self.scene:findEntity(self.fallbackName))
    end
    if not point then
        return
    end

    local dt = math.min(Engine.deltatime, 0.05)
    self.yaw = self.yaw + self.autoSpin * dt

    if self.mouseLook then
        local mouse = Input.getMousePosition()
        if Input.isMousePressed(Input.MOUSE_BUTTON_LEFT) then
            if self.previousMouse then
                self.yaw = self.yaw - (mouse.x - self.previousMouse.x) * self.sensitivity
                self.pitch = clamp(self.pitch + (mouse.y - self.previousMouse.y) * self.sensitivity,
                    self.minPitch, self.maxPitch)
            end
        elseif Input.isMousePressed(Input.MOUSE_BUTTON_RIGHT) then
            if self.previousMouse then
                self.yaw = self.yaw - (mouse.x - self.previousMouse.x) * self.sensitivity * 0.5
                self.height = self.height + (mouse.y - self.previousMouse.y) * self.sensitivity * 0.1
            end
        end
        self.previousMouse = {x = mouse.x, y = mouse.y}

        -- Input.getMouseScroll() is the last wheel event and the engine never clears it, so
        -- zoom has to react to the change since the previous frame or it runs away.
        local scroll = Input.getMouseScroll()
        local delta = scroll.y - self.previousScroll
        self.previousScroll = scroll.y
        if delta ~= 0 then
            self.radius = clamp(self.radius - delta * self.zoomSpeed, 0.5, 200.0)
        end
    end

    -- A single damped radius so a scroll jump never teleports the camera.
    self.currentRadius = self.currentRadius + (self.radius - self.currentRadius)
        * (1.0 - math.exp(-self.smoothing * dt))

    local yawRadians = math.rad(self.yaw)
    local pitchRadians = math.rad(clamp(self.pitch, self.minPitch, self.maxPitch))
    local horizontal = self.currentRadius * math.cos(pitchRadians)
    local focus = Vector3(point.x, point.y + self.lookHeight, point.z)
    local desired = Vector3(
        focus.x + math.sin(yawRadians) * horizontal,
        focus.y + self.height + self.currentRadius * math.sin(pitchRadians),
        focus.z + math.cos(yawRadians) * horizontal
    )

    -- An entity handle in the Shake slot carries no offset, so the shake would vanish without a
    -- trace: only a script table publishes one, and the miswire is named once a second.
    local shake = self.shake
    if type(shake) == "table" and shake.offset then
        local offset = shake.offset
        focus = Vector3(focus.x + offset.x * 0.4, focus.y + offset.y * 0.4, focus.z + offset.z * 0.4)
        desired = Vector3(desired.x + offset.x, desired.y + offset.y, desired.z + offset.z)
    elseif type(shake) == "userdata" and Engine.systemTime - self.lastWarn > 1.0 then
        self.lastWarn = Engine.systemTime
        print("[OrbitCamera] Shake ignored: the Shake slot holds an entity without a CameraShake component")
    end

    camera.position = desired
    camera.target = focus
end

return OrbitCamera
