-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Third person follow camera. Attach it to the entity that holds the Camera component and drag
-- category: Camera & Controllers
-- the character into the Target slot; nothing else is required.
local FollowCamera = {
    properties = {
        -- Optional: without a slot this script drives its own entity, then the active camera.
        { name = "camera", displayName = "Camera (empty = this entity)", type = "camera" },
        { name = "fallbackCameraName", displayName = "Camera name if slot is empty", type = "string", default = "Camera" },
        { name = "target", displayName = "Target", type = "entity" },
        { name = "fallbackName", displayName = "Target name if slot is empty", type = "string", default = "Player" },
        { name = "distance", displayName = "Distance", type = "float", default = 6.0 },
        { name = "height", displayName = "Height", type = "float", default = 2.2 },
        { name = "lookHeight", displayName = "Look height", type = "float", default = 1.4 },
        { name = "smoothing", displayName = "Smoothing", type = "float", default = 8.0 },
        { name = "mouseLook", displayName = "Orbit with mouse", type = "bool", default = true },
        { name = "orbitButton", displayName = "Orbit mouse button (0 = right)", type = "int", default = 0 },
        { name = "sensitivity", displayName = "Mouse sensitivity", type = "float", default = 0.15 },
        -- Capture keeps the pointer inside the window and reads it as motion, which is what a
        -- game wants; off, the orbit only runs while the mouse button is held.
        { name = "freeLook", displayName = "Capture the mouse (key toggles)", type = "bool", default = false },
        { name = "captureKey", displayName = "Capture toggle key (0 = TAB)", type = "int", default = 0 },
        { name = "minPitch", displayName = "Minimum pitch (degrees)", type = "float", default = -20.0 },
        { name = "maxPitch", displayName = "Maximum pitch (degrees)", type = "float", default = 70.0 },
        { name = "yaw", displayName = "Yaw (degrees)", type = "float", default = 0.0 },
        { name = "pitch", displayName = "Pitch (degrees)", type = "float", default = 12.0 },
        -- A typed slot: the engine hands over the CameraShake script instance of the entity
        -- dropped here, so the shake offsets can be folded into this pose.
        { name = "shake", displayName = "Shake", type = "CameraShake" }
    }
}

local function clamp(value, low, high)
    if value < low then return low end
    if value > high then return high end
    return value
end

-- An entity slot reaches Lua either as the target's script table or as an engine handle; both
-- carry the entity id in `.entity`, and a hand-authored value may already be the bare id.
-- A slot reaches Lua as a script table, as an engine handle, or as the bare id the
-- inspector stored, and 0 is NULL_ENTITY: normalise all three shapes to one.
local function entityId(reference)
    if type(reference) == "number" then
        if reference == 0 then return nil end
        return reference
    end
    if reference and type(reference.entity) == "number" then
        if reference.entity == 0 then return nil end
        return reference.entity
    end
    return nil
end

-- 0 is GLFW's unknown key, so an untouched int property can fall back to the documented default.
local function defaultKey(value, fallback)
    if not value or value == 0 then
        return fallback
    end
    return value
end

-- Camera(scene, entity) is only a handle, so attaching this script to an entity without a
-- CameraComponent does not fail until a property is written - which is every frame. Reading a
-- camera-only property is the only component test Lua has, and one probe keeps the engine's
-- "non-existent component" error to a single line instead of one per frame.
local function cameraOf(scene, id)
    if not id then
        return nil
    end

    local ok, camera = pcall(function()
        local handle = Camera(scene, id)
        if handle.yfov == nil then
            error("no camera component")
        end
        return handle
    end)

    if not ok then
        return nil
    end
    return camera
end

function FollowCamera:init()
    RegisterEngineEvent(self, "onUpdate")

    self.distance = self.distance or 6.0
    self.height = self.height or 2.2
    self.lookHeight = self.lookHeight or 1.4
    self.smoothing = self.smoothing or 8.0
    self.sensitivity = self.sensitivity or 0.15
    self.minPitch = self.minPitch or -20.0
    self.maxPitch = self.maxPitch or 70.0
    self.yaw = self.yaw or 0.0
    self.pitch = self.pitch or 12.0
    -- 0 is GLFW's unknown key, so an untouched property can default to the right mouse button.
    if not self.orbitButton or self.orbitButton == 0 then
        self.orbitButton = Input.MOUSE_BUTTON_RIGHT
    end
    self.captureKey = defaultKey(self.captureKey, Input.KEY_TAB)

    self.previousMouse = nil
    self.lastWarn = 0.0
    self.captured = false
    self.captureDown = false
    self.captureWarned = false

    -- Resolved once: rewiring the slots means selecting the script again, which re-runs init.
    self.cameraHandle = cameraOf(self.scene, entityId(self.camera))
    if not self.cameraHandle then
        self.cameraHandle = cameraOf(self.scene, self.entity)
    end
    if not self.cameraHandle then
        self.cameraHandle = cameraOf(self.scene, entityId(self.scene.camera))
    end
    if not self.cameraHandle and self.fallbackCameraName and self.fallbackCameraName ~= "" then
        self.cameraHandle = cameraOf(self.scene, entityId(self.scene:findEntity(self.fallbackCameraName)))
    end

    if not self.cameraHandle then
        print("[FollowCamera] No Camera component to drive: attach this script to the camera entity, "
            .. "drag it into the Camera slot, or name it in \"Camera name if slot is empty\"")
    end
