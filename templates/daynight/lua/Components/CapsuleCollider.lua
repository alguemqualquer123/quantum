-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- A capsule collision volume that builds its own Body3D, so dropping it on an entity is enough to
-- make that entity solid: radius, total height, axis, center offset, trigger mode, surface and
-- collision layers. Attach it to the entity that should collide.
-- category: Physics
local CapsuleCollider = {
    properties = {
        { name = "radius", displayName = "Radius (m)", type = "float", default = 0.5 },
        { name = "height", displayName = "Total height (m)", type = "float", default = 2.0 },
        { name = "direction", displayName = "Axis (X / Y / Z)", type = "string", default = "Y" },
        { name = "center", displayName = "Center offset", type = "vector3", default = { 0.0, 0.0, 0.0 } },
        { name = "isTrigger", displayName = "Is trigger (sensor)", type = "bool", default = false },
        { name = "isStatic", displayName = "Static body", type = "bool", default = true },
        { name = "density", displayName = "Density (kg/m3)", type = "float", default = 1000.0 },
        { name = "mass", displayName = "Mass override (0 = from density)", type = "float", default = 0.0 },
        { name = "friction", displayName = "Friction (0..1)", type = "float", default = 0.6 },
        { name = "restitution", displayName = "Restitution (0..1)", type = "float", default = 0.0 },
        { name = "categoryBits", displayName = "Category bits", type = "int", default = 1 },
        { name = "maskBits", displayName = "Mask bits", type = "int", default = 65535 },
        { name = "antiTunnel", displayName = "Continuous collision (dynamic only)", type = "bool", default = false },
        { name = "allowSleep", displayName = "Allow sleeping", type = "bool", default = true },
        { name = "applyOnStart", displayName = "Build the shape on start", type = "bool", default = true }
    }
}

local MAX_SHAPES = 10

local function num(value, fallback) return type(value) == "number" and value or fallback end
local function clamp(value, low, high) return math.min(math.max(value, low), high) end

-- The inspector delivers a Vector3 userdata and the default above is a table; an entity handle in
-- the slot would raise, hence the pcall.
local function asVec3(value, fallback)
    local ok, out = pcall(function()
        if type(value) ~= "table" and type(value) ~= "userdata" then return fallback end
        if type(value.x) == "number" then return Vector3(value.x, num(value.y, 0.0), num(value.z, 0.0)) end
        return type(value[1]) == "number" and Vector3(value[1], num(value[2], 0.0), num(value[3], 0.0)) or fallback
    end)
    return ok and out or fallback
end

-- Jolt's capsule is Y-up, so the axis is a shape rotation. The components are written out because
-- Quaternion(angle, axis) reads Engine.useDegrees, which a project may flip at runtime.
local function axisQuaternion(direction)
    local s = 0.7071067811865476
    local axis = string.upper(tostring(direction or "Y"))
    if axis:sub(1, 1) == "X" then return Quaternion(s, 0.0, 0.0, -s) end
    if axis:sub(1, 1) == "Z" then return Quaternion(s, s, 0.0, 0.0) end
    return Quaternion(1.0, 0.0, 0.0, 0.0)
end

function CapsuleCollider:init()
    self._registeredEvents = { onFixedUpdate = true }
    RegisterEngineEvent(self, "onFixedUpdate")
    self.added, self.loaded, self.shapeIndex = false, false, -1
    self.attempts, self.nextTry, self.lastWarn = 0, 0.0, -1.0
    self:dimensions()
    if self.applyOnStart == false then return end
    self:apply()
end

-- Unity's height is the whole capsule, Jolt's halfHeight is only the cylinder part.
function CapsuleCollider:dimensions()
    self.radius = math.max(0.01, num(self.radius, 0.5))
    local total = num(self.height, self.radius * 4.0)
    if total < self.radius * 2.0 then total = self.radius * 2.0 end
    self.height = total
    return self.radius, math.max(0.0, total * 0.5 - self.radius)
end

function CapsuleCollider:shapeCount()
    local ok, count = pcall(function() return self.body:getNumShapes() end)
    return ok and num(count, 0) or 0
end

