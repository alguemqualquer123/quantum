-- (c) SR VINIX
-- SPDX-License-Identifier: MIT
-- Fire you can drop on any entity: a flame plume, smoke and embers as three Points+Particles
-- layers, a flickering point light, fuel that runs out, heat that hurts a subject and spreads to
-- the Burnable scripts in the scene. Rain puts it out. Everything it needs is built by itself.
-- category: Environment & Motion
local Fire = {
    properties = {
        { name = "burning", displayName = "Burning when the scene loads", type = "bool", default = true },
        { name = "intensity", displayName = "Intensity (0..1)", type = "float", default = 1.0 },
        { name = "fuel", displayName = "Fuel seconds (0 = never runs out)", type = "float", default = 0.0 },
        { name = "burnRate", displayName = "Fuel used per second at full intensity", type = "float", default = 1.0 },
        { name = "flameSize", displayName = "Flame width (m)", type = "float", default = 0.9 },
        { name = "flameHeight", displayName = "Flame height (m)", type = "float", default = 1.5 },
        { name = "flameRate", displayName = "Flame puffs per second", type = "float", default = 130.0 },
        { name = "flameTexture", displayName = "Flame sprite", type = "string", default = "textures/fire_puff.png" },
        { name = "colorCore", displayName = "Hot core (r, g, b)", type = "color3", default = { 1.0, 0.94, 0.72 } },
        { name = "colorTip", displayName = "Cool tip (r, g, b)", type = "color3", default = { 0.72, 0.16, 0.03 } },
        { name = "smoke", displayName = "Draw smoke", type = "bool", default = true },
        { name = "smokeRate", displayName = "Smoke puffs per second", type = "float", default = 15.0 },
        { name = "smokeColor", displayName = "Smoke tint (r, g, b)", type = "color3", default = { 0.30, 0.29, 0.28 } },
        { name = "smokeTexture", displayName = "Smoke sprite", type = "string", default = "textures/fire_smoke.png" },
        { name = "smokeHeight", displayName = "How high the smoke goes (m)", type = "float", default = 9.0 },
        { name = "embers", displayName = "Throw embers", type = "bool", default = true },
        { name = "emberRate", displayName = "Embers per second", type = "float", default = 12.0 },
        { name = "emberTexture", displayName = "Ember sprite", type = "string", default = "textures/fire_ember.png" },
        { name = "light", displayName = "Cast light", type = "bool", default = true },
        { name = "lightRange", displayName = "Light range (m)", type = "float", default = 14.0 },
        { name = "lightIntensity", displayName = "Light intensity", type = "float", default = 6.0 },
        { name = "flicker", displayName = "Flicker amount (0..1)", type = "float", default = 0.35 },
        { name = "flickerSpeed", displayName = "Flicker speed (Hz)", type = "float", default = 9.0 },
        { name = "windDirection", displayName = "Wind from (degrees)", type = "float", default = 35.0 },
        { name = "windSpeed", displayName = "Wind speed (m/s)", type = "float", default = 0.6 },
        { name = "weather", displayName = "WeatherSystem component", type = "WeatherSystem" },
        { name = "rainExtinguishes", displayName = "Rain puts the fire out", type = "bool", default = true },
        { name = "heatDamage", displayName = "Damage per second inside the radius", type = "float", default = 0.0 },
        { name = "damageRadius", displayName = "Heat radius (m)", type = "float", default = 1.4 },
        { name = "subject", displayName = "Entity to hurt", type = "entity" },
        { name = "fallbackSubjectName", displayName = "Subject name if slot is empty", type = "string", default = "" },
        { name = "health", displayName = "Subject Health component", type = "Health" },
        { name = "spreadRadius", displayName = "Spread to Burnables within (m, 0 = off)", type = "float", default = 0.0 },
        { name = "spreadInterval", displayName = "Spread check interval (s)", type = "float", default = 0.8 },
        { name = "soundPath", displayName = "Crackle loop ('' = silent)", type = "string", default = "sounds/fire_crackle.wav" },
        { name = "volume", displayName = "Crackle volume", type = "float", default = 0.55 },
        { name = "soundRadius", displayName = "Audible radius (m)", type = "float", default = 18.0 },
        { name = "startOnLoad", displayName = "Light it as soon as the scene loads", type = "bool", default = true }
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
local function rgb(value, fallback)
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
    if ok and out then return out end
    return Vector3(fallback[1], fallback[2], fallback[3])
end

-- Burnable publishes itself here so a fire can find what to spread to: Lua has no overlap query,
-- and a slot per neighbour is not something a level author should have to wire. A script state
-- outlives the scene that filled it, so every entry is checked against the scene before use.
local function burnables()
    if not _G.__quantumFire then _G.__quantumFire = { burnables = {} } end
    return _G.__quantumFire.burnables
end

-- A Fire built by a Burnable is told who owns it, which is how the spread loop avoids lighting the
-- same fuel twice: a Burnable that already has a flame is not a target.
function Fire:ownerBurnable()
    return self.owner
end

function Fire:warn(message)
    if not self.lastWarn or Engine.systemTime - self.lastWarn > 1.0 then
        self.lastWarn = Engine.systemTime; print("[Fire] " .. message)
    end
end

function Fire:ownerName()
    if self.cachedName then return self.cachedName end
    local ok, name = pcall(function() return self.scene:getEntityName(self.entity) end)
    self.cachedName = (ok and type(name) == "string" and name ~= "") and name or "Fire"
    return self.cachedName
end

function Fire:position()
    local ok, value = pcall(function() return Object(self.scene, self.entity).position end)
    if ok and value then return value end
    return Vector3(0, 0, 0)
end

-- One Points entity plus the Particles action that drives it. The action's entity has no
-- Transform, so the position lives on the sprite entity and the action runs in its space.
function Fire:makeLayer(key, entityName, texturePath)
    local scene = self.scene
    local layer = {}

    local okPoints, points = pcall(function()
        local made = Points(scene)
        made.name = entityName
        made.position = self:position()
        made.transparent = true
        return made
    end)
    if not okPoints or not points then return nil end
    layer.points = points

    local okParticles, particles = pcall(function()
        local made = Particles(scene)
        made.name = entityName .. "Action"
        made.target = points.entity
        made.localSpace = true
        made.loop = true
        return made
    end)
    if not okParticles or not particles then return nil end
    layer.particles = particles

    if texturePath and texturePath ~= "" then
        pcall(function()
            points:setTexture(texturePath)
            points:addSpriteFrame(key, 0, 0, 1, 1)
            particles:setSpriteIntializer(0, 0)
        end)
    end
    return layer
end

function Fire:build()
    if self.built then return true end
    local base = self:ownerName()
    local height = math.max(number(self.flameHeight, 1.5), 0.1)

    self.flame = self:makeLayer("flame", base .. "Flame", self.flameTexture)
    if not self.flame then
        self:warn("needs a Points entity and a Particles entity")
        return false
    end
    if self.smoke ~= false then
        self.smokeLayer = self:makeLayer("smoke", base .. "Smoke", self.smokeTexture)
    end
    if self.embers ~= false then
        self.emberLayer = self:makeLayer("ember", base .. "Ember", self.emberTexture)
    end

    if self.light ~= false then
        local ok, lamp = pcall(function()
            local made = Light(self.scene)
            made.name = base .. "Light"
            made.position = self:position() + Vector3(0, height * 0.45, 0)
            made:setType(LightType.POINT)
            made:setShadows(false)
            made:setRange(number(self.lightRange, 14.0))
            made:setIntensity(0.0)
            return made
        end)
        if ok then self.lamp = lamp end
    end

    if self.soundPath and self.soundPath ~= "" then
        local ok, sound = pcall(function()
            local made = Sound(self.scene)
            made.name = base .. "Sound"
            made:loadSound(self.soundPath)
            made.looping = true
            made:setMinMaxDistance(1.0, number(self.soundRadius, 18.0))
            made.volume = 0.0
            return made
        end)
        if ok then self.sound = sound end
    end

    self.built = true
    return true
end

-- The whole look is rebuilt from the properties: the particle system has no per-field setters
-- worth chasing, and re-running the initialisers is cheaper than tracking which one changed.
-- Each attribute has exactly one modifier and it writes an absolute value, so the ramps below
-- carry the real sizes instead of the multipliers a two-phase curve would want.
function Fire:apply()
    if not self:build() then return end
    local size = math.max(number(self.flameSize, 0.9), 0.05)
    local height = math.max(number(self.flameHeight, 1.5), 0.1)
    local core = rgb(self.colorCore, { 1.0, 0.94, 0.72 })
    local tip = rgb(self.colorTip, { 0.72, 0.16, 0.03 })
    local radians = math.rad(number(self.windDirection, 35.0))
    local wind = number(self.windSpeed, 0.6)
    local wx, wz = math.cos(radians) * wind, math.sin(radians) * wind

    pcall(function()
        local particles = self.flame.particles
        -- The action owns the pool: starting it sizes the sprite array from maxParticles.
        particles.maxParticles = math.floor(clamp(number(self.flameRate, 130.0) * 1.6, 64, 4096))
        particles:setLifeInitializer(0.26, 0.46)
        particles:setCirclePositionInitializer(size * 0.30, size * 0.14)
        particles:setPositionInitializer(Vector3(0, -height * 0.10, 0), Vector3(0, 0, 0))
        particles:setVelocityInitializer(
            Vector3(-0.16, height * 1.9, -0.16), Vector3(0.16, height * 2.7, 0.16))
        particles:setVelocityModifier(0.0, 1.0, Vector3(0, 0, 0), Vector3(wx, height * 0.5, wz))
        particles:setAccelerationInitializer(Vector3(0, height * 1.4, 0))
        particles:setSizeInitializer(size * 0.62, size * 0.80)
        particles:setSizeModifier(0.0, 1.0, size * 0.72, size * 0.16)
        particles:setRotationInitializer(0.0, 360.0)
        particles:setRotationModifier(0.0, 1.0, 0.0, 120.0)
        particles:clearColorGradient()
        particles:addColorGradientStop(0.0, core)
        particles:addColorGradientStop(0.34, Vector3(1.0, (core.y + tip.y) * 0.55, 0.20))
        particles:addColorGradientStop(0.72, tip)
        particles:addColorGradientStop(1.0, Vector3(tip.x * 0.28, tip.y * 0.18, tip.z * 0.16))
        particles:setColorGradientUseSRGB(false)
        particles:setAlphaInitializer(0.86, 1.0)
        particles:setAlphaModifier(0.0, 1.0, 1.0, 0.0)
    end)

    if self.smokeLayer then
        local smoke = self.smokeLayer
        pcall(function()
            local particles = smoke.particles
            local rise = math.max(number(self.smokeHeight, 9.0), height)
            particles.maxParticles = math.floor(clamp(number(self.smokeRate, 15.0) * 5.0, 64, 2048))
            smoke.points.position = self:position() + Vector3(0, height * 0.82, 0)
            particles:setLifeInitializer(rise * 0.24, rise * 0.36)
            particles:setCirclePositionInitializer(size * 0.20, size * 0.30)
            particles:setVelocityInitializer(Vector3(wx * 0.3, 0.8, wz * 0.3),
                Vector3(wx * 0.6 + 0.25, 1.6, wz * 0.6 + 0.25))
            particles:setVelocityModifier(0.0, 1.0, Vector3(0, 0, 0), Vector3(wx * 1.6, 0.4, wz * 1.6))
            particles:setSizeInitializer(size * 0.6, size * 0.9)
            particles:setSizeModifier(0.0, 1.0, size * 0.7, size * 2.6)
            particles:setRotationInitializer(0.0, 360.0)
            particles:setRotationModifier(0.0, 1.0, -36.0, 36.0)
            local tint = rgb(self.smokeColor, { 0.30, 0.29, 0.28 })
            particles:clearColorGradient()
            particles:addColorGradientStop(0.0, Vector3(tint.x * 1.8, tint.y * 1.6, tint.z * 1.5))
            particles:addColorGradientStop(1.0, tint)
            particles:setColorGradientUseSRGB(false)
            particles:setAlphaInitializer(0.24, 0.34)
            particles:setAlphaModifier(0.0, 1.0, 0.30, 0.0)
        end)
    end

    if self.emberLayer then
        local ember = self.emberLayer
        pcall(function()
            local particles = ember.particles
            particles.maxParticles = math.floor(clamp(number(self.emberRate, 12.0) * 3.0, 64, 2048))
            ember.points.position = self:position() + Vector3(0, height * 0.35, 0)
            particles:setLifeInitializer(0.8, 2.1)
            particles:setCirclePositionInitializer(size * 0.16, size * 0.26)
            particles:setVelocityInitializer(Vector3(wx - 0.7, height * 2.6, wz - 0.7),
                Vector3(wx + 0.7, height * 4.4, wz + 0.7))
            particles:setAccelerationInitializer(Vector3(0, -1.6, 0))
            particles:setSizeInitializer(0.035, 0.075)
            particles:setSizeModifier(0.0, 1.0, 0.06, 0.018)
            particles:clearColorGradient()
            particles:addColorGradientStop(0.0, Vector3(1.0, 0.86, 0.45))
            particles:addColorGradientStop(0.55, Vector3(1.0, 0.42, 0.08))
            particles:addColorGradientStop(1.0, Vector3(0.35, 0.06, 0.02))
            particles:setColorGradientUseSRGB(false)
            particles:setAlphaInitializer(0.8, 1.0)
            particles:setAlphaModifier(0.0, 1.0, 1.0, 0.0)
        end)
    end

    self:restartLayer(self.flame)
    self:restartLayer(self.smokeLayer)
    self:restartLayer(self.emberLayer)
end

-- Without restart() the emitter has nowhere to put a particle: the pool is sized when the action
-- starts, not when the component is configured.
function Fire:restartLayer(layer)
    if not layer then return end
    pcall(function() layer.particles:restart() end)
end

-- Rates are the only thing intensity changes, so a fire re-emits when the level moved enough to
-- see rather than every frame.
function Fire:emit()
    local level = clamp(self.effect, 0.0, 1.0)
    self.emitted = level
    for _, entry in ipairs({ { self.flame, self.flameRate }, { self.smokeLayer, self.smokeRate },
                             { self.emberLayer, self.emberRate } }) do
        local layer, rate = entry[1], entry[2]
        if layer then
            pcall(function()
                local perSecond = math.floor(math.max(number(rate, 0.0), 0.0) * level)
                layer.particles.rate = perSecond
                layer.particles.emitter = perSecond > 0
                -- The engine stops a finished action on its own, so a rate that climbs back over
                -- zero has to restart it; an emitter flag on a stopped action does nothing.
                if perSecond > 0 and not layer.particles:isRunning() then
                    layer.particles:restart()
                end
            end)
        end
    end
    if self.sound then
        pcall(function()
            if level > 0.02 and self.lit then self.sound:play() end
            if not self.lit then self.sound:stop() end
        end)
    end
end

function Fire:init()
    self.effect = clamp(number(self.intensity, 1.0), 0.0, 1.0)
    self.emitted = -1.0
    self.dry = 1.0
    self.dying = 1.0
    self.fuelLeft = number(self.fuel, 0.0)
    self.fuelFull = self.fuelLeft
    self.nextSpread = 0.0
    self.lit = false
    self.lightLevel = 0.0
    RegisterEngineEvent(self, "onUpdate")
    if self.startOnLoad ~= false then
        if self.burning ~= false then
            self:ignite()
        else
            self:apply()
        end
    end
end

function Fire:ignite()
    -- A fire that has eaten its fuel needs new fuel, not a match.
    if self.fuelFull > 0.0 and self.fuelLeft <= 0.0 then
        self:warn("out of fuel, addFuel() before it can burn again")
        return false
    end
    self.lit = true
    self.dying = 1.0
    self:apply()
    self:emit()
    return true
end

function Fire:extinguish()
    self.lit = false
    self.effect = 0.0
    self:emit()
    for _, layer in ipairs({ self.flame, self.smokeLayer, self.emberLayer }) do
        if layer then pcall(function() layer.particles:stop() end) end
    end
end

function Fire:setIntensity(value)
    self.intensity = clamp(number(value, self.intensity), 0.0, 1.0)
end

function Fire:addFuel(seconds)
    local add = number(seconds, 0.0)
    if add <= 0.0 then return self.fuelLeft end
    self.fuelLeft = self.fuelLeft + add
    self.fuelFull = math.max(self.fuelFull, self.fuelLeft)
    self.dying = 1.0
    return self.fuelLeft
end

-- Water poured by hand goes through the same wetness clock as a storm, so a bucket and the weather
-- cannot disagree about how soaked this fire is.
function Fire:soak(amount)
    self.dry = clamp(number(self.dry, 1.0) - number(amount, 0.5), 0.0, 1.0)
    if self.dry <= 0.0 then self:extinguish() end
    return self.dry
end

function Fire:isBurning() return self.lit end
function Fire:getIntensity() return self.effect end
function Fire:getFuel() return self.fuelLeft end
function Fire:getWet() return 1.0 - clamp(number(self.dry, 1.0), 0.0, 1.0) end
function Fire:getParticleCount() return self:layerCount(self.flame) + self:layerCount(self.smokeLayer)
    + self:layerCount(self.emberLayer) end

function Fire:layerCount(layer)
    local out = 0
    if layer then pcall(function() out = layer.points:getNumPoints() end) end
    return number(out, 0)
end

-- Three incommensurate sines read as an irregular flicker without a random walk that could drift
-- the light to zero and leave the fire unlit for the rest of the run.
function Fire:flickerLevel(now)
    local speed = math.max(number(self.flickerSpeed, 9.0), 0.1)
    local t = now * speed
    local wave = math.sin(t * 2.0) * 0.5 + math.sin(t * 5.7 + 1.3) * 0.32
        + math.sin(t * 13.1 + 2.9) * 0.18
    return 1.0 - clamp(number(self.flicker, 0.35), 0.0, 1.0) * 0.5 * (1.0 + wave)
end

function Fire:rainLevel()
    if self.rainExtinguishes == false then return 0.0 end
    local weather = self.weather
    if not weather or type(weather.getIntensity) ~= "function" then return 0.0 end
    if tonumber(weather.type) ~= 0 then return 0.0 end
    return clamp(number(weather:getIntensity(), 0.0), 0.0, 1.0)
end

function Fire:spreadTick(now)
    local radius = number(self.spreadRadius, 0.0)
    if radius <= 0.0 or not self.lit then return end
    if now < self.nextSpread then return end
    self.nextSpread = now + math.max(number(self.spreadInterval, 0.8), 0.05)

    local here = self:position()
    local list = burnables()
    local keep = {}
    for _, entry in ipairs(list) do
        local alive = false
        pcall(function() alive = self.scene:isEntityCreated(entry.id) end)
        if not alive then
            self.destroyed = true
        elseif entry.burnable.fire ~= self and entry.burnable.spent ~= true then
            keep[#keep + 1] = entry
            if not entry.burnable.lit and entry.burnable:distanceTo(here) <= radius then
                entry.burnable:igniteFrom(self)
            end
        else
            keep[#keep + 1] = entry
        end
    end
    if self.destroyed then
        self.destroyed = nil
        for i = #list, 1, -1 do table.remove(list, i) end
        for _, entry in ipairs(keep) do list[#list + 1] = entry end
    end
end

function Fire:onUpdate()
    local dt = math.min(Engine.deltatime, 0.05)
    local now = Engine.systemTime

    -- Rain is a multiplier, not a subtraction: the fuel branch below recomputes effect every frame
    -- and a decay written straight into it would be overwritten on the same tick. This runs for an
    -- unlit fire too, so the ground dries after the shower and a match works again.
    local rain = self:rainLevel()
    local dry = number(self.dry, 1.0)
    if rain > 0.0 then
        dry = clamp(dry - dt * rain * 0.55, 0.0, 1.0)
    elseif dry < 1.0 then
        -- A doused fire dries out and can be lit again; the rate is slow enough that one shower
        -- costs more than one lighting.
        dry = clamp(dry + dt * 0.15, 0.0, 1.0)
    end
    self.dry = dry
    if not self.lit then return end

    local burnable = 1.0
    if self.fuelFull > 0.0 then
        local strength = math.max(number(self.intensity, 1.0) * dry, 0.05)
        self.fuelLeft = self.fuelLeft - dt * number(self.burnRate, 1.0) * strength
        if self.fuelLeft <= 0.0 then
            self.fuelLeft = 0.0
            -- Out of fuel is a fade, not a cut: the flames have to go down over a second for the
            -- fire to read as having burned out rather than as having been switched off.
            self.dying = math.max(number(self.dying, 1.0) - dt * 0.8, 0.0)
            burnable = self.dying
        else
            burnable = clamp(self.fuelLeft / self.fuelFull * 3.0, 0.0, 1.0)
        end
    end
    self.effect = number(self.intensity, 1.0) * burnable * dry

    if self.effect <= 0.0 then self:extinguish() return end

    if math.abs(self.emitted - self.effect) > 0.05 then self:emit() end

    self.lightLevel = number(self.lightIntensity, 6.0) * self.effect * self:flickerLevel(now)
    if self.lamp then
        pcall(function() self.lamp:setIntensity(self.lightLevel) end)
    end
    if self.sound then
        pcall(function()
            self.sound.volume = clamp(number(self.volume, 0.55) * self.effect
                * (0.8 + 0.2 * self:flickerLevel(now)), 0.0, 1.0)
        end)
    end

    self:heatTick(dt)
    self:spreadTick(now)
end

-- No overlap queries from Lua, so the subject is tested by hand the same way DamageArea does it.
function Fire:heatTick(dt)
    local damage = number(self.heatDamage, 0.0)
    if damage <= 0.0 then return end
    local health = self.health
    if not health or type(health.takeDamage) ~= "function" then return end
    local id = entityId(self.subject)
    if not id and self.fallbackSubjectName and self.fallbackSubjectName ~= "" then
        id = entityId(self.scene:findEntity(self.fallbackSubjectName))
    end
    if not id then return end
    local ok, position = pcall(function() return Object(self.scene, id).position end)
    if not ok or not position then return end
    local here = self:position()
    local radius = math.max(number(self.damageRadius, 1.4), 0.1)
    local dx, dy, dz = here.x - position.x, here.y - position.y, here.z - position.z
    if dx * dx + dy * dy + dz * dz > radius * radius then return end

    -- Health refuses a second hit inside its invulnerability window, so a per-second heat is
    -- accumulated and paid in one chunk every quarter second; a refused chunk stays owed.
    self.heatOwed = number(self.heatOwed, 0.0) + damage * dt
    self.heatClock = number(self.heatClock, 0.0) + dt
    if self.heatClock < 0.25 then return end
    self.heatClock = 0.0
    local applied = health:takeDamage(self.heatOwed, self.entity)
    if applied ~= false or (type(health.isAlive) == "function" and health:isAlive() == false) then
        self.heatOwed = 0.0
    end
end

return Fire
