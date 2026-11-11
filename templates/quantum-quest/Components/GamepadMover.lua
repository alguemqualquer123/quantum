-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Gamepad twin of the top-down mover: the left stick steers a Body3D across X/Z (gravity keeps
-- category: Environment & Motion
-- Y), the right trigger or bumper sprints and A jumps. Attach to the entity with a dynamic Body3D.
local GamepadMover = {
    properties = {
        { name = "padIndex", displayName = "Pad index (0 = first)", type = "int", default = 0 },
        { name = "axisLeftX", displayName = "Left stick X axis id (0)", type = "int", default = 0 },
        { name = "axisLeftY", displayName = "Left stick Y axis id (1)", type = "int", default = 1 },
        { name = "deadzone", displayName = "Stick deadzone (0..0.9)", type = "float", default = 0.18 },
        { name = "invertY", displayName = "Invert stick Y", type = "bool", default = true },
        { name = "speed", displayName = "Speed (m/s)", type = "float", default = 5.0 },
        { name = "acceleration", displayName = "Acceleration (1/s)", type = "float", default = 12.0 },
        { name = "sprintAxis", displayName = "Sprint axis id (5 = right trigger)", type = "int", default = 5 },
        { name = "sprintThreshold", displayName = "Sprint axis threshold", type = "float", default = 0.15 },
        { name = "sprintButton", displayName = "Sprint button id (4 = left bumper)", type = "int", default = 4 },
        { name = "sprintMultiplier", displayName = "Sprint multiplier", type = "float", default = 1.6 },
        { name = "jumpButton", displayName = "Jump button id (0 = A)", type = "int", default = 0 },
        { name = "jumpHeight", displayName = "Jump height (metres)", type = "float", default = 1.2 },
        { name = "coyoteTime", displayName = "Coyote time (seconds)", type = "float", default = 0.12 },
        { name = "groundSpeed", displayName = "Grounded vertical speed (m/s)", type = "float", default = 0.4 },
        { name = "rotateSpeed", displayName = "Turn speed (degrees/s)", type = "float", default = 540.0 },
        { name = "relativeToCamera", displayName = "Move relative to camera", type = "bool", default = true },
        { name = "camera", displayName = "Reference camera", type = "entity" },
        { name = "fallbackCameraName", displayName = "Camera name if slot is empty", type = "string", default = "Camera" }
    }
}

local function clamp(v, lo, hi) if v < lo then return lo end; if v > hi then return hi end; return v end
-- A slot reaches Lua as a script table, an engine handle or a bare id, and 0 is NULL_ENTITY.
local function entityId(reference)
    if type(reference) == "number" then return reference ~= 0 and reference or nil end
    if reference and type(reference.entity) == "number" and reference.entity ~= 0 then return reference.entity end
    return nil
end

-- The engine keeps angles in degrees only while useDegrees is set.
local function angle(deg) return Engine.useDegrees and deg or math.rad(deg) end

local function shortestArc(from, to)
    local delta = (to - from) % 360.0
    return delta > 180.0 and delta - 360.0 or delta
end

function GamepadMover:init()
    RegisterEngineEvent(self, "onFixedUpdate")
    RegisterEngineEvent(self, "onUpdate")
    self.speed = self.speed or 5.0
    self.acceleration = self.acceleration or 12.0
    self.deadzone = clamp(self.deadzone or 0.18, 0.0, 0.9)
    self.jumpHeight = self.jumpHeight or 1.2
    self.coyoteTime = self.coyoteTime or 0.12
    self.groundSpeed = self.groundSpeed or 0.4
    self.rotateSpeed = self.rotateSpeed or 540.0
    self.inputVector = Vector3(0.0, 0.0, 0.0)
    self.grounded, self.airborneFor = false, 0.0
    self.previousJump = false
    self.currentYaw, self.targetYaw = 0.0, 0.0
    self.bodyMissing, self.padWarned = false, false
    local ok, body = pcall(function() return Body3D(self.scene, self.entity) end)
    self.body = (ok and body) or nil
end

