-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Kinematic character mover: writes the entity transform directly, so it needs no physics body
-- category: Camera & Controllers
-- and stands on a terrain as long as the terrain has a collider. Attach it to the character.
local CharacterController3D = {
    properties = {
        { name = "camera", displayName = "Reference camera", type = "camera" },
        { name = "fallbackCameraName", displayName = "Camera name if slot is empty", type = "string", default = "Camera" },
        { name = "speed", displayName = "Speed (m/s)", type = "float", default = 5.0 },
        { name = "sprintMultiplier", displayName = "Sprint multiplier", type = "float", default = 1.8 },
        { name = "acceleration", displayName = "Acceleration (m/s^2)", type = "float", default = 30.0 },
        { name = "rotateSpeed", displayName = "Turn speed (degrees/s)", type = "float", default = 720.0 },
        { name = "relativeToCamera", displayName = "Move relative to camera", type = "bool", default = true },
        { name = "gravity", displayName = "Gravity (m/s^2, 0 = scene gravity)", type = "float", default = 0.0 },
        { name = "jumpHeight", displayName = "Jump height (metres)", type = "float", default = 1.2 },
        { name = "groundY", displayName = "Ground height (metres)", type = "float", default = 0.0 },
        { name = "ground", displayName = "Ground entity (its height wins over groundY)", type = "entity" },
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

-- 0 is GLFW's unknown key, so an untouched int property can fall back to the documented default.
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

local function positionOf(scene, reference)
    local id = entityId(reference)
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

-- Quaternion's euler constructor follows Engine.useDegrees, so authored degrees have to be
-- converted when a game switches the engine to radians.
local function angle(degrees)
    if Engine.useDegrees then
        return degrees
    end
    return math.rad(degrees)
end

-- The ground entity carries its origin, not its surface: a terrain authored at y = -5 with a
-- 5 m heightmap would drop the character 5 m under the grass it can see. A downward ray against
-- the physics world finds the surface actually standing there, so the entity height and the
-- authored groundY are only the fallback for a scene with no collider under the feet.
local FEET_PROBE = 0.5
local FLOOR_REACH = 6.0
local STEP_UP = 0.4

local function surfaceBelow(scene, x, y, z)
    local ok, hit = pcall(function()
        return Ray(Vector3(x, y + FEET_PROBE, z), Vector3(0, -(FEET_PROBE + FLOOR_REACH), 0))
            :intersects(scene, RayFilter.BODY_3D)
    end)
    if ok and hit and hit.hit then
        return hit.point.y
    end
    return nil
end

-- The engine exposes no shortest-arc helper, and turning the long way around a facing angle is
-- the classic stutter of a rotation that ignores the wrap.
local function shortestArc(from, to)
    local delta = (to - from) % 360.0
    if delta > 180.0 then
        delta = delta - 360.0
    end
    return delta
end

function CharacterController3D:init()
    RegisterEngineEvent(self, "onUpdate")

    self.speed = self.speed or 5.0
    self.sprintMultiplier = self.sprintMultiplier or 1.8
    self.acceleration = self.acceleration or 30.0
    self.rotateSpeed = self.rotateSpeed or 720.0
    self.jumpHeight = self.jumpHeight or 1.2
    self.groundY = self.groundY or 0.0

    self.keyForward = defaultKey(self.keyForward, Input.KEY_W)
    self.keyBack = defaultKey(self.keyBack, Input.KEY_S)
    self.keyLeft = defaultKey(self.keyLeft, Input.KEY_A)
    self.keyRight = defaultKey(self.keyRight, Input.KEY_D)
    self.keyJump = defaultKey(self.keyJump, Input.KEY_SPACE)
    self.keySprint = defaultKey(self.keySprint, Input.KEY_LEFT_SHIFT)

    self.velocityX = 0.0
    self.velocityZ = 0.0
    self.velocityY = 0.0
    self.grounded = true
    self.targetYaw = 0.0
    self.currentYaw = 0.0
    self.previousJump = false
    self.warningShown = false
end

function CharacterController3D:inputAxis()
    local x, z = 0.0, 0.0
    if Input.isKeyPressed(self.keyForward) then z = z - 1.0 end
    if Input.isKeyPressed(self.keyBack) then z = z + 1.0 end
    if Input.isKeyPressed(self.keyLeft) then x = x - 1.0 end
    if Input.isKeyPressed(self.keyRight) then x = x + 1.0 end
    return x, z
end

function CharacterController3D:resolveCameraId()
    local id = entityId(self.camera)
    if id then
        return id
    end
    if self.fallbackCameraName and self.fallbackCameraName ~= "" then
        local byName = self.scene:findEntity(self.fallbackCameraName)
        if byName and byName ~= NULL_ENTITY then
            return byName
        end
    end
    return entityId(self.scene.camera) or self.entity
end

-- Camera space into ground space: both camera axes projected onto XZ, or looking down would
-- shorten the stride.
function CharacterController3D:toCameraSpace(x, z)
    local ok, camera = pcall(function()
        return Camera(self.scene, self:resolveCameraId())
    end)
    if not ok or not camera then
        return x, z
    end

    local forward = camera:getWorldDirection()
    local right = camera:getWorldRight()

    local fx, fz = forward.x, forward.z
    local fLength = math.sqrt(fx * fx + fz * fz)
    if fLength < 0.0001 then
        return x, z
    end
    fx, fz = fx / fLength, fz / fLength

    local rx, rz = right.x, right.z
    local rLength = math.sqrt(rx * rx + rz * rz)
    if rLength < 0.0001 then
        return x, z
    end
    rx, rz = rx / rLength, rz / rLength

    return rx * x + fx * (-z), rz * x + fz * (-z)
end

function CharacterController3D:onUpdate()
    local okObject, object = pcall(function()
        return Object(self.scene, self.entity)
    end)
    if not okObject or not object then
        if not self.warningShown then
            self.warningShown = true
            print("[CharacterController3D] The entity needs a Transform component.")
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
    local rate = clamp(self.acceleration * dt, 0.0, 1.0)
    self.velocityX = self.velocityX + (x * limit - self.velocityX) * rate
    self.velocityZ = self.velocityZ + (z * limit - self.velocityZ) * rate

    local gravity = self.gravity
    if not gravity or gravity == 0.0 then
        gravity = math.abs(self.scene.gravity3D.y)
    end
    gravity = math.max(gravity, 0.1)

    local ground = self.groundY
    local groundPoint = positionOf(self.scene, self.ground)
    if groundPoint then
        ground = groundPoint.y
    end

    local jumpPressed = Input.isKeyPressed(self.keyJump)
    if jumpPressed and not self.previousJump and self.grounded then
        self.velocityY = math.sqrt(2.0 * gravity * math.max(self.jumpHeight, 0.05))
        self.grounded = false
    end
    self.previousJump = jumpPressed

    if not self.grounded then
        self.velocityY = self.velocityY - gravity * dt
    end

    local position = object.position
    position.x = position.x + self.velocityX * dt
    position.z = position.z + self.velocityZ * dt
    position.y = position.y + self.velocityY * dt

    -- The surface a collider reports under the new position is the floor. A hit higher than a
    -- stride is a wall rather than ground, so the last floor seen is held instead of climbing up
    -- the face of it, and an unanswered probe falls back to the entity height.
    local surface = surfaceBelow(self.scene, position.x, position.y, position.z)
    if surface then
        if surface <= position.y + STEP_UP then
            ground = surface
        else
            ground = self.groundHeight or ground
        end
        self.groundHeight = ground
    elseif self.groundHeight then
        ground = math.max(ground, self.groundHeight)
    end

    if position.y <= ground then
        position.y = ground
        self.velocityY = 0.0
        self.grounded = true
    elseif position.y > ground + 0.05 then
        self.grounded = false
    end

    object.position = position

    if length > 0.001 then
        self.targetYaw = math.deg(math.atan(self.velocityX, self.velocityZ))
    end

    local step = self.rotateSpeed * dt
    self.currentYaw = self.currentYaw + clamp(shortestArc(self.currentYaw, self.targetYaw), -step, step)
    object.rotation = Quaternion(0.0, angle(self.currentYaw), 0.0)
end

return CharacterController3D
