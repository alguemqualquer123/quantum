-- (c) SR VINIX
-- SPDX-License-Identifier: MIT
-- Fuel for a Fire: drop it on any entity and a nearby flame can light it. It builds its own Fire
-- (flames, smoke, embers, light, crackle) on this entity, eats its fuel, darkens its mesh as it
-- chars, damages the Health it is given, can hurt whoever stands in it, spreads to the Burnables
-- next to it, and either hides when the fuel is gone or stays as charcoal. Rain puts it out.
-- category: Environment & Motion
local Burnable = {
    properties = {
        { name = "ignited", displayName = "Already burning when the scene loads", type = "bool", default = false },
        { name = "intensity", displayName = "Intensity (0..1)", type = "float", default = 1.0 },
        { name = "fuel", displayName = "Fuel seconds", type = "float", default = 10.0 },
        { name = "burnRate", displayName = "Fuel used per second at full intensity", type = "float", default = 1.0 },
        { name = "flammability", displayName = "Flammable (0 = refuses any fire)", type = "float", default = 1.0 },
        { name = "igniteChance", displayName = "Chance to catch from a nearby fire (0..1)", type = "float", default = 1.0 },
        { name = "burnSize", displayName = "Flame width (m, 0 = auto)", type = "float", default = 0.0 },
        { name = "burnHeight", displayName = "Flame height (m, 0 = auto)", type = "float", default = 0.0 },
        { name = "autoSize", displayName = "Size the flames from the Mesh bounds", type = "bool", default = false },
        { name = "showFlames", displayName = "Draw flames", type = "bool", default = true },
        { name = "showSmoke", displayName = "Draw smoke", type = "bool", default = true },
        { name = "showEmbers", displayName = "Throw embers", type = "bool", default = true },
        { name = "castLight", displayName = "Cast light", type = "bool", default = true },
        { name = "crackle", displayName = "Play the crackle loop", type = "bool", default = true },
        { name = "tint", displayName = "Char the Mesh while burning", type = "bool", default = false },
        { name = "charAmount", displayName = "How dark the char gets (0..1)", type = "float", default = 0.85 },
        { name = "burnAway", displayName = "Disappear when the fuel is gone", type = "bool", default = false },
        { name = "spreadRadius", displayName = "Light Burnables within (m, 0 = off)", type = "float", default = 0.0 },
        { name = "health", displayName = "Own Health component to drain", type = "Health" },
        { name = "selfDamage", displayName = "Own health lost per second", type = "float", default = 0.0 },
        { name = "heatDamage", displayName = "Damage per second to the subject", type = "float", default = 0.0 },
        { name = "damageRadius", displayName = "Heat radius (m)", type = "float", default = 1.4 },
        { name = "subject", displayName = "Entity the heat hurts", type = "entity" },
        { name = "fallbackSubjectName", displayName = "Subject name if slot is empty", type = "string", default = "" },
        { name = "weather", displayName = "WeatherSystem component", type = "WeatherSystem" },
        { name = "rainExtinguishes", displayName = "Rain puts it out", type = "bool", default = true },
        { name = "flameColor", displayName = "Hot core (r, g, b)", type = "color3", default = { 1.0, 0.94, 0.72 } },
        { name = "tipColor", displayName = "Cool tip (r, g, b)", type = "color3", default = { 0.72, 0.16, 0.03 } }
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

-- Fire and Burnable ship together, but a project may only carry one of them and the install
-- directory is the author's choice, so every layout the library is placed in is tried.
local function library(name)
    for _, path in ipairs({ "Components." .. name, "Scripts." .. name, "components." .. name, name }) do
        local ok, module = pcall(require, path)
        if ok and type(module) == "table" and module.properties then return module end
    end
    return nil
end

-- The same setup the engine does when it attaches a script: property defaults, then the overrides,
-- then init. The instance is kept in self.fire, which is what keeps its particle layers reachable.
local function attach(module, scene, entity, overrides)
    local instance = setmetatable({}, { __index = module })
    instance.scene = scene
    instance.entity = entity
    for _, property in ipairs(rawget(module, "properties") or {}) do
        if property.default ~= nil then instance[property.name] = property.default end
    end
    for key, value in pairs(overrides or {}) do
        instance[key] = value
    end
    if rawget(module, "init") then instance:init() end
    return instance
end

-- The same table Fire.lua reads: a Burnable publishes itself so a flame can find what to light
-- without a slot per neighbour. Entries are checked against the scene before they are used.
local function burnables()
    if not _G.__quantumFire then _G.__quantumFire = { burnables = {} } end
    return _G.__quantumFire.burnables
end

function Burnable:warn(message)
    if not self.lastWarn or Engine.systemTime - self.lastWarn > 1.0 then
        self.lastWarn = Engine.systemTime; print("[Burnable] " .. message)
    end
end

function Burnable:ownerName()
    if self.cachedName then return self.cachedName end
    local ok, name = pcall(function() return self.scene:getEntityName(self.entity) end)
    self.cachedName = (ok and type(name) == "string" and name ~= "") and name or "Burnable"
    return self.cachedName
end

function Burnable:position()
    local ok, value = pcall(function() return Object(self.scene, self.entity).position end)
    if ok and value then return value end
    return Vector3(0, 0, 0)
end

function Burnable:distanceTo(point)
    if not point then return math.huge end
    local here = self:position()
    local ok, x, y, z = pcall(function()
        return math.abs(here.x - point.x), math.abs(here.y - point.y), math.abs(here.z - point.z)
    end)
    if not ok then return math.huge end
    return math.sqrt(x * x + y * y + z * z)
end

-- Reading bounds off an entity with no Mesh and no Model logs a missing-component error, so the
-- probe is opt-in and runs once. Without it the flame size is only the two properties.
function Burnable:extents()
    if self.sizeProbed then return self.sizeBox end
    self.sizeProbed = true
    if self.autoSize == false then return nil end
    local ok, box = pcall(function() return Mesh(self.scene, self.entity):getWorldAABB() end)
    if not ok or not box then return nil end
    local empty = false
    pcall(function() empty = box:isNull() end)
    if empty or not (box.maximum.y >= box.minimum.y) then return nil end
    self.sizeBox = box
    return self.sizeBox
end

function Burnable:flameSize()
    local explicit = number(self.burnSize, 0.0)
    if explicit > 0.0 then return explicit end
    local box = self:extents()
    if not box then return 1.0 end
    return clamp(math.max((box.maximum.x - box.minimum.x) * 0.55,
        (box.maximum.z - box.minimum.z) * 0.55), 0.2, 8.0)
end

function Burnable:flameHeight()
    local explicit = number(self.burnHeight, 0.0)
    if explicit > 0.0 then return explicit end
    local box = self:extents()
    if not box then return 1.8 end
    local tall = (box.maximum.y - box.minimum.y) * 0.9
    -- A thin sheet of wood should not produce a 20 m column, so the probe is capped, not trusted.
    return clamp(math.max(tall, 0.6), 0.4, 6.0)
end

-- The Fire module is what draws and lights the burn; without it the fuel clock still runs, so a
-- project that installs only Burnable gets the gameplay (charring, damage, spread) with no visuals.
function Burnable:ensureFire()
    if self.fire or self.noFire then return self.fire end
    local module = library("Fire")
    if not module then
        self.noFire = true
        self:warn("Fire.lua is not in the project, burning without visuals")
        return nil
    end
    local fire = attach(module, self.scene, self.entity, {
        startOnLoad = false,
        burning = false,
        intensity = clamp(number(self.intensity, 1.0), 0.0, 1.0),
        -- The fuel clock lives here: Fire would empty it on its own and the char ramp and the
        -- burn-out state would stop agreeing with it.
        fuel = 0.0,
        flameSize = self:flameSize(),
        flameHeight = self:flameHeight(),
        smoke = self.showSmoke ~= false,
        embers = self.showEmbers ~= false,
        light = self.castLight ~= false,
        colorCore = self.flameColor,
        colorTip = self.tipColor,
        spreadRadius = number(self.spreadRadius, 0.0),
        weather = self.weather,
        -- The Burnable owns the rain: it is the one with the wet state that decides whether the
        -- fuel catches at all, and two systems damping the same flame would fight over the level.
        rainExtinguishes = false,
        heatDamage = number(self.heatDamage, 0.0),
        damageRadius = number(self.damageRadius, 1.4),
        subject = self.subject,
        fallbackSubjectName = self.fallbackSubjectName,
        soundPath = self.crackle == false and "" or nil
    })
    fire.owner = self
    if self.showFlames == false then
        fire.flameRate = 0.0
    end
    self.fire = fire
    return fire
end

function Burnable:init()
    self.lit = false
    self.spent = false
    self.char = 0.0
    self.wet = 0.0
    self.effect = 0.0
    self.fuelFull = math.max(number(self.fuel, 10.0), 0.0)
    self.fuelLeft = self.fuelFull
    self.baseColor = nil
    self.registered = false
    RegisterEngineEvent(self, "onUpdate")
    burnables()[#burnables() + 1] = { id = entityId(self.entity) or self.entity, burnable = self }
    self.registered = true
    if self.ignited ~= false then self:ignite() end
end

function Burnable:ignite()
    if self.spent then return false end
    if number(self.flammability, 1.0) <= 0.0 then return false end
    -- Soaked fuel takes a match and gives back steam; the wet level is what rain drives up.
    if number(self.wet, 0.0) > 0.6 then return false end
    self.lit = true
    local fire = self:ensureFire()
    if fire then fire:ignite() end
    return true
end

function Burnable:igniteFrom(source)
    if self.lit or self.spent then return false end
    if number(self.flammability, 1.0) <= 0.0 then return false end
    local chance = clamp(number(self.igniteChance, 1.0), 0.0, 1.0)
    if chance < 1.0 and math.random() > chance then return false end
    self.source = source
    return self:ignite()
end

function Burnable:extinguish()
    self.lit = false
    self.effect = 0.0
    if self.fire then self.fire:extinguish() end
end

-- Kept separate from extinguish(): rain douses and leaves the fuel, a burnt-out object has no fuel
-- left and must never light again from a passing flame.
function Burnable:burnOut()
    self:extinguish()
    if self.fuelLeft <= 0.0 then self.spent = true end
    self.char = 1.0
    if self.burnAway ~= false then
        self:hideAway()
    end
end

function Burnable:hideAway()
    local id = entityId(self.entity) or self.entity
    pcall(function()
        local object = Object(self.scene, id)
        object.visible = false
        object:removeBody3D()
    end)
end

-- Water by hand goes through the same wetness clock rain drives, so a bucket and a storm cannot
-- disagree about how soaked this fuel is - and fully soaked fuel refuses a match until it dries.
function Burnable:soak(amount)
    self.wet = clamp(number(self.wet, 0.0) + number(amount, 0.5), 0.0, 1.0)
    if self.wet >= 1.0 then self:extinguish() end
    return self.wet
end

function Burnable:getWet() return number(self.wet, 0.0) end
function Burnable:douse() return self:soak(1.0) end

function Burnable:addFuel(seconds)
    local add = number(seconds, 0.0)
    if add <= 0.0 then return self.fuelLeft end
    if self.fuelFull <= 0.0 then self.fuelFull = add end
    self.fuelLeft = self.fuelLeft + add
    self.fuelFull = math.max(self.fuelFull, self.fuelLeft)
    self.spent = false
    return self.fuelLeft
end

function Burnable:setIntensity(value)
    self.intensity = clamp(number(value, self.intensity), 0.0, 1.0)
    if self.fire then self.fire:setIntensity(self.effect) end
end

function Burnable:isBurning() return self.lit end
function Burnable:getFuel() return self.fuelLeft end
function Burnable:getChar() return self.char end
function Burnable:getIntensity() return self.effect end
function Burnable:getFire() return self.fire end

function Burnable:rainLevel()
    if self.rainExtinguishes == false then return 0.0 end
    local weather = self.weather
    if not weather or type(weather.getIntensity) ~= "function" then return 0.0 end
    if tonumber(weather.type) ~= 0 then return 0.0 end
    return clamp(number(weather:getIntensity(), 0.0), 0.0, 1.0)
end

-- Mesh::setColor only walks the submeshes that exist, so this is safe on a Model whose geometry is
-- on child nodes; it is the reason tint is off by default (a meshless entity has nothing to char).
-- The read is a probe, so it runs once: a component a handle does not have logs an engine error and
-- then throws, and throwing inside the pcall below is how "this entity has no Mesh" is detected.
function Burnable:charTick()
    if self.tint == false then return end
    local level = clamp(number(self.charAmount, 0.85), 0.0, 1.0) * self.char
    if level <= 0.0 then return end
    if math.abs(number(self.tinted, -1.0) - level) < 0.02 then return end
    self.tinted = level
    local ok = pcall(function()
        local mesh = Mesh(self.scene, self.entity)
        if not self.baseColor then self.baseColor = mesh.color end
        local base = self.baseColor
        local mixed = Vector4(base.x + (0.035 - base.x) * level,
            base.y + (0.030 - base.y) * level,
            base.z + (0.028 - base.z) * level,
            base.w)
        mesh.color = mixed
    end)
    if not ok then
        self.tint = false
        self:warn("no Mesh on " .. self:ownerName() .. ", char tint is off")
    end
end

function Burnable:selfDamageTick(dt)
    local damage = number(self.selfDamage, 0.0)
    local health = self.health
    if damage <= 0.0 or not health or type(health.takeDamage) ~= "function" then return end
    -- Same quarter-second chunk as Fire's heat: Health drops a second hit inside its invulnerability
    -- window, so a per-second drain has to be paid in one go or it never lands.
    self.drainOwed = number(self.drainOwed, 0.0) + damage * dt
    self.drainClock = number(self.drainClock, 0.0) + dt
    if self.drainClock < 0.25 then return end
    self.drainClock = 0.0
    local applied = health:takeDamage(self.drainOwed, self.entity)
    if applied ~= false or (type(health.isAlive) == "function" and health:isAlive() == false) then
        self.drainOwed = 0.0
    end
    if type(health.isAlive) == "function" and health:isAlive() == false and self.burnAway ~= false then
        self:hideAway()
    end
end

function Burnable:onUpdate()
    local dt = math.min(Engine.deltatime, 0.05)

    -- Wet is what rain drives and the sun takes back, and it runs on unlit fuel too: a crate out in
    -- a shower has to get soaked before it can refuse a match. Multiplying the flame by how dry it
    -- is, instead of subtracting, is what keeps the fuel clock and the flame level agreeing.
    local rain = self:rainLevel()
    if rain > 0.0 then
        self.wet = clamp(number(self.wet, 0.0) + dt * rain * 0.55, 0.0, 1.0)
    elseif number(self.wet, 0.0) > 0.0 then
        self.wet = clamp(self.wet - dt * 0.15, 0.0, 1.0)
    end
    if self.wet >= 1.0 and self.lit then
        self:extinguish()
        return
    end

    if not self.lit then return end

    local dry = 1.0 - number(self.wet, 0.0)
    if self.fuelFull > 0.0 then
        self.fuelLeft = self.fuelLeft - dt * number(self.burnRate, 1.0) * dry
        if self.fuelLeft <= 0.0 then
            self.fuelLeft = 0.0
            self:burnOut()
            return
        end
        -- The last third of the fuel is where a fire visibly dies, so the ramp reaches full
        -- intensity early instead of sitting at 1 until the fuel is gone.
        self.effect = number(self.intensity, 1.0) * dry
            * clamp(self.fuelLeft / self.fuelFull * 3.0, 0.0, 1.0)
        self.char = clamp(1.0 - self.fuelLeft / self.fuelFull, 0.0, 1.0)
    else
        self.effect = number(self.intensity, 1.0) * dry
    end

    if self.fire then self.fire:setIntensity(self.effect) end
    self:charTick()
    self:selfDamageTick(dt)
end

return Burnable
