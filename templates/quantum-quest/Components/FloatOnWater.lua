-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Real floating for a Body3D: attach to any dynamic body near the water; it samples the surface
-- category: Water & Shore
-- and shallow-field current at the centre of mass each fixed step and feeds applyBuoyancyImpulse.
local FloatOnWater = {
    properties = {
        { name = "water", displayName = "Water", type = "entity" },
        { name = "fallbackName", displayName = "Water name if slot is empty", type = "string", default = "Water" },
        { name = "buoyancy", displayName = "Buoyancy strength (1 = neutral)", type = "float", default = 1.0 },
        { name = "waterDrag", displayName = "Linear drag in water (1/s)", type = "float", default = 0.4 },
        { name = "waterAngularDrag", displayName = "Angular drag in water (1/s)", type = "float", default = 0.6 },
        { name = "bob", displayName = "Bob amplitude (metres)", type = "float", default = 0.05 },
        { name = "stabilise", displayName = "Keep the hull level (no bob)", type = "bool", default = true },
        { name = "rideCurrent", displayName = "Ride the simulated current", type = "bool", default = true },
        { name = "sinkPoint", displayName = "Sink point (submerged fraction 0..1)", type = "float", default = 0.4 },
        { name = "maxSpeed", displayName = "Max speed (m/s, 0 = unlimited)", type = "float", default = 0.0 },
        { name = "onFloatEntity", displayName = "On float: entity", type = "entity" },
        { name = "onFloatMethod", displayName = "On float: method", type = "string", default = "onFloat" },
        { name = "onSinkEntity", displayName = "On sink: entity", type = "entity" },
        { name = "onSinkMethod", displayName = "On sink: method", type = "string", default = "onSink" }
    }
}

local function clamp(v, lo, hi) if v < lo then return lo end if v > hi then return hi end return v end

local function entityId(ref)
    if type(ref) == "number" then return ref ~= 0 and ref or nil end
    if ref and type(ref.entity) == "number" then return ref.entity ~= 0 and ref.entity or nil end
    return nil
end

local function warn(self, message)
    if Engine.systemTime - self.lastWarn > 1.0 then
        self.lastWarn = Engine.systemTime
        print("[FloatOnWater] " .. message)
    end
end

local function callMethod(reference, methodName)
    if type(reference) ~= "table" or not methodName or methodName == "" then return end
    local method = reference[methodName]
    if type(method) == "function" then pcall(method, reference) end
end

function FloatOnWater:init()
    RegisterEngineEvent(self, "onFixedUpdate")
    self.buoyancy = self.buoyancy or 1.0
    self.waterDrag, self.waterAngularDrag = self.waterDrag or 0.4, self.waterAngularDrag or 0.6
    self.bob = self.bob or 0.05
    self.sinkPoint = clamp(self.sinkPoint or 0.4, 0.0, 1.0)
    self.submerged, self.floating, self.sunk = 0.0, false, false
    self.lastWarn = 0.0
    local ok, body = pcall(function()
        return Body3D(self.scene, self.entity)
    end)
    self.body = (ok and body) or nil
end

function FloatOnWater:resolveWater()
    if self.waterHandle then return self.waterHandle end
    local id = entityId(self.water)
    if not id and self.fallbackName and self.fallbackName ~= "" then
        id = entityId(self.scene:findEntity(self.fallbackName))
    end
    if not id then return nil end
    local ok, water = pcall(function()
        return Water(self.scene, id)
    end)
    self.waterHandle = (ok and water) or nil
    return self.waterHandle
end

function FloatOnWater:getSubmerged()
    return self.submerged or 0.0
end

function FloatOnWater:setBuoyancy(value)
    if type(value) == "number" then self.buoyancy = math.max(value, 0.0) end
end

function FloatOnWater:onFixedUpdate()
    if not self.body then
        warn(self, "The entity needs a Body3D component.")
        return
    end
    local water = self:resolveWater()
    if not water then
        warn(self, "No water: add a Water component or drag it into the Water slot.")
        return
    end

    local dt = math.min(Engine.deltatime, 1.0 / 30.0)
    local com = self.body:getCenterOfMassPosition()
    local surfaceY = water:getHeight(com.x, com.z)
    local depth = clamp(water:getSimWaterDepth(com.x, com.z), 0.0, 1000000.0)
    local normal = water:getNormal(com.x, com.z)
    -- Fraction of the hull under the surface, measured against the water's own buoyancyDepth.
    self.submerged = clamp((surfaceY - com.y) / math.max(water.buoyancyDepth, 0.3) + 0.5, 0.0, 1.0)
    if depth <= 0.0 then
        self.submerged = 0.0
    end

    local nowFloating = self.submerged > 0.05
    if nowFloating ~= self.floating then
        self.floating = nowFloating
        if nowFloating then callMethod(self.onFloatEntity, self.onFloatMethod) end
    end
    -- Past the sink point the lift fades and the body goes under; it resurfaces when the
    -- waterline drops back below the point again.
    local nowSunk = self.sinkPoint < 1.0 and self.submerged > self.sinkPoint
    if nowSunk ~= self.sunk then
        self.sunk = nowSunk
        if nowSunk then callMethod(self.onSinkEntity, self.onSinkMethod) end
    end
    if not nowFloating then return end

    local lift = self.buoyancy
    if self.sunk then
        lift = lift * clamp(1.0 - (self.submerged - self.sinkPoint) * 2.0, 0.15, 1.0)
    end
    local surface = Vector3(com.x, surfaceY, com.z)
    if not self.stabilise and self.bob > 0.0 then
        surface = Vector3(surface.x, surface.y + math.sin(Engine.systemTime * 2.0) * self.bob, surface.z)
    end
    local fluid = Vector3(0.0, 0.0, 0.0)
    if self.rideCurrent then
        local current = water:getSimVelocity(com.x, com.z)
        fluid = Vector3(fluid.x + current.x, fluid.y + current.y, fluid.z + current.z)
    end

    local ok = pcall(function()
        self.body:applyBuoyancyImpulse(surface, normal, lift, self.waterDrag,
            self.waterAngularDrag, fluid, self.scene.gravity3D, dt)
    end)
    if not ok then
        warn(self, "applyBuoyancyImpulse failed: check the Body3D is dynamic.")
        return
    end
    if (self.maxSpeed or 0.0) > 0.0 then
        local velocity = self.body.linearVelocity
        local speed = velocity:length()
        if speed > self.maxSpeed then
            self.body.linearVelocity = velocity * (self.maxSpeed / speed)
        end
    end
end

return FloatOnWater
