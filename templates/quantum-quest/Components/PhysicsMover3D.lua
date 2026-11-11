-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Physics character for a Body3D: it steers the body toward a wanted velocity instead of writing
-- category: Camera & Controllers
-- the transform, so collisions and slopes keep working. Needs a dynamic body with shapes on the
-- same entity (create one in the inspector, or let the entity be a capsule).
local PhysicsMover3D = {
    properties = {
        { name = "camera", displayName = "Reference camera", type = "camera" },
        { name = "fallbackCameraName", displayName = "Camera name if slot is empty", type = "string", default = "Camera" },
        { name = "speed", displayName = "Speed (m/s)", type = "float", default = 5.0 },
        { name = "sprintMultiplier", displayName = "Sprint multiplier", type = "float", default = 1.7 },
        { name = "responsiveness", displayName = "How fast it reaches the wanted velocity (1/s)", type = "float", default = 10.0 },
        { name = "rotateSpeed", displayName = "Turn speed (degrees/s)", type = "float", default = 720.0 },
        { name = "relativeToCamera", displayName = "Move relative to camera", type = "bool", default = true },
        { name = "jumpHeight", displayName = "Jump height (metres)", type = "float", default = 1.2 },
        { name = "coyoteTime", displayName = "Coyote time (seconds)", type = "float", default = 0.12 },
        { name = "groundSpeed", displayName = "Grounded vertical speed (m/s)", type = "float", default = 0.4 },
        { name = "keyForward", displayName = "Forward key (0 = W)", type = "int", default = 0 },
        { name = "keyBack", displayName = "Back key (0 = S)", type = "int", default = 0 },
        { name = "keyLeft", displayName = "Left key (0 = A)", type = "int", default = 0 },
        { name = "keyRight", displayName = "Right key (0 = D)", type = "int", default = 0 },
        { name = "keyJump", displayName = "Jump key (0 = space)", type = "int", default = 0 },
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

function PhysicsMover3D:init()
    RegisterEngineEvent(self, "onFixedUpdate")
    RegisterEngineEvent(self, "onUpdate")

    self.speed = self.speed or 5.0
    self.sprintMultiplier = self.sprintMultiplier or 1.7
    self.responsiveness = self.responsiveness or 10.0
    self.rotateSpeed = self.rotateSpeed or 720.0
    self.jumpHeight = self.jumpHeight or 1.2
    self.coyoteTime = self.coyoteTime or 0.12
    self.groundSpeed = self.groundSpeed or 0.4

    self.keyForward = defaultKey(self.keyForward, Input.KEY_W)
    self.keyBack = defaultKey(self.keyBack, Input.KEY_S)
    self.keyLeft = defaultKey(self.keyLeft, Input.KEY_A)
    self.keyRight = defaultKey(self.keyRight, Input.KEY_D)
    self.keyJump = defaultKey(self.keyJump, Input.KEY_SPACE)
    self.keySprint = defaultKey(self.keySprint, Input.KEY_LEFT_SHIFT)

    self.targetYaw = 0.0
    self.currentYaw = 0.0
    self.grounded = false
    self.airborneFor = 0.0
    self.previousJump = false
    self.warningShown = false

    local ok, body = pcall(function()
        return Body3D(self.scene, self.entity)
    end)
    self.body = (ok and body) or nil
end

function PhysicsMover3D:inputAxis()
    local x, z = 0.0, 0.0
    if Input.isKeyPressed(self.keyForward) then z = z - 1.0 end
    if Input.isKeyPressed(self.keyBack) then z = z + 1.0 end
    if Input.isKeyPressed(self.keyLeft) then x = x - 1.0 end
    if Input.isKeyPressed(self.keyRight) then x = x + 1.0 end
    return x, z
end

function PhysicsMover3D:toCameraSpace(x, z)
    local id = entityId(self.camera)
    if not id and self.fallbackCameraName and self.fallbackCameraName ~= "" then
        id = entityId(self.scene:findEntity(self.fallbackCameraName))
    end
    id = id or entityId(self.scene.camera)

    local ok, camera = pcall(function()
        return Camera(self.scene, id)
    end)
    if not ok or not camera then
        return x, z
    end

    local forward = camera:getWorldDirection()
    local fx, fz = forward.x, forward.z
    local fLength = math.sqrt(fx * fx + fz * fz)
    if fLength < 0.0001 then
        return x, z
    end
    fx, fz = fx / fLength, fz / fLength
    local rx, rz = -fz, fx
    return rx * x + fx * (-z), rz * x + fz * (-z)
end

-- Vertical state is read off the fixed step, where the solver has already written the body.
function PhysicsMover3D:onFixedUpdate()
    if not self.body then
        return
    end

    local dt = 1.0 / 60.0
    local velocity = self.body.linearVelocity

    -- A body at rest on the ground has no vertical velocity; falling is what makes a jump, so the
    -- coyote window keeps the jump alive briefly after the character walks off a ledge.
    self.grounded = math.abs(velocity.y) < self.groundSpeed
    if self.grounded then
        self.airborneFor = 0.0
    else
        self.airborneFor = self.airborneFor + dt
    end
end

function PhysicsMover3D:onUpdate()
    if not self.body then
        if not self.warningShown then
            self.warningShown = true
            print("[PhysicsMover3D] The entity needs a Body3D component.")
        end
        return
    end

    local dt = math.min(Engine.deltatime, 0.05)
    local x, z = self:inputAxis()

    if self.relativeToCamera ~= false and (x ~= 0.0 or z ~= 0.0) then
        x, z = self:toCameraSpace(x, z)
    end

    local length = math.sqrt(x * x + z * z)
    if length > 1.0 then
        x, z = x / length, z / length
        length = 1.0
    end

    local limit = self.speed * (Input.isKeyPressed(self.keySprint) and self.sprintMultiplier or 1.0)
    local velocity = self.body.linearVelocity

    local rate = clamp(self.responsiveness * dt, 0.0, 1.0)
    -- Gravity owns Y, so only X and Z are steered.
    self.body.linearVelocity = Vector3(
        velocity.x + (x * limit - velocity.x) * rate,
        velocity.y,
        velocity.z + (z * limit - velocity.z) * rate
    )

    local jumpPressed = Input.isKeyPressed(self.keyJump)
    local canJump = self.grounded or self.airborneFor <= self.coyoteTime
    if jumpPressed and not self.previousJump and canJump then
        local gravity = math.max(math.abs(self.scene.gravity3D.y), 0.1)
        local upward = math.sqrt(2.0 * gravity * math.max(self.jumpHeight, 0.05))
        self.body.linearVelocity = Vector3(self.body.linearVelocity.x, upward, self.body.linearVelocity.z)
        self.airborneFor = self.coyoteTime + 1.0
    end
    self.previousJump = jumpPressed

    if length > 0.001 then
        self.targetYaw = math.deg(math.atan(x, z))
    end

    local okObject, object = pcall(function()
        return Object(self.scene, self.entity)
    end)
    if okObject and object then
        local step = self.rotateSpeed * dt
        self.currentYaw = self.currentYaw + clamp(shortestArc(self.currentYaw, self.targetYaw), -step, step)
        object.rotation = Quaternion(0.0, angle(self.currentYaw), 0.0)
    end
end

return PhysicsMover3D