function CapsuleCollider:apply()
    if self.added then return self.loaded end
    local physics = self.scene:getPhysicsSystem()
    if not physics then
        self:warn("the scene has no physics system")
        return false
    end

    -- Idempotent: createBody3D adds the component only when the signature lacks it, so an entity
    -- already authored with a Body3D keeps its own body and gains this shape on top.
    physics:createBody3D(self.entity)
    local ok, body = pcall(function() return Body3D(self.scene, self.entity) end)
    self.body = (ok and body) or nil
    if not self.body then
        self:warn("the entity refused the Body3D component")
        return false
    end

    -- Only the type is safe before the load: setType writes the component field and touches the Jolt
    -- body just when one exists. The collision bits have no component field at all - they live in the
    -- body's ObjectLayer - so setCategoryBitsFilter/setMaskBitsFilter checkBody and raise on an
    -- unloaded body. They belong in applySurface, after the cook.
    pcall(function()
        self.body.type = self.isStatic ~= false and BodyType.STATIC or BodyType.DYNAMIC
    end)

    local radius, halfHeight = self:dimensions()
    local center = asVec3(self.center, Vector3(0.0, 0.0, 0.0))
    local rotation = axisQuaternion(self.direction)
    if self:shapeCount() >= MAX_SHAPES then
        self:warn("this body already holds " .. MAX_SHAPES .. " shapes, the capsule was not added")
        return false
    end

    local built, index = pcall(function()
        return self.body:createCapsuleShape(center, rotation, halfHeight, radius)
    end)
    if not built or type(index) ~= "number" or index < 0 then
        self:warn("the capsule shape was refused by the body")
        return false
    end
    self.added, self.shapeIndex = true, index
    -- Density is read by the cook (PhysicsSystem calls SetDensity on the Jolt convex at the end of
    -- createShape3DForIndex), so it has to land between appending the shape and loading the body.
    -- Before the cook the live shape does not exist yet, so the engine prints the expected
    -- "The shape 0 does not have density" warning while still storing the value on the component.
    pcall(function()
        self.body:setShapeDensity(index, clamp(num(self.density, 1000.0), 0.001, 1000000.0))
    end)

    self.loaded = physics:loadBody3D(self.entity) == true
    if self.loaded then self:applySurface() end
    return self.loaded
end

-- Only reachable on a loaded body: the collision bits, friction, restitution, sleeping and mass all
-- resolve the Jolt handle, and checkBody raises "Body3D is not loaded" when it is still invalid.
function CapsuleCollider:applySurface()
    pcall(function()
        self.body.categoryBitsFilter = math.floor(num(self.categoryBits, 1))
        self.body.maskBitsFilter = math.floor(num(self.maskBits, 65535))
    end)
    pcall(function()
        self.body.sensor = self.isTrigger == true
        self.body.friction = clamp(num(self.friction, 0.6), 0.0, 1.0)
        self.body.restitution = clamp(num(self.restitution, 0.0), 0.0, 1.0)
        self.body.allowSleeping = self.allowSleep ~= false
        if self.isStatic == false then
            if self.antiTunnel then
                self.body.motionQuality = Body3DMotionQuality.LINEAR_CAST
            end
            local mass = num(self.mass, 0.0)
            if mass > 0.0 then self.body.mass = mass end
        end
    end)
end

function CapsuleCollider:warn(message)
    local now = num(Engine.systemTime, 0.0)
    if now - num(self.lastWarn, -1.0) < 1.0 then return end
    self.lastWarn = now
    print("[CapsuleCollider] " .. message)
end

-- A Jolt shape is cooked once: there is no Lua handle on Shape3D, so radius/height/center cannot be
-- rewritten after the body exists. Resizing means a new entity or a new body.
function CapsuleCollider:setSize(radius, height)
    self:warn("a built capsule cannot be resized from Lua; the request for " ..
        tostring(radius) .. " x " .. tostring(height) .. " was ignored")
    return false
end

function CapsuleCollider:setTrigger(value)
    self.isTrigger = value == true
    if self.loaded then pcall(function() self.body.sensor = self.isTrigger end) end
end

function CapsuleCollider:isLoaded() return self.loaded == true end

function CapsuleCollider:getShapeIndex() return num(self.shapeIndex, -1) end

function CapsuleCollider:onFixedUpdate()
    if self.loaded then return end
    local now = num(Engine.systemTime, 0.0)
    if now - num(self.nextTry, 0.0) < 0.25 then return end
    self.nextTry = now + 0.25
    self.attempts = num(self.attempts, 0) + 1
    -- loadBody3D refuses while the world is stepping and returns false; retrying is the fix.
    local physics = self.scene:getPhysicsSystem()
    if physics and physics:loadBody3D(self.entity) == true then
        self.loaded = true
        self:applySurface()
    elseif self.attempts == 20 then
        self:warn("the body is still not loaded after 20 tries")
    end
end

return CapsuleCollider