end

function FollowCamera:resolveTarget()
    local id = entityId(self.target)
    if id then
        return id
    end
    if self.fallbackName and self.fallbackName ~= "" then
        return entityId(self.scene:findEntity(self.fallbackName))
    end
    return nil
end

-- Capturing hides the pointer and clips it to the window, after which the platform router feeds
-- Input.getMousePosition from raw motion instead of absolute coordinates. Not every backend can
-- do it, so the failure is named once and the button orbit stays available.
function FollowCamera:setCaptured(on)
    local ok = pcall(function()
        Engine.mouseMode = on and MouseMode.CAPTURED or MouseMode.NORMAL
    end)

    if not ok then
        if not self.captureWarned then
            self.captureWarned = true
            print("[FollowCamera] Mouse capture unavailable: orbit with the mouse button instead")
        end
        return
    end

    self.captured = on
    self.previousMouse = nil
    print(on and "[FollowCamera] Mouse captured: move to look, the toggle key releases it"
        or "[FollowCamera] Mouse released")
end

function FollowCamera:onUpdate()
    local camera = self.cameraHandle
    if not camera then
        return
    end

    if self.freeLook then
        local pressed = Input.isKeyPressed(self.captureKey)
        if pressed and not self.captureDown then
            self:setCaptured(not self.captured)
        end
        self.captureDown = pressed
    end

    local id = self:resolveTarget()
    if not id then
        return
    end

    local okPoint, point = pcall(function()
        return Object(self.scene, id).position
    end)
    if not okPoint or not point then
        -- Warn at most once a second: an unwired slot is a setup mistake, not a per-frame error.
        if Engine.systemTime - self.lastWarn > 1.0 then
            self.lastWarn = Engine.systemTime
            print("[FollowCamera] Target invalid: drag an entity into the Target slot or name it "
                .. tostring(self.fallbackName))
        end
        return
    end

    -- The pointer is sampled every frame so a delta is never worth more than one frame of
    -- motion: comparing only on orbit frames would throw the whole accumulated travel into the
    -- first frame after a click.
    local mouse = Input.getMousePosition()
    local deltaX, deltaY = 0.0, 0.0
    if self.previousMouse then
        deltaX = mouse.x - self.previousMouse.x
        deltaY = mouse.y - self.previousMouse.y
    end
    self.previousMouse = {x = mouse.x, y = mouse.y}

    if self.mouseLook ~= false and (self.captured or Input.isMousePressed(self.orbitButton)) then
        self.yaw = self.yaw - deltaX * self.sensitivity
        self.pitch = clamp(self.pitch + deltaY * self.sensitivity, self.minPitch, self.maxPitch)
    end

    local yawRadians = math.rad(self.yaw)
    local pitchRadians = math.rad(clamp(self.pitch, self.minPitch, self.maxPitch))
    local horizontal = self.distance * math.cos(pitchRadians)

    local focus = Vector3(point.x, point.y + self.lookHeight, point.z)
    local desired = Vector3(
        focus.x + math.sin(yawRadians) * horizontal,
        focus.y + self.height + self.distance * math.sin(pitchRadians),
        focus.z + math.cos(yawRadians) * horizontal
    )

    -- The shake offset rides on top of the pose; smoothing then softens it into a sway rather
    -- than a jitter, which is what a follow camera wants. An entity handle in that slot carries no
    -- offset, so the shake would vanish without a trace: the miswire is named once a second.
    local shake = self.shake
    if type(shake) == "table" and shake.offset then
        local offset = shake.offset
        focus = Vector3(focus.x + offset.x * 0.4, focus.y + offset.y * 0.4, focus.z + offset.z * 0.4)
        desired = Vector3(desired.x + offset.x, desired.y + offset.y, desired.z + offset.z)
    elseif type(shake) == "userdata" and Engine.systemTime - self.lastWarn > 1.0 then
        self.lastWarn = Engine.systemTime
        print("[FollowCamera] Shake ignored: the Shake slot holds an entity without a CameraShake component")
    end

    local dt = math.min(Engine.deltatime, 0.05)
    local alpha = 1.0 - math.exp(-self.smoothing * dt)
    local current = camera.position

    camera.position = Vector3(
        current.x + (desired.x - current.x) * alpha,
        current.y + (desired.y - current.y) * alpha,
        current.z + (desired.z - current.z) * alpha
    )
    camera.target = focus
end

return FollowCamera
