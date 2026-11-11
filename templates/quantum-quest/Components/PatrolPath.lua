-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Route between named places: guards, drones, supply crates, patrol boats. Attach it to the mover
-- category: Interaction & Combat
-- and either list the waypoint entity names ("A,B,C,D") or drop the entities in the A..D slots.
local PatrolPath = {
    properties = {
        { name = "waypointNames", displayName = "Waypoint names (comma separated)", type = "string", default = "A,B,C,D" },
        { name = "waypointA", displayName = "Waypoint A", type = "entity" },
        { name = "waypointB", displayName = "Waypoint B", type = "entity" },
        { name = "waypointC", displayName = "Waypoint C", type = "entity" },
        { name = "waypointD", displayName = "Waypoint D", type = "entity" },
        { name = "mode", displayName = "Drive (0 = transform, 1 = body)", type = "int", default = 0 },
        { name = "subjectBody", displayName = "Body to steer (empty = this entity)", type = "entity" },
        { name = "speed", displayName = "Speed (units/s)", type = "float", default = 2.0 },
        { name = "turnSpeed", displayName = "Turn speed (degrees/s, 0 = instant)", type = "float", default = 180.0 },
        { name = "pauseAtEach", displayName = "Pause at each waypoint (seconds)", type = "float", default = 0.5 },
        { name = "reachedDistance", displayName = "Arrive distance (units)", type = "float", default = 0.3 },
        { name = "loop", displayName = "Loop back to the start", type = "bool", default = true },
        { name = "pingPong", displayName = "Walk the route both ways", type = "bool", default = false },
        { name = "facing", displayName = "Facing (0 = none, 1 = travel, 2 = look at slot)", type = "int", default = 1 },
        { name = "lookAt", displayName = "Look at", type = "entity" },
        { name = "fallbackName", displayName = "Look-at name if slot is empty", type = "string", default = "Player" },
        { name = "onWaypointEntity", displayName = "On waypoint: entity", type = "entity" },
        { name = "onWaypointMethod", displayName = "On waypoint: method", type = "string", default = "onWaypoint" },
        { name = "onLoopEntity", displayName = "On full loop: entity", type = "entity" },
        { name = "onLoopMethod", displayName = "On full loop: method", type = "string", default = "onLoop" }
    }
}

local function clamp(value, low, high) return math.min(math.max(value, low), high) end
local function angle(degrees) return Engine.useDegrees and degrees or math.rad(degrees) end
-- A slot reaches Lua as a script table, an engine handle or the bare stored id (0 = NULL_ENTITY).
local function entityId(reference)
    if type(reference) == "number" then return reference ~= 0 and reference or nil end
    if type(reference) ~= "table" and type(reference) ~= "userdata" then return nil end
    return reference.entity ~= 0 and reference.entity or nil
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
    method(reference, first, second) return true
end

function PatrolPath:init()
    RegisterEngineEvent(self, "onUpdate")
    self.speed = math.max(tonumber(self.speed) or 2.0, 0.0)
    self.turnSpeed = math.max(tonumber(self.turnSpeed) or 180.0, 0.0)
    self.reachedDistance = math.max(tonumber(self.reachedDistance) or 0.3, 0.01)
    self.index, self.forward, self.paused = 1, true, false
    self.yaw, self.waiting, self.points, self.lastWarn = 0.0, 0.0, {}, 0.0
    local ok, object = pcall(function() return Object(self.scene, self.entity) end)
    self.object = (ok and object) or nil
    self:refresh()
end

function PatrolPath:warn(message)
    if Engine.systemTime - self.lastWarn <= 1.0 then return end
    self.lastWarn = Engine.systemTime print("[PatrolPath] " .. message)
end

