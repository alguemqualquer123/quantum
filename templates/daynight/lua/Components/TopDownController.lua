-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Top down mover: WASD or arrows drive the entity across the ground plane and it turns to face
-- category: Camera & Controllers
-- the way it moves. Attach it to the entity; a camera slot is optional and only changes the
-- direction the keys mean.
local TopDownController = {
    properties = {
        { name = "speed", displayName = "Speed (m/s)", type = "float", default = 4.0 },
        { name = "sprintMultiplier", displayName = "Sprint multiplier", type = "float", default = 1.6 },
        { name = "acceleration", displayName = "Acceleration (1/s)", type = "float", default = 12.0 },
        { name = "rotateSpeed", displayName = "Turn speed (degrees/s)", type = "float", default = 540.0 },
        { name = "faceMovement", displayName = "Face the movement", type = "bool", default = true },
        { name = "relativeToCamera", displayName = "Keys relative to camera", type = "bool", default = false },
        { name = "camera", displayName = "Reference camera", type = "camera" },
        { name = "keepY", displayName = "Keep the same height", type = "bool", default = true },
        { name = "fixedY", displayName = "Height used when keeping Y is off", type = "float", default = 0.0 },
        { name = "keyForward", displayName = "Forward key (0 = W)", type = "int", default = 0 },
        { name = "keyBack", displayName = "Back key (0 = S)", type = "int", default = 0 },
        { name = "keyLeft", displayName = "Left key (0 = A)", type = "int", default = 0 },
        { name = "keyRight", displayName = "Right key (0 = D)", type = "int", default = 0 },
        { name = "keySprint", displayName = "Sprint key (0 = left shift)", type = "int", default = 0 }
    }
}

local function clamp(value, low, high)
    if value < low then return low end
    if value > high then return high end
    return value
end

local function defaultKey(value, fallback)
    if not value or value == 0 then
        return fallback
    end
    return value
end

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

local function angle(degrees)
    if Engine.useDegrees then
        return degrees
    end
    return math.rad(degrees)
end

local function shortestArc(from, to)
    local delta = (to - from) % 360.0
    if delta > 180.0 then
        delta = delta - 360.0
    end
    return delta
end

function TopDownController:init()
    RegisterEngineEvent(self, "onUpdate")

    self.speed = self.speed or 4.0
    self.sprintMultiplier = self.sprintMultiplier or 1.6
    self.acceleration = self.acceleration or 12.0
    self.rotateSpeed = self.rotateSpeed or 540.0

    self.keyForward = defaultKey(self.keyForward, Input.KEY_W)
    self.keyBack = defaultKey(self.keyBack, Input.KEY_S)
    self.keyLeft = defaultKey(self.keyLeft, Input.KEY_A)
    self.keyRight = defaultKey(self.keyRight, Input.KEY_D)
    self.keySprint = defaultKey(self.keySprint, Input.KEY_LEFT_SHIFT)

    self.velocityX = 0.0
    self.velocityZ = 0.0
    self.currentYaw = 0.0
    self.targetYaw = 0.0
    self.warningShown = false
end

function TopDownController:onUpdate()
    local okObject, object = pcall(function()
        return Object(self.scene, self.entity)
    end)
    if not okObject or not object then
        if not self.warningShown then
            self.warningShown = true
            print("[TopDownController] The entity needs a Transform component.")
        end
        return
    end

    local dt = math.min(Engine.deltatime, 0.05)

    local x, z = 0.0, 0.0
    if Input.isKeyPressed(self.keyForward) then z = z - 1.0 end
    if Input.isKeyPressed(self.keyBack) then z = z + 1.0 end
    if Input.isKeyPressed(self.keyLeft) then x = x - 1.0 end
    if Input.isKeyPressed(self.keyRight) then x = x + 1.0 end

    -- The keys mean "up on screen" once a camera is supplied: rotate the input by the camera yaw.
    if self.relativeToCamera ~= false then
        local id = entityId(self.camera) or entityId(self.scene.camera)
        local ok, camera = pcall(function()
            return Camera(self.scene, id)
        end)
        if ok and camera and (x ~= 0.0 or z ~= 0.0) then
            local forward = camera:getWorldDirection()
            local fx, fz = forward.x, forward.z
            local fLength = math.sqrt(fx * fx + fz * fz)
            if fLength > 0.0001 then
                fx, fz = fx / fLength, fz / fLength
                local rx, rz = -fz, fx
                x, z = rx * x + fx * (-z), rz * x + fz * (-z)
            end
        end
    end

    local length = math.sqrt(x * x + z * z)
    if length > 1.0 then
        x, z = x / length, z / length
        length = 1.0
    end

    local limit = self.speed * (Input.isKeyPressed(self.keySprint) and self.sprintMultiplier or 1.0)
    local rate = clamp(self.acceleration * dt, 0.0, 1.0)
    self.velocityX = self.velocityX + (x * limit - self.velocityX) * rate
    self.velocityZ = self.velocityZ + (z * limit - self.velocityZ) * rate

    local position = object.position
    position.x = position.x + self.velocityX * dt
    position.z = position.z + self.velocityZ * dt
    if self.keepY == false then
        position.y = self.fixedY or 0.0
    end
    object.position = position

    if self.faceMovement ~= false and length > 0.001 then
        self.targetYaw = math.deg(math.atan(self.velocityX, self.velocityZ))
        local step = self.rotateSpeed * dt
        self.currentYaw = self.currentYaw + clamp(shortestArc(self.currentYaw, self.targetYaw), -step, step)
        object.rotation = Quaternion(0.0, angle(self.currentYaw), 0.0)
    end
end

return TopDownController
