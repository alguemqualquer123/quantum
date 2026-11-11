-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- A collision volume built from the entity's own rendered geometry: either a convex hull (the only
-- form a dynamic body may use) or the full triangle mesh for static scenery. Attach it to the entity
-- that carries the Mesh or the Model; the engine reads that entity's submeshes and its model nodes,
-- so the volume cannot be offset - it is the geometry.
-- category: Physics
local MeshCollider = {
    properties = {
        { name = "convex", displayName = "Convex hull (required for dynamic)", type = "bool", default = true },
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
        { name = "maxRetries", displayName = "Retries while the model loads", type = "int", default = 20 },
        { name = "applyOnStart", displayName = "Build the shape on start", type = "bool", default = true }
    }
}

local MAX_SHAPES = 10

local function num(value, fallback) return type(value) == "number" and value or fallback end
local function clamp(value, low, high) return math.min(math.max(value, low), high) end

function MeshCollider:init()
    self._registeredEvents = { onFixedUpdate = true }
    RegisterEngineEvent(self, "onFixedUpdate")
    self.added, self.loaded, self.shapeIndex = false, false, -1
    self.attempts, self.nextTry, self.lastWarn = 0, 0.0, -1.0
    self.maxRetries = clamp(math.floor(num(self.maxRetries, 20)), 1, 600)
    -- Jolt's MeshShape reports MustBeStatic, and the physics system honours it: createGenericJoltBody
    -- warns and rewrites the body type to STATIC. Refusing the combination
    -- here keeps the property honest about what the entity ends up doing instead of a silent coercion.
    if self.convex == false and self.isStatic == false then
        self:warn("a non-convex mesh collider cannot be dynamic, the body is staying static")
        self.isStatic = true
    end
    if self.applyOnStart == false then return end
    self:bake()
end

function MeshCollider:shapeCount()
    local ok, count = pcall(function() return self.body:getNumShapes() end)
    return ok and num(count, 0) or 0
end

function MeshCollider:bake()
    if self.added then
        local physics = self.scene:getPhysicsSystem()
        self.loaded = (physics and physics:loadBody3D(self.entity) == true) or false
        if self.loaded then self:applySurface() end
        return self.loaded
    end

    local physics = self.scene:getPhysicsSystem()
    if not physics then
        self:warn("the scene has no physics system")
        return false
    end

    physics:createBody3D(self.entity)
    local ok, body = pcall(function() return Body3D(self.scene, self.entity) end)
    self.body = (ok and body) or nil
    if not self.body then
        self:warn("the entity refused the Body3D component")
        return false
    end

    -- Only the type is safe before the load: setType writes the component field and touches the Jolt
    -- body just when one exists. The collision bits have no component field - they live in the body's
    -- ObjectLayer - so their setters checkBody and raise on an unloaded body.
    pcall(function()
        self.body.type = self.isStatic ~= false and BodyType.STATIC or BodyType.DYNAMIC
    end)

    if self:shapeCount() >= MAX_SHAPES then
        self:warn("this body already holds " .. MAX_SHAPES .. " shapes, the mesh was not added")
        return false
    end

    -- Only the no-argument forms install the ENTITY_MESH source; the vertex-list overloads switch the
    -- shape to RAW_MESH/RAW_VERTICES and would cook an empty volume.
    local wantConvex = self.convex ~= false
    local built, index = pcall(function()
        if wantConvex then
            return self.body:createConvexHullShape()
        end
        return self.body:createMeshShape()
    end)
    if not built or type(index) ~= "number" or index < 0 then
        self:warn("the mesh shape was refused by the body")
        return false
    end
    self.added, self.shapeIndex = true, index

    -- The cook reads the shape's density, so the request has to land before loadBody3D. A triangle
    -- mesh is not a Jolt convex and keeps whatever mass properties the mesh carries.
    pcall(function()
        self.body:setShapeDensity(index, clamp(num(self.density, 1000.0), 0.001, 1000000.0))
    end)

    -- This is where the geometry is actually read: false means the model has not loaded yet, or the
    -- world was stepping, and the retry in onFixedUpdate is what recovers from both.
    self.loaded = physics:loadBody3D(self.entity) == true
    if self.loaded then self:applySurface() end
    return self.loaded
end

function MeshCollider:applySurface()
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

function MeshCollider:warn(message)
    local now = num(Engine.systemTime, 0.0)
    if now - num(self.lastWarn, -1.0) < 1.0 then return end
    self.lastWarn = now
    print("[MeshCollider] " .. message)
end

function MeshCollider:setTrigger(value)
    self.isTrigger = value == true
    if self.loaded then pcall(function() self.body.sensor = self.isTrigger end) end
end

function MeshCollider:isLoaded() return self.loaded == true end

function MeshCollider:getShapeIndex() return num(self.shapeIndex, -1) end

-- After the model is swapped, re-cook the same shape slot: Body3D:load raises needUpdateShapes and
-- reloads, so no second collider is added.
function MeshCollider:rebake()
    if not self.body then return false end
    local ok = pcall(function() self.body:load() end)
    if not ok then return self.loaded end
    local physics = self.scene:getPhysicsSystem()
    self.loaded = (physics and physics:loadBody3D(self.entity) == true) or false
    if self.loaded then self:applySurface() end
    return self.loaded
end

function MeshCollider:onFixedUpdate()
    if self.loaded then return end
    local now = num(Engine.systemTime, 0.0)
    if now - num(self.nextTry, 0.0) < 0.5 then return end
    self.nextTry = now + 0.5
    self.attempts = num(self.attempts, 0) + 1
    if self:bake() then return end
    if self.attempts == self.maxRetries then
        self:warn("gave up after " .. tostring(self.maxRetries) ..
            " tries: the entity has no mesh geometry to collide with")
    end
end

return MeshCollider