-- Lua cannot enumerate the scene, so a route is built only from names and from the four slots.
function PatrolPath:refresh()
    local points = {}
    for part in tostring(self.waypointNames or ""):gmatch("[^,]+") do
        local name = part:match("^%s*(.-)%s*$") local id = name ~= "" and entityId(self.scene:findEntity(name)) or nil
        if id then points[#points + 1] = { id = id, name = name } end
    end
    for _, slot in ipairs({ self.waypointA, self.waypointB, self.waypointC, self.waypointD }) do
        local id = entityId(slot) if id then points[#points + 1] = { id = id, name = "waypoint" .. tostring(#points + 1) } end
    end
    self.points, self.index = points, clamp(self.index, 1, math.max(#points, 1))
    return #points
end

function PatrolPath:steer(x, z)
    local ok, body = pcall(function() return Body3D(self.scene, entityId(self.subjectBody) or self.entity) end)
    if not ok or not body then return false end
    -- Gravity owns Y, so only the horizontal velocity is steered.
    body.linearVelocity = Vector3(x, (body.linearVelocity and tonumber(body.linearVelocity.y)) or 0.0, z)
    return true
end

function PatrolPath:advance()
    local last, point = math.max(#self.points, 1), self.points[self.index]
    callMethod(self.onWaypointEntity, self.onWaypointMethod, point and point.id, self.entity)
    self.waiting = math.max(tonumber(self.pauseAtEach) or 0.0, 0.0)
    local wanted = self.index + (self.forward and 1 or -1)
    if wanted >= 1 and wanted <= last then self.index = wanted return end
    if self.pingPong == true and last > 1 then
        self.forward = not self.forward self.index = self.index + (self.forward and 1 or -1)
    elseif self.loop == true then
        self.forward, self.index = true, (self.index % last) + 1
        callMethod(self.onLoopEntity, self.onLoopMethod, self.entity, self.index)
    else
        self.paused = true
    end
end

function PatrolPath:turnTo(dx, dz, dt)
    if dx == 0.0 and dz == 0.0 then return end
    local arc = (math.deg(math.atan(dx, dz)) - self.yaw) % 360.0 if arc > 180.0 then arc = arc - 360.0 end
    local step = self.turnSpeed > 0.0 and self.turnSpeed * dt or 360.0
    self.yaw = self.yaw + clamp(arc, -step, step)
    if self.object then self.object.rotation = Quaternion(0.0, angle(self.yaw), 0.0) end
end

function PatrolPath:gotoWaypoint(target)
    local wanted = type(target) == "number" and clamp(math.floor(target), 1, math.max(#self.points, 1)) or nil
    if type(target) == "string" then
        for i, point in ipairs(self.points) do if point.name == target then wanted = i break end end
    end
    if not wanted then return false end
    self.forward, self.paused, self.waiting = wanted >= self.index, false, 0.0
    self.index = wanted
    return true
end

function PatrolPath:next() self:advance() return self.index end
function PatrolPath:pause() self.paused = true if self.mode == 1 then self:steer(0.0, 0.0) end end
function PatrolPath:resume() self.paused, self.waiting = false, 0.0 end
function PatrolPath:getCurrent() local p = self.points[self.index] return self.index, p and p.id, p and p.name end
function PatrolPath:getProgress()
    local last = #self.points
    return last == 0 and 0.0 or clamp((self.index - 1) / last, 0.0, 1.0)
end

function PatrolPath:onUpdate()
    if not self.object then self:warn("this entity needs a Transform component") return end
    if #self.points == 0 and self:refresh() == 0 then
        self:warn("wire this: name the waypoints or fill the Waypoint A..D slots")
        return
    end
    local dt, here = math.min(Engine.deltatime, 0.05), self.object.position
    local target = positionOf(self.scene, self.points[self.index].id)
    if not target then self:warn("a waypoint entity is gone") self:advance() return end
    local tx, tz = target.x - here.x, target.z - here.z
    local travel = math.sqrt(tx * tx + tz * tz)
    if self.facing == 1 and travel > self.reachedDistance then
        self:turnTo(tx / travel, tz / travel, dt)
    elseif self.facing == 2 then
        local id = entityId(self.lookAt) or entityId(self.scene:findEntity(self.fallbackName or ""))
        local watched = positionOf(self.scene, id)
        if watched then self:turnTo(watched.x - here.x, watched.z - here.z, dt) end
    end
    if self.paused or self.waiting > 0.0 then
        self.waiting = math.max(self.waiting - dt, 0.0)
        if self.mode == 1 then self:steer(0.0, 0.0) end
        return
    end
    if travel <= self.reachedDistance then self:advance() return end
    local unitX, unitZ = tx / travel, tz / travel
    if self.mode == 1 and not self:steer(unitX * self.speed, unitZ * self.speed) then
        self:warn("the Body3D slot is empty, so it drives by transform") self.mode = 0
    end
    if self.mode ~= 1 then
        local step = math.min(self.speed * dt, travel)
        self.object.position = Vector3(here.x + unitX * step, here.y, here.z + unitZ * step)
    end
end

return PatrolPath
