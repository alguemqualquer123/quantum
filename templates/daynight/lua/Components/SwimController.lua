-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Swimming character for a Body3D: surfaces and dives through the water with hysteresis, steers
-- category: Water & Shore
-- the body through it, restores its drag on exit. Attach next to a Body3D; keys match PhysicsMover3D.
local SwimController = {
    properties = {
        { name = "water", displayName = "Water", type = "entity" },
        { name = "fallbackName", displayName = "Water name if slot is empty", type = "string", default = "Water" },
        { name = "waves", displayName = "Waves driver", type = "WaterWaves" },
        { name = "swimSpeed", displayName = "Swim speed (m/s)", type = "float", default = 2.6 },
        { name = "sinkSpeed", displayName = "Dive speed (m/s)", type = "float", default = 1.6 },
        { name = "maxDepth", displayName = "Max dive depth below surface (metres)", type = "float", default = 4.0 },
        { name = "eyeHeight", displayName = "Eye height (metres)", type = "float", default = 1.6 },
        { name = "autoSurface", displayName = "Keep the head above water", type = "bool", default = true },
        { name = "waterDrag", displayName = "Water drag while swimming (1/s)", type = "float", default = 2.0 },
        { name = "waterAngularDrag", displayName = "Water angular drag while swimming (1/s)", type = "float", default = 3.0 },
        { name = "keyForward", displayName = "Forward key (0 = W)", type = "int", default = 0 },
        { name = "keyBack", displayName = "Back key (0 = S)", type = "int", default = 0 },
        { name = "keyLeft", displayName = "Left key (0 = A)", type = "int", default = 0 },
        { name = "keyRight", displayName = "Right key (0 = D)", type = "int", default = 0 },
        { name = "jumpKey", displayName = "Surface key (0 = space)", type = "int", default = 0 },
        { name = "diveKey", displayName = "Dive key (0 = C)", type = "int", default = 0 },
        { name = "splashEntity", displayName = "Splash: entity", type = "entity" },
        { name = "splashMethod", displayName = "Splash: method", type = "string", default = "onSplash" }
    }
}

local function clamp(v, lo, hi) if v < lo then return lo end if v > hi then return hi end return v end

local function entityId(ref)
    if type(ref) == "number" then return ref ~= 0 and ref or nil end
    if ref and type(ref.entity) == "number" then return ref.entity ~= 0 and ref.entity or nil end
    return nil
end

-- 0 in a key property means "use the component's default", like PhysicsMover3D.
local function keyOf(value, fallback) return (not value or value == 0) and fallback or value end

local function warn(self, message)
    if Engine.systemTime - self.lastWarn > 1.0 then
        self.lastWarn = Engine.systemTime
        print("[SwimController] " .. message)
    end
end

function SwimController:init()
    RegisterEngineEvent(self, "onFixedUpdate")
    self.swimSpeed, self.sinkSpeed = self.swimSpeed or 2.6, self.sinkSpeed or 1.6
    self.maxDepth = math.max(self.maxDepth or 4.0, 0.2)
    self.eyeHeight = math.max(self.eyeHeight or 1.6, 0.2)
    self.keyForward, self.keyBack = keyOf(self.keyForward, Input.KEY_W), keyOf(self.keyBack, Input.KEY_S)
    self.keyLeft, self.keyRight = keyOf(self.keyLeft, Input.KEY_A), keyOf(self.keyRight, Input.KEY_D)
    self.jumpKey, self.diveKey = keyOf(self.jumpKey, Input.KEY_SPACE), keyOf(self.diveKey, Input.KEY_C)
    -- dry / surfaced / diving; the thresholds below give hysteresis so wading does not flicker.
    self.state, self.savedDrag, self.lastWarn = "dry", nil, 0.0
    local ok, body = pcall(function()
        return Body3D(self.scene, self.entity)
    end)
    self.body = (ok and body) or nil
end

