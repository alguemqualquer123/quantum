-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Kinematic platform that travels between two anchor entities (or along an offset) and carries
-- category: Environment & Motion
-- riders. Attach to the platform entity (Transform); each rider needs a Body3D and arrives
-- through the Rider slots or the comma-separated Riders names (Lua cannot query overlaps).
local MovePlatform = {
    properties = {
        { name = "from", displayName = "From anchor", type = "entity" },
        { name = "to", displayName = "To anchor", type = "entity" },
        { name = "offset", displayName = "Offset when anchors are empty (m)", type = "vector3", default = { 0.0, 2.0, 0.0 } },
        { name = "speed", displayName = "Speed (m/s)", type = "float", default = 2.0 },
        { name = "ease", displayName = "Ease (0 linear .. 1 smooth start/stop)", type = "float", default = 1.0 },
        { name = "pingPong", displayName = "Ping pong (false = jump back)", type = "bool", default = true },
        { name = "loop", displayName = "Loop the trip", type = "bool", default = true },
        { name = "pauseAtEach", displayName = "Pause at each end (seconds)", type = "float", default = 0.5 },
        { name = "lockX", displayName = "Lock the X axis", type = "bool", default = false },
        { name = "lockY", displayName = "Lock the Y axis", type = "bool", default = false },
        { name = "lockZ", displayName = "Lock the Z axis", type = "bool", default = false },
        { name = "carryRadius", displayName = "Carry radius (metres)", type = "float", default = 1.2 },
        { name = "carryHeight", displayName = "Carry height above deck (metres)", type = "float", default = 0.8 },
        { name = "riderA", displayName = "Rider A", type = "entity" },
        { name = "riderB", displayName = "Rider B", type = "entity" },
        { name = "riderC", displayName = "Rider C", type = "entity" },
        { name = "riders", displayName = "Rider names (comma separated)", type = "string", default = "" },
        { name = "onArriveEntity", displayName = "On arrive: entity", type = "entity" },
        { name = "onArriveMethod", displayName = "On arrive: method", type = "string", default = "onArrive" }
    }
}
local function clamp(v, lo, hi) if v < lo then return lo end; if v > hi then return hi end; return v end
-- A slot reaches Lua as a script table, an engine handle or a bare id, and 0 is NULL_ENTITY.
local function entityId(reference)
    if type(reference) == "number" then return reference ~= 0 and reference or nil end
    if reference and type(reference.entity) == "number" and reference.entity ~= 0 then return reference.entity end
    return nil
end
-- The engine delivers a vector row as Vector3 userdata and the Inspector default as a table; the
-- pcall keeps an entity handle from raising when it has no such channel.
local function asVec3(value, fallback)
    local ok, out = pcall(function()
        if type(value) ~= "table" and type(value) ~= "userdata" then return fallback end
        if type(value.x) == "number" then return Vector3(value.x, value.y or 0.0, value.z or 0.0) end
        if type(value[1]) == "number" then return Vector3(value[1], value[2] or 0.0, value[3] or 0.0) end
        return fallback
    end)
    return ok and out or fallback
end
local function warn(self, message)
    if Engine.systemTime - self.lastWarn > 1.0 then
        self.lastWarn = Engine.systemTime
        print("[MovePlatform] " .. message)
    end
end
function MovePlatform:init()
    RegisterEngineEvent(self, "onFixedUpdate")
    self.speed = math.max(self.speed or 2.0, 0.01)
    self.ease = clamp(self.ease or 1.0, 0.0, 1.0)
    self.pauseAtEach = math.max(self.pauseAtEach or 0.5, 0.0)
    self.carryRadius = math.max(self.carryRadius or 1.2, 0.05)
    self.carryHeight = math.max(self.carryHeight or 0.8, 0.1)
    self.offsetVec = asVec3(self.offset, Vector3(0.0, 2.0, 0.0))
    self.progress, self.direction, self.pauseTimer = 0.0, 1.0, 0.0
    self.stopped, self.lastWarn = false, 0.0
    self.lastDelta = Vector3(0.0, 0.0, 0.0)
end
function MovePlatform:getDelta() return self.lastDelta end
function MovePlatform:setProgress(t) if type(t) == "number" then self.progress = clamp(t, 0.0, 1.0) end end
function MovePlatform:pause() self.stopped = true end
function MovePlatform:resume() self.stopped = false end
function MovePlatform:nextWaypoint() self.progress = self.direction > 0.0 and 0.999 or 0.001 end
function MovePlatform:anchorPos(slot)
    local id = entityId(slot)
    if not id then return nil end
    local ok, position = pcall(function() return Object(self.scene, id).position end)
    if ok and position and type(position.x) == "number" then return position end
    return nil
