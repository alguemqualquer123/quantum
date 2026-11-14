-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- The single source of truth for wind: a drifting direction with gusts, forwarded to a
-- category: Environment & Motion
-- WaterWaves component and pushed onto explicitly wired bodies (Lua cannot query overlaps).
local WindZone = {
    properties = {
        { name = "direction", displayName = "Wind direction (degrees)", type = "float", default = 35.0 },
        { name = "drift", displayName = "Direction drift (degrees)", type = "float", default = 3.0 },
        { name = "strength", displayName = "Wind strength (m/s^2)", type = "float", default = 2.0 },
        { name = "gustInterval", displayName = "Gust interval (seconds)", type = "float", default = 9.0 },
        { name = "gustStrength", displayName = "Gust strength (multiplier)", type = "float", default = 2.5 },
        { name = "gustRamp", displayName = "Gust ramp (seconds)", type = "float", default = 0.6 },
        { name = "waves", displayName = "WaterWaves component", type = "WaterWaves" },
        { name = "radius", displayName = "Push radius (metres)", type = "float", default = 12.0 },
        { name = "push", displayName = "Push bodies", type = "bool", default = true },
        { name = "factor", displayName = "Push factor", type = "float", default = 1.0 },
        { name = "subject", displayName = "Body to push (A)", type = "entity" },
        { name = "targetA", displayName = "Body to push (B)", type = "entity" },
        { name = "targetB", displayName = "Body to push (C)", type = "entity" },
        { name = "targetC", displayName = "Body to push (D)", type = "entity" },
        { name = "watchNames", displayName = "Body names to push (comma separated)", type = "string", default = "" }
    }
}

local function clamp(value, low, high)
    if value < low then return low end
    if value > high then return high end
    return value
end

-- A slot reaches Lua as a script table, an engine handle or a bare id, and 0 is NULL_ENTITY.
local function entityId(reference)
    if type(reference) == "number" then return reference ~= 0 and reference or nil end
    if reference and type(reference.entity) == "number" then
        return reference.entity ~= 0 and reference.entity or nil
    end
    return nil
end

local function warn(self, message)
    if Engine.systemTime - self.lastWarn > 1.0 then
        self.lastWarn = Engine.systemTime
        print("[WindZone] " .. message)
    end
end

-- Incommensurate sines read as weather; the engine has no noise function for Lua.
local function wander(t)
    return math.sin(t * 0.13) * 0.55 + math.sin(t * 0.371 + 1.7) * 0.25
        + math.sin(t * 0.0417 + 4.2) * 0.45
end

function WindZone:init()
    RegisterEngineEvent(self, "onUpdate")
    self.direction = self.direction or 35.0
    self.drift = self.drift or 3.0
    self.strength = math.max(self.strength or 2.0, 0.0)
    self.gustInterval = math.max(self.gustInterval or 9.0, 0.5)
    self.gustStrength = math.max(self.gustStrength or 2.5, 1.0)
    self.gustRamp = math.max(self.gustRamp or 0.6, 0.05)
    self.radius = math.max(self.radius or 12.0, 0.1)
    self.factor = self.factor or 1.0
    self.windDir = self.direction
    self.currentStrength = 0.0
    self.gustTimer, self.gustTime, self.gustScale = self.gustInterval, 0.0, 1.0
    self.lastWarn = 0.0
    self.bodies = nil
end

function WindZone:resolveBodies()
    local bodies = {}
    local seen = {}
    local function use(id)
        if not id or seen[id] then return end
        seen[id] = true
        local ok, body = pcall(function() return Body3D(self.scene, id) end)
        if ok and body then bodies[#bodies + 1] = body end
    end
    use(entityId(self.subject)); use(entityId(self.targetA))
    use(entityId(self.targetB)); use(entityId(self.targetC))
    for token in string.gmatch(self.watchNames or "", "[^,]+") do
        use(entityId(self.scene:findEntity(token:match("^%s*(.-)%s*$"))))
    end
    self.bodies = bodies
end

function WindZone:getDirection() return self.windDir end
function WindZone:getStrength() return self.currentStrength end

function WindZone:setGust(scale)
    self.gustScale = clamp(type(scale) == "number" and scale or 1.0, 0.0, 4.0)
    self.gustTime = self.gustRamp
end

function WindZone:calm()
    self.gustTime, self.gustScale = 0.0, 1.0
    self.currentStrength = 0.0
    self.gustTimer = self.gustInterval
end

function WindZone:onUpdate()
    local dt = math.min(Engine.deltatime, 0.05)
    self.windDir = self.direction + wander(Engine.systemTime) * self.drift * 3.0

    self.gustTimer = self.gustTimer - dt
    if self.gustTimer <= 0.0 then
        self.gustTimer = self.gustInterval * (0.7 + math.random() * 0.6)
        self.gustTime = self.gustRamp
        self.gustScale = 1.0
        local waves = self.waves
        if type(waves) == "table" and type(waves.gust) == "function" then
            pcall(function() waves:gust(self.gustStrength * 0.5) end)
        end
    end

    -- A gust breathes in and out over gustRamp; the base strength eases toward the result.
    local envelope = 0.0
    if self.gustTime > 0.0 then
        self.gustTime = self.gustTime - dt
        local half = math.max(self.gustRamp * 0.5, 0.001)
        envelope = clamp(1.0 - math.abs(self.gustTime - half) / half, 0.0, 1.0) * self.gustScale
    end
    local desired = self.strength * (1.0 + math.max(self.gustStrength - 1.0, 0.0) * envelope)
    self.currentStrength = self.currentStrength + (desired - self.currentStrength) * clamp(dt * 3.0, 0.0, 1.0)

    local waves = self.waves
    if type(waves) == "table" and type(waves.setWindDirection) == "function" then
        pcall(function() waves:setWindDirection(((self.windDir % 360.0) + 360.0) % 360.0) end)
    elseif type(waves) == "userdata" then
        -- A script slot only answers when the wired entity really carries that script.
        warn(self, "wire this: a WaterWaves component on the entity in the WaterWaves slot")
    end

    if self.push == false then return end
    if not self.bodies then self:resolveBodies() end
    if #self.bodies == 0 then
        warn(self, "No bodies wired to push: use Subject/Target A..C or a comma list in "
            .. "Watch Names (Lua cannot query overlaps).")
        return
    end
    local okOrigin, object = pcall(function() return Object(self.scene, self.entity) end)
    if not okOrigin or not object then
        warn(self, "The zone entity has no transform to measure the radius from.")
        return
    end

    local origin = object.position
    local radians = math.rad(self.windDir)
    local forceDir = Vector3(math.sin(radians), 0.0, math.cos(radians))
    local limit = self.radius * self.radius
    for _, body in ipairs(self.bodies) do
        local ok = pcall(function()
            local position = body.position
            local dx = position.x - origin.x
            local dy = position.y - origin.y
            local dz = position.z - origin.z
            if dx * dx + dy * dy + dz * dz <= limit then
                local mass = body.mass
                if type(mass) ~= "number" or mass <= 0.0 then mass = 1.0 end
                body:applyForce(forceDir * (self.currentStrength * self.factor * mass))
            end
        end)
        if not ok then
            warn(self, "A wired entity has no Body3D to push: check the slots and names.")
        end
    end
end

return WindZone
