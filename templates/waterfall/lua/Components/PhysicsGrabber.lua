-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Mouse grabbing for 3D bodies: a screen ray picks a Body3D and damped impulses drag it along the
-- category: Physics
-- ray, so throws still collide. Attach it to the camera entity or to anything with a camera wired.
local PhysicsGrabber = {
    properties = {
        { name = "camera", displayName = "Camera", type = "entity" },
        { name = "fallbackCameraName", displayName = "Camera name if slot is empty", type = "string", default = "Camera" },
        { name = "button", displayName = "Grab button (0 = left)", type = "int", default = 0 },
        { name = "maxDistance", displayName = "Reach (m)", type = "float", default = 12.0 },
        { name = "grabDistance", displayName = "Hold distance along the ray (m)", type = "float", default = 4.0 },
        { name = "strength", displayName = "Spring strength", type = "float", default = 60.0 },
        { name = "damping", displayName = "Velocity damping", type = "float", default = 8.0 },
        { name = "keepRotation", displayName = "Keep the rotation still", type = "bool", default = true },
        { name = "throwOnRelease", displayName = "Throw on release", type = "bool", default = true },
        { name = "throwSmoothing", displayName = "Throw smoothing (seconds)", type = "float", default = 0.12 },
        { name = "rotateAxis", displayName = "Roll the grab with the mouse wheel", type = "bool", default = false },
        { name = "allowUiEvents", displayName = "Grab even when the UI took the click", type = "bool", default = false }
    }
}

local function clamp(value, low, high) return math.min(math.max(value, low), high) end
local function num(value, fallback) return type(value) == "number" and value or fallback end

local function entityId(reference)
    local id = (type(reference) == "number") and reference
        or ((reference and type(reference.entity) == "number") and reference.entity or 0)
    return id ~= 0 and id or nil
end

function PhysicsGrabber:init()
    RegisterEngineEvent(self, "onFixedUpdate")
    self.maxDistance = math.max(0.1, num(self.maxDistance, 12.0))
    self.grabDistance = clamp(num(self.grabDistance, 4.0), 0.05, self.maxDistance)
    self.strength = math.max(0.0, num(self.strength, 60.0))
    self.damping = math.max(0.0, num(self.damping, 8.0))
    self.throwSmoothing = math.max(0.01, num(self.throwSmoothing, 0.12))
    self.button = num(self.button, 0)
    self.grabbed, self.grabbedId, self.lastWarn = nil, nil, -1.0
    self.pointerVelocity = Vector3(0.0, 0.0, 0.0)
    local id = entityId(self.camera) or entityId(self.scene:findEntity(self.fallbackCameraName))
    self.cameraId = id or entityId(self.scene.camera)
    if not self.cameraId then
        self:warn("no camera entity: the pick ray has no origin")
    end
end

function PhysicsGrabber:warn(message)
    local now = num(Engine.systemTime, 0.0)
    if now - num(self.lastWarn, -1.0) < 1.0 then return end
    self.lastWarn = now
    print("[PhysicsGrabber] " .. message)
end

function PhysicsGrabber:screenRay()
    if not self.cameraId then return nil end
    local mouse = Input.getMousePosition()
    -- screenToRay is bound as (float x, float y); the Vector2 form the header shows is not a Lua overload.
    local ok, ray = pcall(function()
        return Camera(self.scene, self.cameraId):screenToRay(mouse.x, mouse.y)
    end)
    if ok and ray then return ray end
    return nil
end

-- Box2D has a mouse joint and Jolt exposes no 3D twin to Lua, so the pick is a ray and the drag is
-- an impulse pair: the body keeps colliding while it is held.
function PhysicsGrabber:pick(ray)
    if not ray then return nil end
    local ok, hit = pcall(function() return ray:intersects(self.scene, RayFilter.BODY_3D, self.entity) end)
    if not ok then
        self:warn("the scene does not answer ray:intersects, so picking is off")
        return nil
    end
    if hit and hit.hit and hit.distance <= self.maxDistance then return hit.body end
    return nil
end

function PhysicsGrabber:grab(reference)
    local id = entityId(reference) or reference
    if type(id) ~= "number" or id == NULL_ENTITY then return false end
    local ok, body = pcall(function() return Body3D(self.scene, id) end)
    if not ok or not body then
        self:warn("entity " .. tostring(id) .. " has no Body3D component")
        return false
    end
    local isDynamic = true
    pcall(function() isDynamic = (body.type == BodyType.DYNAMIC) end)
    if not isDynamic then
        self:warn("that body is not dynamic, so it cannot be dragged")
        return false
    end
    self.grabbed, self.grabbedId = body, id
    self.pointerVelocity = Vector3(0.0, 0.0, 0.0)
    return true
end

function PhysicsGrabber:release()
    if not self.grabbed then return false end
    local body, velocity = self.grabbed, self.pointerVelocity
    if self.throwOnRelease ~= false then
        pcall(function() body:applyImpulse(velocity * math.max(num(body.mass, 0.0), 0.01)) end)
    end
    self.grabbed, self.grabbedId = nil, nil
    return true
end

function PhysicsGrabber:isGrabbing() return self.grabbed ~= nil end

function PhysicsGrabber:setStrength(value)
    self.strength = math.max(0.0, num(value, num(self.strength, 60.0)))
end

function PhysicsGrabber:onFixedUpdate()
    local dt = clamp(num(Engine.deltatime, 1.0 / 60.0), 0.0005, 0.05)
    local pressed = Input.isMousePressed(self.button)
    if pressed and not self.grabbed then
        if Engine.isUIEventReceived() and self.allowUiEvents ~= true then return end
        self:grab(self:pick(self:screenRay()) or NULL_ENTITY)
    elseif not pressed and self.grabbed then
        self:release()
        return
    end
    if not self.grabbed then return end

    local ray, body = self:screenRay(), self.grabbed
    if not ray then return end
    -- Stop the grab sliding the body through whatever the pointer is over.
    local distance = self.grabDistance
    local okCloser, nearer = pcall(function()
        local hit = ray:intersects(self.scene, RayFilter.BODY_3D, self.grabbedId)
        return (hit and hit.hit) and hit.distance or nil
    end)
    if okCloser and num(nearer, distance) < distance then distance = math.max(nearer, 0.05) end

    local ok, com = pcall(function() return body:getCenterOfMassPosition() end)
    if not ok or not com then return end
    local mass = math.max(num(body.mass, 0.0), 0.01)
    local held = (ray:getPoint(distance) - com) * (self.strength * dt)
        - body.linearVelocity * (self.damping * dt)
    -- The pointer velocity is smoothed over throwSmoothing, which is what makes a throw feel aimed
    -- instead of carrying every frame's jitter into it.
    self.pointerVelocity = self.pointerVelocity
        + (held * (1.0 / dt) - self.pointerVelocity) * clamp(dt / self.throwSmoothing, 0.0, 1.0)

    pcall(function()
        body:applyImpulse(held * mass)
        if self.keepRotation ~= false then
            body.angularVelocity = Vector3(0.0, 0.0, 0.0)
        end
        local scroll = Input.getMouseScroll().y
        if self.rotateAxis and scroll ~= 0.0 then
            body.angularVelocity = ray.direction * (scroll * 3.0)
        end
    end)
end

return PhysicsGrabber