end
function MovePlatform:onFixedUpdate()
    local dt = math.min(Engine.deltatime, 0.05)
    local okObject, object = pcall(function() return Object(self.scene, self.entity) end)
    if not okObject or not object then
        warn(self, "The entity needs a Transform component.")
        return
    end
    if not self.startPos then
        local p = object.position; self.startPos = Vector3(p.x, p.y, p.z)
    end
    local a = self:anchorPos(self.from) or self.startPos
    local hasOffset = self.offsetVec.x ~= 0.0 or self.offsetVec.y ~= 0.0 or self.offsetVec.z ~= 0.0
    local toPos = self:anchorPos(self.to)
    if toPos == nil and not self:anchorPos(self.from) and not hasOffset then
        warn(self, "Wire the From/To anchors or set a non-zero Offset: the platform cannot move.")
        return
    end
    local b = toPos or (self.startPos + self.offsetVec)
    if self.pauseTimer > 0.0 then
        self.pauseTimer = self.pauseTimer - dt
        self.lastDelta = Vector3(0.0, 0.0, 0.0)
        return
    end
    if not self.stopped then
        self.progress = self.progress + self.direction * (self.speed / math.max((b - a):length(), 0.001)) * dt
        if self.progress >= 1.0 or self.progress <= 0.0 then
            self.progress = clamp(self.progress, 0.0, 1.0)
            self.pauseTimer = self.pauseAtEach
            self:arrive()
        end
    end

    -- ease 0 runs straight through, ease 1 uses a smoothstep that starts and stops soft.
    local t = self.progress
    local eased = t + (t * t * (3.0 - 2.0 * t) - t) * self.ease
    local target = Vector3(a.x + (b.x - a.x) * eased, a.y + (b.y - a.y) * eased, a.z + (b.z - a.z) * eased)
    if self.lockX == true then target.x = self.startPos.x end; if self.lockY == true then target.y = self.startPos.y end
    if self.lockZ == true then target.z = self.startPos.z end
    local previous = object.position
    self.lastDelta = Vector3(target.x - previous.x, target.y - previous.y, target.z - previous.z)
    object.position = target
    self:carryRiders(target, self.lastDelta, dt)
end

function MovePlatform:arrive()
    if self.pingPong ~= false then
        self.direction = -self.direction
    elseif self.loop ~= false then
        self.progress = self.direction > 0.0 and 0.0 or 1.0
    else
        self.stopped = true
    end
    local target, method = self.onArriveEntity, self.onArriveMethod
    -- A hook slot only holds a script table when the target really carries that script.
    if type(target) ~= "table" or not method or method == "" then return end
    pcall(function()
        local fn = target[method]
        if type(fn) == "function" then fn(target) end
    end)
end

function MovePlatform:carryRiders(platformPos, delta, dt)
    if delta.x == 0.0 and delta.y == 0.0 and delta.z == 0.0 then return end
    local seen = {}
    local function use(id)
        if not id or seen[id] then return end
        seen[id] = true
        local ok, body = pcall(function() return Body3D(self.scene, id) end)
        if not (ok and body) then return end
        pcall(function()
            local position = body.position
            local dx = position.x - platformPos.x
            local dz = position.z - platformPos.z
            local dy = position.y - platformPos.y
            -- Riders are matched by distance, not by collider overlap (no overlap query in Lua).
            if dx * dx + dz * dz > self.carryRadius * self.carryRadius
                or dy > self.carryHeight or dy < -0.5 then
                return
            end
            body.position = Vector3(position.x + delta.x, position.y + delta.y, position.z + delta.z)
            if delta.y > 0.0 then
                -- Rising deck: match the vertical velocity so the solver does not sink the rider.
                local velocity = body.linearVelocity
                body.linearVelocity = Vector3(velocity.x, delta.y / math.max(dt, 0.0001), velocity.z)
            end
        end)
    end
    use(entityId(self.riderA)); use(entityId(self.riderB)); use(entityId(self.riderC))
    for token in string.gmatch(self.riders or "", "[^,]+") do
        use(entityId(self.scene:findEntity(token:match("^%s*(.-)%s*$"))))
    end
end

return MovePlatform
