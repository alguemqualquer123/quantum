-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- A reusable four-wheel vehicle controller. It uses the entity's Body3D as the chassis and
-- applies suspension, traction, lateral grip, steering and braking forces at four wheel points.
-- category: Physics
local VehicleController = {
    properties = {
        { name = "engineForce", displayName = "Engine force (N)", type = "float", default = 6500.0 },
        { name = "brakeForce", displayName = "Brake force (N)", type = "float", default = 12000.0 },
        { name = "maxSpeed", displayName = "Maximum speed (m/s)", type = "float", default = 42.0 },
        { name = "maxSteerAngle", displayName = "Maximum steer angle (degrees)", type = "float", default = 30.0 },
        { name = "wheelbase", displayName = "Wheelbase (m)", type = "float", default = 2.4 },
        { name = "trackWidth", displayName = "Track width (m)", type = "float", default = 1.5 },
        { name = "wheelHeight", displayName = "Wheel anchor height (m)", type = "float", default = 0.35 },
        { name = "wheelRadius", displayName = "Wheel radius (m)", type = "float", default = 0.34 },
        { name = "suspensionLength", displayName = "Suspension travel (m)", type = "float", default = 0.45 },
        { name = "suspensionStiffness", displayName = "Suspension stiffness", type = "float", default = 32000.0 },
        { name = "suspensionDamping", displayName = "Suspension damping", type = "float", default = 4500.0 },
        { name = "lateralGrip", displayName = "Lateral grip", type = "float", default = 8.0 },
        { name = "steeringResponse", displayName = "Steering response", type = "float", default = 5.0 },
        { name = "throttleKey", displayName = "Throttle key (0 = W)", type = "int", default = 0 },
        { name = "reverseKey", displayName = "Reverse key (0 = S)", type = "int", default = 0 },
        { name = "brakeKey", displayName = "Brake key (0 = Space)", type = "int", default = 0 }
    }
}

local function clamp(value, low, high)
    if value < low then return low end
    if value > high then return high end
    return value
end

local function keyOr(value, fallback)
    return (not value or value == 0) and fallback or value
end

local function warn(self, message)
    if Engine.systemTime - self.lastWarning > 1.0 then
        self.lastWarning = Engine.systemTime
        print("[VehicleController] " .. message)
    end
end

function VehicleController:init()
    RegisterEngineEvent(self, "onFixedUpdate")
    self.engineForce = math.max(self.engineForce or 6500.0, 0.0)
    self.brakeForce = math.max(self.brakeForce or 12000.0, 0.0)
    self.maxSpeed = math.max(self.maxSpeed or 42.0, 0.0)
    self.maxSteerAngle = math.max(self.maxSteerAngle or 30.0, 0.0)
    self.wheelbase = math.max(self.wheelbase or 2.4, 0.2)
    self.trackWidth = math.max(self.trackWidth or 1.5, 0.2)
    self.wheelHeight = self.wheelHeight or 0.35
    self.wheelRadius = math.max(self.wheelRadius or 0.34, 0.05)
    self.suspensionLength = math.max(self.suspensionLength or 0.45, 0.05)
    self.suspensionStiffness = math.max(self.suspensionStiffness or 32000.0, 0.0)
    self.suspensionDamping = math.max(self.suspensionDamping or 4500.0, 0.0)
    self.lateralGrip = math.max(self.lateralGrip or 8.0, 0.0)
    self.steeringResponse = math.max(self.steeringResponse or 5.0, 0.0)
    self.throttleKey = keyOr(self.throttleKey, Input.KEY_W)
    self.reverseKey = keyOr(self.reverseKey, Input.KEY_S)
    self.brakeKey = keyOr(self.brakeKey, Input.KEY_SPACE)
    self.steering = 0.0
    self.lastWarning = 0.0

    local ok, body = pcall(function() return Body3D(self.scene, self.entity) end)
    if not ok or not body then
        warn(self, "The vehicle needs a Body3D component.")
        return
    end
    self.body = body
    pcall(function()
        body.type = BodyType.DYNAMIC
        if body:getNumShapes() == 0 then
            body:createBoxShape(Vector3(0.0, self.wheelHeight + 0.35, 0.0), Quaternion.IDENTITY,
                Vector3(self.trackWidth * 0.48, 0.35, self.wheelbase * 0.48))
            self.scene:getPhysicsSystem():loadBody3D(self.entity)
        end
        body:activate()
    end)
