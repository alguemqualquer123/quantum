-- (c) SR VINIX
-- SPDX-License-Identifier: MIT
-- Precipitation you can see: a Points sprite sheet driven by a Particles action, rebuilt for rain,
-- snow or hail, kept centred on the camera and blown by the wind. Builds its own two entities, so
-- dropping the script on any entity is enough; a texture path is optional (without one the drops
-- are the engine's default sprite).
-- category: Environment & Motion
local Precipitation = {
    properties = {
        { name = "kind", displayName = "0 rain, 1 snow, 2 hail", type = "int", default = 0 },
        { name = "intensity", displayName = "Intensity (0..1)", type = "float", default = 1.0 },
        { name = "density", displayName = "Particles per second at full intensity", type = "float", default = 420.0 },
        { name = "area", displayName = "Emitter size (m, x and z)", type = "float", default = 60.0 },
        { name = "spawnHeight", displayName = "Spawn height above the camera (m)", type = "float", default = 26.0 },
        { name = "particleSize", displayName = "Drop size (m, 0 = per kind)", type = "float", default = 0.0 },
        { name = "fallSpeed", displayName = "Fall speed (m/s, 0 = per kind)", type = "float", default = 0.0 },
        { name = "windDirection", displayName = "Wind from (degrees)", type = "float", default = 35.0 },
        { name = "windSpeed", displayName = "Wind speed (m/s)", type = "float", default = 3.0 },
        { name = "sway", displayName = "Snow sway (m/s sideways)", type = "float", default = 1.2 },
        { name = "color", displayName = "Tint (r, g, b)", type = "color3", default = { 0.8, 0.86, 0.95 } },
        { name = "alpha", displayName = "Opacity (0..1)", type = "float", default = 0.7 },
        { name = "streak", displayName = "Rain streak length (x the size, 0 = off)", type = "float", default = 6.0 },
        { name = "texture", displayName = "Sprite texture ('' = engine default)", type = "string", default = "" },
        { name = "frameName", displayName = "Frame name inside the sheet", type = "string", default = "drop" },
        { name = "frameU", displayName = "Frame rect U (0..1)", type = "float", default = 0.0 },
        { name = "frameV", displayName = "Frame rect V (0..1)", type = "float", default = 0.0 },
        { name = "frameW", displayName = "Frame rect width (0..1)", type = "float", default = 1.0 },
        { name = "frameH", displayName = "Frame rect height (0..1)", type = "float", default = 1.0 },
        { name = "followCamera", displayName = "Keep the emitter over the camera", type = "bool", default = true },
        { name = "pointsEntity", displayName = "Points entity (empty = make one)", type = "entity" },
        { name = "particlesEntity", displayName = "Particles entity (empty = make one)", type = "entity" },
        { name = "weather", displayName = "WeatherSystem component", type = "WeatherSystem" },
        { name = "followWeather", displayName = "Scale with the weather intensity", type = "bool", default = true },
        { name = "fallbackWeatherName", displayName = "Weather entity name if slot is empty", type = "string", default = "Weather" },
        { name = "startOnLoad", displayName = "Start as soon as the scene loads", type = "bool", default = true }
    }
}

local function clamp(v, lo, hi) if v < lo then return lo end; if v > hi then return hi end; return v end
local function number(v, fallback)
    if type(v) ~= "number" or v ~= v then return fallback end
    return v
end
local function entityId(reference)
    if type(reference) == "number" then return reference ~= 0 and reference or nil end
    if reference and type(reference.entity) == "number" then
        return reference.entity ~= 0 and reference.entity or nil
    end
    return nil
end
local function vec3(value, fallback)
    local ok, out = pcall(function()
        if type(value) ~= "table" and type(value) ~= "userdata" then return nil end
        if type(value.x) == "number" then
            return Vector3(value.x, number(value.y, value.x), number(value.z, value.x))
        end
        if type(value[1]) == "number" then
            return Vector3(value[1], number(value[2], value[1]), number(value[3], value[1]))
        end
        return nil
    end)
    return (ok and out) or Vector3(fallback[1], fallback[2], fallback[3])
end
local function rectOf(self)
    return Rect(clamp(number(self.frameU, 0.0), 0.0, 1.0), clamp(number(self.frameV, 0.0), 0.0, 1.0),
        clamp(number(self.frameW, 1.0), 0.001, 1.0), clamp(number(self.frameH, 1.0), 0.001, 1.0))
end
-- One row per look. A 0 size or speed means "use this row", and the per-property override still
-- wins, so the Inspector can fine-tune a preset without the component forgetting what it is.
local presets = {
    [0] = { size = 0.05, fall = 16.0, alpha = 0.55, color = { 0.75, 0.82, 0.92 }, streak = 6.0, sway = 0.0 },
    [1] = { size = 0.16, fall = 1.9, alpha = 0.95, color = { 1.0, 1.0, 1.0 }, streak = 0.0, sway = 1.2 },
    [2] = { size = 0.1, fall = 26.0, alpha = 1.0, color = { 0.9, 0.95, 1.0 }, streak = 0.0, sway = 0.0 }
}

function Precipitation:init()
    RegisterEngineEvent(self, "onUpdate")
    self.kind = math.floor(clamp(number(self.kind, 0), 0, 2))
    self.intensity = clamp(number(self.intensity, 1.0), 0.0, 1.0)
    self.area = math.max(number(self.area, 60.0), 2.0)
    self.spawnHeight = math.max(number(self.spawnHeight, 26.0), 2.0)
    self.lastWarn = 0.0
    self.active = self.startOnLoad ~= false
    self:build()
    self:apply()
end

function Precipitation:warn(message)
    if Engine.systemTime - self.lastWarn > 1.0 then
        self.lastWarn = Engine.systemTime; print("[Precipitation] " .. message)
    end
end

-- Points draws the sprites and Particles moves them; the action renders only what it targets, so
-- both entities are needed and the link between them is set here rather than in the editor.
function Precipitation:build()
    if self.points and self.particles then return true end
    local scene = self.scene

    if not self.points then
        local id = entityId(self.pointsEntity)
        if not id then id = entityId(scene:findEntity("PrecipitationSprites")) end
        local ok, handle = pcall(function()
            if id then return Points(scene, id) end
            local made = Points(scene)
            made.name = "PrecipitationSprites"
            return made
        end)
        self.points = (ok and handle) or nil
    end
    if not self.particles then
        local id = entityId(self.particlesEntity)
        if not id then id = entityId(scene:findEntity("Precipitation")) end
        local ok, handle = pcall(function()
            if id then return Particles(scene, id) end
            local made = Particles(scene)
            made.name = "Precipitation"
            return made
        end)
        self.particles = (ok and handle) or nil
    end
    if not self.points or not self.particles then
        self:warn("needs a Points entity and a Particles entity")
        return false
    end

    pcall(function()
        -- The action owns the pool: starting it sizes both the particle array and the sprite
        -- array from maxParticles, and copies that number onto Points.maxPoints, so this is the
        -- property that actually sets how many drops can be alive at once.
        self.particles.maxParticles = math.floor(clamp(number(self.density, 420.0) * 4.0, 64, 8192))
        self.points.transparent = true
    end)
    -- The sprite sheet is optional and lives in its own pcall: a texture path that does not resolve
    -- must not also lose the target link below, which is the only thing that makes drops move.
    local path = self.texture
    if path and path ~= "" then
        pcall(function()
            self.points:setTexture(path)
            local rect = rectOf(self)
            self.points:addSpriteFrame(self.frameName, rect.x, rect.y, rect.w, rect.h)
            self.particles:setSpriteIntializer(0, 0)
        end)
    end
    pcall(function()
        -- localSpace keeps the drops in the Points entity's own space, which is what makes
        -- onUpdate's follow move carry the rain along with it.
        self.particles.target = self.points.entity
        self.particles.localSpace = true
        self.particles.loop = true
    end)
    return true
end

function Precipitation:preset()
    return presets[self.kind] or presets[0]
end

-- The whole look is rebuilt from the properties: the particle system has no per-field setters
-- worth chasing, and re-running the initialisers is cheaper than tracking which one changed.
function Precipitation:apply()
    if not self.particles then
        if not self:build() then return end
    end
    local preset = self:preset()
    local size = math.max(number(self.particleSize, 0.0), 0.0)
    if size <= 0.0 then size = preset.size end
    local fall = math.max(number(self.fallSpeed, 0.0), 0.0)
    if fall <= 0.0 then fall = preset.fall end
    local alpha = clamp(number(self.alpha, preset.alpha), 0.0, 1.0)
    local streak = number(self.streak, preset.streak)
    if streak < 0.0 then streak = 0.0 end
    local sway = math.max(number(self.sway, preset.sway), 0.0)
    local tint = vec3(self.color, preset.color)

    local radians = math.rad(number(self.windDirection, 35.0))
    local wind = number(self.windSpeed, 3.0)
    local vx = math.cos(radians) * wind
    local vz = math.sin(radians) * wind

    -- Life is the time to cross the emitter box, so a drop that spawns at the top reaches the
    -- bottom exactly when it is recycled - too long and old drops pile up below the camera.
    local life = clamp(self.spawnHeight / math.max(fall, 0.2), 0.2, 12.0)
    local half = self.area * 0.5

    pcall(function()
        local particles = self.particles
        particles:setLifeInitializer(life * 0.6, life)
        particles:setPositionInitializer(Vector3(-half, self.spawnHeight, -half),
            Vector3(half, self.spawnHeight * 1.15, half))
        particles:setVelocityInitializer(Vector3(vx - sway * 0.35, -fall, vz - sway * 0.35),
            Vector3(vx + sway * 0.35, -fall, vz + sway * 0.35))
        particles:setVelocityModifier(0.0, life,
            Vector3(vx - sway, -fall, vz - sway), Vector3(vx + sway, -fall, vz + sway))
        particles:setAccelerationInitializer(Vector3(0.0, self.kind == 2 and -18.0 or -1.2, 0.0))
        particles:setSizeInitializer(size)
        if self.kind == 0 and streak > 0.0 then
            particles:setScaleInitializer(Vector3(1.0, 1.0 + streak, 1.0))
        else
            particles:setScaleInitializer(Vector3(1.0, 1.0, 1.0))
        end
        particles:setColorInitializer(tint)
        particles:setAlphaInitializer(alpha)
        if self.kind == 1 then
            particles:setRotationInitializer(0.0, 360.0)
            particles:setRotationModifier(0.0, life, 0.0, 180.0)
        end
        self:restart()
    end)
end

-- An action only allocates its particle pool when it starts, so every rebuild of the look has to
-- go through restart(): without it the emitter has nowhere to put a drop.
function Precipitation:restart()
    pcall(function() self.particles:restart() end)
    self:updateRate()
end

function Precipitation:weatherIntensity()
    if self.followWeather == false then return 1.0 end
    local weather = self.weather
    if type(weather) ~= "table" or type(weather.getIntensity) ~= "function" then
        local id = entityId(self.scene:findEntity(self.fallbackWeatherName))
        if id then
            pcall(function()
                local holder = Object(self.scene, id)
                if holder and type(holder.weather) == "table" then weather = holder.weather end
            end)
        end
    end
    if type(weather) == "table" and type(weather.getIntensity) == "function" then
        local value = nil
        pcall(function() value = weather:getIntensity() end)
        if type(value) == "number" then return clamp(value, 0.0, 1.0) end
    end
    return 1.0
end

function Precipitation:updateRate()
    if not self.particles then return end
    local scale = self.active and self.intensity * self:weatherIntensity() or 0.0
    local rate = math.floor(clamp(number(self.density, 420.0) * scale, 0, 100000.0))
    pcall(function()
        self.particles.rate = rate
        self.particles.emitter = rate > 0
        -- The engine stops a finished action on its own, so a rate that climbs back over zero
        -- has to restart it - leaving the emitter flag open on a stopped action does nothing.
        if rate > 0 and not self.particles:isRunning() then
            self.particles:restart()
        end
    end)
end

function Precipitation:setIntensity(value)
    self.intensity = clamp(number(value, 1.0), 0.0, 1.0)
    self:updateRate()
end
function Precipitation:setKind(value)
    self.kind = math.floor(clamp(number(value, 0), 0, 2))
    self:apply()
end
function Precipitation:rain() self:setKind(0) end
function Precipitation:snow() self:setKind(1) end
function Precipitation:hail() self:setKind(2) end
function Precipitation:setWind(direction, speed)
    self.windDirection = number(direction, self.windDirection)
    self.windSpeed = number(speed, self.windSpeed)
    self:apply()
end
function Precipitation:start() self.active = true; self:updateRate() end
function Precipitation:stop() self.active = false; self:updateRate() end
function Precipitation:isActive() return self.active end
function Precipitation:getCount()
    local out = 0
    if self.points then pcall(function() out = self.points:getNumPoints() end) end
    return out
end

function Precipitation:onUpdate()
    if self.followCamera == false then return end
    if not self.particles then return end
    local id
    pcall(function() id = entityId(self.scene.camera) end)
    if not id then return end
    local eye
    pcall(function() eye = Object(self.scene, id).position end)
    if not eye or type(eye.x) ~= "number" then return end
    -- The sprites entity is the one with a Transform: the action entity is a bare Action +
    -- Particles pair, so writing a position onto it would only make the engine complain.
    pcall(function() self.points.position = Vector3(eye.x, eye.y, eye.z) end)
    self:updateRate()
end

return Precipitation
