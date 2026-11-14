-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Living sea/river surface: attach to the entity holding the Water component (or point the Water
-- category: Water & Shore
-- slot at it); the wind drifts, gusts breathe and steepness couples to height. Drives setIntensity()/gust().
local WaterWaves = {
    properties = {
        { name = "water", displayName = "Water", type = "entity" },
        { name = "fallbackName", displayName = "Water name if slot is empty", type = "string", default = "Water" },
        { name = "waveHeight", displayName = "Wave height (metres)", type = "float", default = 0.25 },
        { name = "waveLength", displayName = "Wave length (metres)", type = "float", default = 12.0 },
        { name = "waveSpeed", displayName = "Wave speed (m/s)", type = "float", default = 1.0 },
        { name = "waveDirection", displayName = "Wave direction (degrees)", type = "float", default = 35.0 },
        { name = "waveSteepness", displayName = "Wave steepness (0..1)", type = "float", default = 0.6 },
        { name = "windDrift", displayName = "Direction drift (degrees/s)", type = "float", default = 0.6 },
        { name = "gustInterval", displayName = "Gust interval (seconds)", type = "float", default = 12.0 },
        { name = "gustStrength", displayName = "Gust strength (multiplier)", type = "float", default = 1.8 },
        { name = "intensity", displayName = "Intensity (0..1)", type = "float", default = 1.0 },
        { name = "crestFoam", displayName = "Crest foam (0..1)", type = "float", default = 0.18 },
        { name = "ripples", displayName = "Enable sim ripples", type = "bool", default = true }
    }
}

local function clamp(v, lo, hi)
    if v < lo then return lo end
    if v > hi then return hi end
    return v
end

local function entityId(ref)
    if type(ref) == "number" then
        return ref ~= 0 and ref or nil
    end
    if ref and type(ref.entity) == "number" then
        return ref.entity ~= 0 and ref.entity or nil
    end
    return nil
end

local function angle(deg)
    return Engine.useDegrees and deg or math.rad(deg)
end

local function warn(self, message)
    if Engine.systemTime - self.lastWarn > 1.0 then
        self.lastWarn = Engine.systemTime
        print("[WaterWaves] " .. message)
    end
end

-- Smooth pseudo-noise from sines: the engine has no Perlin/simplex, and three incommensurate
-- partials feel weather-like instead of periodic.
local function noise(t)
    return clamp((math.sin(t * 0.13) * 0.55 + math.sin(t * 0.371 + 1.7) * 0.25
        + math.sin(t * 0.0417 + 4.2) * 0.45) / 1.25, -1.0, 1.0)
end
function WaterWaves:init()
    RegisterEngineEvent(self, "onUpdate")
    self.waveHeight, self.waveLength = self.waveHeight or 0.25, self.waveLength or 12.0
    self.waveSpeed, self.waveDirection = self.waveSpeed or 1.0, self.waveDirection or 35.0
    self.waveSteepness, self.crestFoam = self.waveSteepness or 0.6, self.crestFoam or 0.18
    self.gustInterval = math.max(self.gustInterval or 12.0, 0.5)
    self.gustStrength = math.max(self.gustStrength or 1.8, 1.0)
    self.windDir = self.waveDirection
    self.gustBoost = 0.0
    self.currentHeight = self.waveHeight
    self.lastWarn = 0.0
    self.waterHandle = self:resolveWater()
    self:loadOnce()
end

function WaterWaves:resolveWater()
    local id = entityId(self.water)
    if not id and self.fallbackName and self.fallbackName ~= "" then
        id = entityId(self.scene:findEntity(self.fallbackName))
    end
    if not id then return nil end
    local ok, water = pcall(function()
        return Water(self.scene, id)
    end)
    return (ok and water) or nil
end

function WaterWaves:loadOnce()
    if self.loaded or not self.waterHandle then return end
    self.loaded = pcall(function()
        self.waterHandle:load()
    end)
end

function WaterWaves:setWindDirection(degrees)
    if type(degrees) == "number" then
        self.waveDirection, self.windDir = degrees, degrees
    end
end

function WaterWaves:setIntensity(zeroToOne)
    if type(zeroToOne) == "number" then
        self.intensity = clamp(zeroToOne, 0.0, 1.0)
    end
end

-- The boost decays over the following seconds, so hits from swimmers read as splashes.
function WaterWaves:gust(strength)
    self.gustBoost = clamp(self.gustBoost + math.max(type(strength) == "number" and strength or 1.0, 0.0), 0.0, 4.0)
end

function WaterWaves:getWaveHeight()
    return self.currentHeight or 0.0
end
function WaterWaves:getWindDirection()
    return self.windDir or 0.0
end

function WaterWaves:onUpdate()
    if not self.waterHandle then
        self.waterHandle = self:resolveWater()
        self:loadOnce()
    end
    if not self.waterHandle then
        warn(self, "No water: add a Water component or drag it into the Water slot "
            .. "(fallback name: " .. tostring(self.fallbackName) .. ")")
        return
    end

    local dt = math.min(Engine.deltatime, 0.05)
    local t = Engine.systemTime
    local intensity = clamp(self.intensity or 1.0, 0.0, 1.0)
    self.windDir = self.windDir + noise(t) * (self.windDrift or 0.6) * dt
    self.gustBoost = math.max(0.0, self.gustBoost - dt * 0.6)

    -- Squaring the swell keeps most of the cycle calm and gives short sharp peaks; gust()'s
    -- boost rides on top and decays.
    local swell = math.max(0.0, math.sin(t * math.pi * 2.0 / self.gustInterval))
    local envelope = 1.0 + (self.gustStrength - 1.0) * swell * swell * swell + self.gustBoost
    local height = self.waveHeight * intensity * envelope
    self.currentHeight = height
    local ratio = height / math.max(self.waveHeight, 0.001)
    local steepness = clamp(self.waveSteepness * (0.6 + 0.4 * ratio), 0.05, 0.95)

    local ok, err = pcall(function()
        local water = self.waterHandle
        water.waveHeight = height
        water.waveLength = self.waveLength * (0.9 + 0.15 * envelope)
        water.waveSpeed = self.waveSpeed * (0.8 + 0.3 * intensity * envelope)
        water.waveDirection = angle(((self.windDir % 360.0) + 360.0) % 360.0)
        water.waveSteepness = steepness
        water.crestFoam = clamp(self.crestFoam * (0.4 + 0.6 * ratio), 0.0, 1.0)
        if self.ripples then
            water.simRipples = true
        end
    end)
    if not ok then
        warn(self, "Could not write the wave state to the Water component. " .. tostring(err))
    end
end

return WaterWaves
