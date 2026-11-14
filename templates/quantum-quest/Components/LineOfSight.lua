-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Occlusion-correct sight for guards and enemies: range, view cone and a ray against the bodies in
-- category: Interaction & Combat
-- between. Attach it to the observer and drag what it watches into the Target slot.
local LineOfSight = {
    properties = {
        { name = "target", displayName = "Target", type = "entity" },
        { name = "fallbackName", displayName = "Target name if slot is empty", type = "string", default = "Player" },
        { name = "maxDistance", displayName = "Sight range (units)", type = "float", default = 20.0 },
        { name = "fovDegrees", displayName = "Field of view (degrees, 360 = all around)", type = "float", default = 120.0 },
        { name = "checkInterval", displayName = "Check every (seconds)", type = "float", default = 0.15 },
        { name = "occlude", displayName = "Blocked by bodies", type = "bool", default = true },
        { name = "use2D", displayName = "Query 2D bodies instead of 3D", type = "bool", default = false },
        { name = "marker", displayName = "Marker entity (shown while seen)", type = "entity" },
        { name = "chase", displayName = "Chase with the mover", type = "bool", default = false },
        { name = "chaseDistance", displayName = "Start chasing beyond (units)", type = "float", default = 6.0 },
        { name = "mover", displayName = "Mover (PatrolPath or PhysicsMover3D)", type = "PatrolPath" },
        { name = "onSeenEntity", displayName = "On seen: entity", type = "entity" },
        { name = "onSeenMethod", displayName = "On seen: method", type = "string", default = "onSeen" },
        { name = "onLostEntity", displayName = "On lost: entity", type = "entity" },
        { name = "onLostMethod", displayName = "On lost: method", type = "string", default = "onLost" }
    }
}

local function clamp(value, low, high) return math.min(math.max(value, low), high) end

-- A slot reaches Lua as a script table, an engine handle or the bare stored id (0 = NULL_ENTITY).
local function entityId(reference)
    if type(reference) == "number" then return reference ~= 0 and reference or nil end
    if type(reference) ~= "table" and type(reference) ~= "userdata" then return nil end
    return reference.entity ~= 0 and reference.entity or nil
end
local function slotId(scene, reference, fallbackName)
    local id = entityId(reference)
    if not id and fallbackName and fallbackName ~= "" then id = entityId(scene:findEntity(fallbackName)) end
    return id
end
local function positionOf(scene, id)
    if not id then return nil end
    local ok, position = pcall(function() return Object(scene, id).position end)
    return (ok and position) or nil
end
local function callMethod(reference, methodName, first, second)
    if type(reference) ~= "table" or not methodName or methodName == "" then return false end
    local method = reference[methodName]
    if type(method) ~= "function" then return false end
    method(reference, first, second)
    return true
end

function LineOfSight:init()
    RegisterEngineEvent(self, "onUpdate")
    self.maxDistance = math.max(tonumber(self.maxDistance) or 20.0, 0.1)
    self.checkInterval = math.max(tonumber(self.checkInterval) or 0.15, 0.01)
    self.fovDegrees, self.chaseDistance = tonumber(self.fovDegrees) or 120.0, tonumber(self.chaseDistance) or 6.0
    self.visible, self.seen, self.lastWarn, self.markerShown = false, false, 0.0, nil
    self.distance, self.sinceCheck = self.maxDistance, self.checkInterval
    local ok, object = pcall(function() return Object(self.scene, self.entity) end)
    self.object = (ok and object) or nil
end

function LineOfSight:warn(message)
    if Engine.systemTime - self.lastWarn > 1.0 then
        self.lastWarn = Engine.systemTime
        print("[LineOfSight] " .. message)
    end
end

function LineOfSight:forward()
    local ok, direction = pcall(function() return self.object:getWorldDirection() end)
    if ok and direction then return direction end
    -- The shipped Object binding may stop short of that helper, so read the axis off the rotation.
    local _, rotation = pcall(function() return self.object:getWorldRotation() end)
    local axisOk, axis = pcall(function() return rotation and rotation:zAxis() end)
    return (axisOk and axis) or nil