function SwimController:resolveWater()
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

function SwimController:onStateChanged(from, to)
    -- A script slot only answers when the wired entity really carries that script.
    if type(self.waves) == "table" and type(self.waves.gust) == "function" then
        pcall(function() self.waves:gust(to == "diving" and 0.6 or 0.35) end)
    elseif type(self.waves) == "userdata" then
        warn(self, "wire this: a WaterWaves component on the entity in the Waves driver slot")
    end
    local method = type(self.splashEntity) == "table" and self.splashEntity[self.splashMethod or ""] or nil
    if type(method) == "function" then pcall(method, self.splashEntity) end
end

function SwimController:enterWater()
    local from = self.state
    self.state = "surfaced"
    self.savedDrag = { linear = self.body.waterDrag, angular = self.body.waterAngularDrag }
    self.body.waterDrag, self.body.waterAngularDrag = self.waterDrag or 2.0, self.waterAngularDrag or 3.0
    self:onStateChanged(from, "surfaced")
end

function SwimController:leaveWater()
    local from = self.state
    self.state = "dry"
    if self.savedDrag then
        self.body.waterDrag, self.body.waterAngularDrag = self.savedDrag.linear, self.savedDrag.angular
        self.savedDrag = nil
    end
    self:onStateChanged(from, "dry")
end

function SwimController:onFixedUpdate()
    if not self.body then
        warn(self, "The entity needs a Body3D component.")
        return
    end
    local water = self:resolveWater()
    if not water then
        if self.state ~= "dry" then self:leaveWater() end
        return
    end
    local dt = math.min(Engine.deltatime, 1.0 / 30.0)
    local position = self.body.position
    local surfaceY = water:getHeight(position.x, position.z)
    local submerged = clamp((surfaceY - position.y) / self.eyeHeight, 0.0, 1.0)
    if self.state == "dry" and submerged > 0.15 then
        self:enterWater()
    elseif self.state ~= "dry" and submerged < 0.05 then
        self:leaveWater()
    end
    if self.state == "dry" then return end

    local x, z = 0.0, 0.0
    if Input.isKeyPressed(self.keyForward) then z = z - 1.0 end
    if Input.isKeyPressed(self.keyBack) then z = z + 1.0 end
    if Input.isKeyPressed(self.keyLeft) then x = x - 1.0 end
    if Input.isKeyPressed(self.keyRight) then x = x + 1.0 end
    local length = math.sqrt(x * x + z * z)
    if length > 1.0 then x, z = x / length, z / length end

    local velocity = self.body.linearVelocity
    local y = velocity.y
    if self.state == "surfaced" then
        -- Diving needs intent (key) or being pushed deep by a wave; the surface key climbs back.
        if Input.isKeyPressed(self.diveKey) or submerged > 0.8 then
            self.state = "diving"
            self:onStateChanged("surfaced", "diving")
        end
        if Input.isKeyPressed(self.jumpKey) then
            y = self.swimSpeed
        elseif self.autoSurface then
            y = y + clamp((surfaceY - self.eyeHeight - position.y) * 6.0 - y, -8.0 * dt, 8.0 * dt)
        end
    else
        if Input.isKeyPressed(self.jumpKey) or submerged < 0.35 then
            self.state = "surfaced"
            self:onStateChanged("diving", "surfaced")
        elseif Input.isKeyPressed(self.diveKey) then
            y = math.max(-self.sinkSpeed, (surfaceY - self.maxDepth - position.y) * 2.0)
        else
            y = y - self.sinkSpeed * 0.25 * dt
        end
    end

    -- Gravity still owns Y on the surface, but while swimming the keys steer it too.
    local rate = clamp(6.0 * dt, 0.0, 1.0)
    self.body.linearVelocity = Vector3(velocity.x + (x * self.swimSpeed - velocity.x) * rate, y,
        velocity.z + (z * self.swimSpeed - velocity.z) * rate)
end

return SwimController