-- Raw axis with the deadzone cut out and the remaining range re-normalised to -1..1.
function GamepadMover:readAxis(axis)
    if type(axis) ~= "number" then return 0.0 end
    local ok, value = pcall(function() return Input.getGamepadAxis(self.padIndex, axis) end)
    if not ok or type(value) ~= "number" or math.abs(value) < self.deadzone then return 0.0 end
    local sign = value > 0.0 and 1.0 or -1.0
    return sign * (math.abs(value) - self.deadzone) / (1.0 - self.deadzone)
end

function GamepadMover:toCameraSpace(x, z)
    local id = entityId(self.camera)
    if not id and self.fallbackCameraName and self.fallbackCameraName ~= "" then id = entityId(self.scene:findEntity(self.fallbackCameraName)) end
    id = id or entityId(self.scene.camera)
    local ok, camera = pcall(function() return Camera(self.scene, id) end)
    if not ok or not camera then return x, z end
    local forward = camera:getWorldDirection()
    local fx, fz = forward.x, forward.z
    local fLength = math.sqrt(fx * fx + fz * fz)
    if fLength < 0.0001 then return x, z end
    fx, fz = fx / fLength, fz / fLength
    local rx, rz = -fz, fx
    return rx * x + fx * (-z), rz * x + fz * (-z)
end

-- Vertical state is read off the fixed step, where the solver has already written the body.
function GamepadMover:onFixedUpdate()
    if not self.body then return end
    if math.abs(self.body.linearVelocity.y) < self.groundSpeed then self.grounded = true; self.airborneFor = 0.0
    else
        self.grounded = false
        self.airborneFor = self.airborneFor + 1.0 / 60.0
    end
end

function GamepadMover:onUpdate()
    if not self.body then
        if not self.bodyMissing then
            self.bodyMissing = true
            print("[GamepadMover] The entity needs a Body3D component.")
        end
        return
    end
    if Input.numGamepads() == 0 then
        if not self.padWarned then
            self.padWarned = true
            print("[GamepadMover] No gamepad connected: plug one in or use TopDownController.")
        end
        return
    end

    local x = self:readAxis(self.axisLeftX)
    local z = self:readAxis(self.axisLeftY)
    if self.invertY ~= false then z = -z end
    self.inputVector = Vector3(x, 0.0, z)
    if self.relativeToCamera ~= false and (x ~= 0.0 or z ~= 0.0) then
        x, z = self:toCameraSpace(x, z)
    end

    local dt = math.min(Engine.deltatime, 0.05)
    local raw = math.sqrt(x * x + z * z)
    local length = math.min(raw, 1.0)
    if raw > 1.0 then x, z = x / raw, z / raw end

    local sprinting = self:readAxis(self.sprintAxis) >= self.sprintThreshold or Input.isGamepadButtonPressed(self.padIndex, self.sprintButton)
    local limit = self.speed * (sprinting and self.sprintMultiplier or 1.0) * length
    local velocity = self.body.linearVelocity
    local rate = clamp(self.acceleration * dt, 0.0, 1.0)
    -- Gravity owns Y, so only X and Z are steered; the analog tilt scales the wanted speed.
    self.body.linearVelocity = Vector3(velocity.x + (x * limit - velocity.x) * rate, velocity.y,
        velocity.z + (z * limit - velocity.z) * rate)

    -- No released-query in the binding: the edge is the previous frame's boolean.
    local jumpPressed = Input.isGamepadButtonPressed(self.padIndex, self.jumpButton)
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
        local okObject, object = pcall(function() return Object(self.scene, self.entity) end)
        if okObject and object then
            local step = self.rotateSpeed * dt
            self.currentYaw = self.currentYaw + clamp(shortestArc(self.currentYaw, self.targetYaw), -step, step)
            object.rotation = Quaternion(0.0, angle(self.currentYaw), 0.0)
        end
    end
end

function GamepadMover:getInputVector() return self.inputVector end

function GamepadMover:isActive()
    local ok, connected = pcall(function() return Input.isGamepadConnected(self.padIndex) end)
    return ok and connected == true
end

return GamepadMover