end

-- Lua binds two scene query shapes: the one that ignores an entity keeps the observer from seeing
-- itself, and the plain form is the fallback. There is no overlap test, so occlusion is one ray.
function LineOfSight:cast(origin, direction)
    local ok, ray = pcall(function() return Ray(origin, direction) end)
    if not ok or not ray then return nil end
    local filter = self.use2D == true and RayFilter.BODY_2D or RayFilter.BODY_3D
    local attempts = {
        function() return ray:intersects(self.scene, filter, self.entity) end,
        function() return ray:intersects(self.scene, filter, false) end }
    for _, attempt in ipairs(attempts) do
        local hitOk, hit = pcall(attempt)
        if hitOk and hit then return hit end
    end
    return nil
end

function LineOfSight:canSee(reference)
    local id = entityId(reference)
    local here = self.object and self.object.position
    local point = positionOf(self.scene, id)
    if not here or not point then return false end
    local dx, dy, dz = point.x - here.x, point.y - here.y, point.z - here.z
    local distance = math.sqrt(dx * dx + dy * dy + dz * dz)
    self.distance = distance
    if distance < 0.0001 or distance > self.maxDistance then return false end
    if self.fovDegrees < 360.0 then
        local forward = self:forward()
        if forward then
            local dot = clamp(forward:dotProduct(Vector3(dx, dy, dz)) / distance, -1.0, 1.0)
            if math.deg(math.acos(dot)) > self.fovDegrees * 0.5 then return false end
        end
    end
    if self.occlude == true then
        local hit = self:cast(Vector3(here.x, here.y, here.z), Vector3(dx / distance, dy / distance, dz / distance))
        -- Anything closer than the target, minus a slack for its own size, blocks the view.
        if hit and hit.hit and hit.distance < distance - 0.25 then return false end
    end
    return true
end

-- The slot promises one component type, so whatever method the mover answers to is tried blind.
function LineOfSight:pursue(id)
    if self.chase ~= true then return end
    local mover = self.mover
    if type(mover) ~= "table" then
        self:warn("wire this: a PatrolPath or PhysicsMover3D entity in the Mover slot")
        return
    end
    if self.seen and self.distance > self.chaseDistance then
        pcall(function() if type(mover.setTarget) == "function" then mover:setTarget(id) end end)
        pcall(function() if type(mover.resume) == "function" then mover:resume() end end)
    else
        pcall(function() if type(mover.pause) == "function" then mover:pause() end end)
    end
end

function LineOfSight:isSeen() return self.seen end
function LineOfSight:getDistance() return self.distance end
function LineOfSight:refresh()
    self.sinceCheck = self.checkInterval
    self:onUpdate()
    return self.visible
end

function LineOfSight:onUpdate()
    if not self.object then self:warn("this entity needs a Transform component") return end
    -- A sight check every frame is wasted work: the interval is what makes many guards affordable.
    self.sinceCheck = self.sinceCheck + math.min(Engine.deltatime, 0.05)
    if self.sinceCheck < self.checkInterval then return end
    self.sinceCheck = 0.0
    local id = slotId(self.scene, self.target, self.fallbackName)
    if not id then self:warn("wire this: drag the player into the Target slot") return end
    self.visible = self:canSee(id)
    local marker = entityId(self.marker)
    if marker and self.markerShown ~= self.visible then
        self.markerShown = self.visible
        pcall(function() Object(self.scene, marker).visible = self.visible end)
    end
    if self.visible and not self.seen then
        self.seen = true
        callMethod(self.onSeenEntity, self.onSeenMethod, id, self.entity)
    elseif not self.visible and self.seen then
        self.seen = false
        callMethod(self.onLostEntity, self.onLostMethod, id, self.entity)
    end
    self:pursue(id)
end

return LineOfSight