end

function VehicleController:input()
    local throttle = Input.isKeyPressed(self.throttleKey) and 1.0 or 0.0
    local reverse = Input.isKeyPressed(self.reverseKey) and 1.0 or 0.0
    local left = Input.isKeyPressed(Input.KEY_A) or Input.isKeyPressed(Input.KEY_LEFT)
    local right = Input.isKeyPressed(Input.KEY_D) or Input.isKeyPressed(Input.KEY_RIGHT)
    local steer = (right and 1.0 or 0.0) - (left and 1.0 or 0.0)
    return throttle - reverse, steer, Input.isKeyPressed(self.brakeKey)
end

function VehicleController:onFixedUpdate()
    if not self.body then return end
    local dt = clamp(Engine.deltatime or 0.016, 0.001, 1.0 / 20.0)
    local throttle, steerInput, braking = self:input()
    self.steering = self.steering + (steerInput - self.steering) * clamp(self.steeringResponse * dt, 0.0, 1.0)

    local bodyPosition = self.body.position
    local bodyRotation = self.body.rotation
    local up = bodyRotation * Vector3(0.0, 1.0, 0.0)
    local down = -up
    local forward = bodyRotation * Vector3(0.0, 0.0, -1.0)
    local right = bodyRotation * Vector3(1.0, 0.0, 0.0)
    local halfTrack = self.trackWidth * 0.5
    local halfWheelbase = self.wheelbase * 0.5
    local rayLength = self.suspensionLength + self.wheelRadius + 0.15
    local wheelPoints = {
        { x = -halfTrack, z = -halfWheelbase, front = true },
        { x = halfTrack, z = -halfWheelbase, front = true },
        { x = -halfTrack, z = halfWheelbase, front = false },
        { x = halfTrack, z = halfWheelbase, front = false }
    }

    for _, wheel in ipairs(wheelPoints) do
        local localPoint = Vector3(wheel.x, self.wheelHeight, wheel.z)
        local worldPoint = bodyPosition + bodyRotation * localPoint
        local hit = Ray(worldPoint + up * 0.08, down * rayLength):intersects(
            self.scene, RayFilter.BODY_3D, self.entity)
        if hit and hit.hit then
            local compression = clamp(self.suspensionLength - (hit.distance - self.wheelRadius),
                0.0, self.suspensionLength)
            local pointVelocity = self.body:getPointVelocity(worldPoint)
            local suspensionVelocity = pointVelocity:dotProduct(up)
            local springForce = math.max(0.0,
                compression * self.suspensionStiffness - suspensionVelocity * self.suspensionDamping)
            self.body:applyForce(up * springForce, worldPoint)

            local wheelForward = forward
            if wheel.front and math.abs(self.steering) > 0.001 then
                wheelForward = (forward + right * (self.steering * self.maxSteerAngle / 45.0)):normalized()
            end
            local wheelRight = up:crossProduct(wheelForward):normalized()
            local longitudinalSpeed = pointVelocity:dotProduct(wheelForward)
            local lateralSpeed = pointVelocity:dotProduct(wheelRight)
            local driveForce = throttle * self.engineForce - longitudinalSpeed * self.lateralGrip * 80.0
            if braking then driveForce = driveForce - longitudinalSpeed * self.brakeForce end
            self.body:applyForce(wheelForward * driveForce, worldPoint)
            self.body:applyForce(wheelRight * (-lateralSpeed * self.lateralGrip * self.body.mass), worldPoint)
        end
    end

    local velocity = self.body.linearVelocity
    local horizontal = Vector3(velocity.x, 0.0, velocity.z)
    local speed = horizontal:length()
    if self.maxSpeed > 0.0 and speed > self.maxSpeed then
        local limited = horizontal * (self.maxSpeed / speed)
        self.body:setLinearVelocityClamped(Vector3(limited.x, velocity.y, limited.z))
    end
end

return VehicleController
