-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Cloud deck: drives the cloud members of the sky_daynight fork on a Sky entity.
-- category: Environment & Motion
-- Attach to any entity, or wire the Sky slot (fallback: an entity named "Sky").
--
-- Deliberately separate from DayNightCycle: the cycle owns sunDir/moonDir/atmosphere/stars and
-- this one owns cloudLayer/cloudShape/cloudWind/cloudTint, so a game can change the weather
-- without touching the clock and the two can sit on the same Sky entity without fighting over a
-- member. camPos is the one name both write, always with the same value, so either component
-- works on its own.
--
-- The clouds are one raymarched plane plus a thin cirrus plane above it, so altitude and parallax
-- are real: raise the camera and the deck flattens toward the horizon under you.
local Clouds = {
    properties = {
        { name = "preset", displayName = "Preset (clear|few|overcast|storm|custom)", type = "string", default = "few" },
        { name = "coverage", displayName = "Coverage (0..1)", type = "float", default = 0.35 },
        { name = "density", displayName = "Density (0..1)", type = "float", default = 0.85 },
        { name = "altitude", displayName = "Altitude (m)", type = "float", default = 1600.0 },
        { name = "cloudScale", displayName = "Horizontal scale (m)", type = "float", default = 2200.0 },
        { name = "thickness", displayName = "Layer thickness (m)", type = "float", default = 600.0 },
        { name = "detail", displayName = "Detail (0..1)", type = "float", default = 0.55 },
        { name = "softness", displayName = "Edge softness", type = "float", default = 0.22 },
        { name = "cirrus", displayName = "Cirrus layer (0..1)", type = "float", default = 0.25 },
        { name = "windSpeed", displayName = "Wind speed (m/s)", type = "float", default = 6.0 },
        { name = "windDirection", displayName = "Wind direction (degrees)", type = "float", default = 210.0 },
        { name = "cloudColor", displayName = "Cloud colour (r, g, b)", type = "color3", default = { 1.0, 0.99, 0.98 } },
        { name = "litLuminance", displayName = "Lit luminance", type = "float", default = 1.15 },
        { name = "shadowLuminance", displayName = "Shadowed luminance", type = "float", default = 0.34 },
        { name = "nightDarkness", displayName = "Night darkness (0..1)", type = "float", default = 0.16 },
        { name = "sky", displayName = "Sky entity", type = "entity" },
        { name = "fallbackSkyName", displayName = "Sky name if slot is empty", type = "string", default = "Sky" }
    }
}

local PRESETS = {
    clear = { coverage = 0.04, density = 0.40, cirrus = 0.06, detail = 0.40, altitude = 2200.0 },
    few = { coverage = 0.35, density = 0.85, cirrus = 0.25, detail = 0.55, altitude = 1600.0,
            thickness = 600.0, softness = 0.22 },
    overcast = { coverage = 0.92, density = 1.00, cirrus = 0.10, detail = 0.70, altitude = 950.0,
                 thickness = 420.0, softness = 0.30, shadowLuminance = 0.42, litLuminance = 0.95 },
    storm = { coverage = 0.99, density = 1.00, cirrus = 0.00, detail = 0.85, altitude = 1300.0,
              thickness = 1500.0, softness = 0.16, shadowLuminance = 0.11, litLuminance = 0.55,
              nightDarkness = 0.06 }
}

local function clamp(v, lo, hi) if type(v) ~= "number" or v ~= v then return lo end
    if v < lo then return lo end; if v > hi then return hi end; return v end
local function entityId(reference)
    if type(reference) == "number" then return reference ~= 0 and reference or nil end
    if reference and type(reference.entity) == "number" then
        return reference.entity ~= 0 and reference.entity or nil
    end
    return nil
end
local function asVec3(value, fallback)
    local ok, out = pcall(function()
        if type(value) ~= "table" and type(value) ~= "userdata" then return fallback end
        if type(value.x) == "number" then return Vector3(value.x, value.y or 0.0, value.z or 0.0) end
        if type(value[1]) == "number" then return Vector3(value[1], value[2] or 0.0, value[3] or 0.0) end
        return fallback
    end)
    return ok and out or fallback
end

function Clouds:setPreset(name)
    local preset = PRESETS[name]
    if not preset then return false end
    for field, value in pairs(preset) do self[field] = value end
    self.preset = name
    return true
end

function Clouds:init()
    RegisterEngineEvent(self, "onUpdate")
    if self.preset ~= "custom" then self:setPreset(self.preset) end
    self.lastWarn, self.warned = 0.0, false
end

function Clouds:onUpdate()
    local id = entityId(self.sky)
    if not id and self.fallbackSkyName and self.fallbackSkyName ~= "" then
        id = entityId(self.scene:findEntity(self.fallbackSkyName))
    end
    if not id then
        if Engine.systemTime - self.lastWarn > 2.0 then
            self.lastWarn = Engine.systemTime
            print("[Clouds] No sky to write: wire the Sky slot or name an entity "
                .. tostring(self.fallbackSkyName))
        end
        return
    end
    if not self.skyHandle then
        local ok, handle = pcall(function() return SkyBox(self.scene, id) end)
        if not ok or not handle then return end
        self.skyHandle = handle
    end

    local colour = asVec3(self.cloudColor, Vector3(1.0, 0.99, 0.98))
    local bearing = math.rad(clamp(self.windDirection, 0.0, 360.0))
    local speed = clamp(self.windSpeed, 0.0, 200.0)
    local values = {
        cloudLayer = Vector4(clamp(self.coverage, 0.0, 1.0), math.max(self.altitude, 60.0),
                             math.max(self.cloudScale, 20.0), clamp(self.detail, 0.0, 1.0)),
        cloudShape = Vector4(clamp(self.density, 0.0, 1.0), clamp(self.softness, 0.02, 1.0),
                             math.max(self.thickness, 20.0), clamp(self.cirrus, 0.0, 1.0)),
        cloudWind = Vector4(math.cos(bearing) * speed, math.sin(bearing) * speed,
                            clamp(self.litLuminance, 0.0, 4.0), clamp(self.shadowLuminance, 0.0, 2.0)),
        cloudTint = Vector4(colour.x, colour.y, colour.z, clamp(self.nightDarkness, 0.0, 1.0))
    }
    local cameraId = entityId(self.scene.camera)
    if cameraId then
        local eye = Object(self.scene, cameraId)
        local p = eye.position
        values.camPos = Vector4(p.x, p.y, p.z, 0.0)
    end

    local ok, err = pcall(function()
        for name, value in pairs(values) do self.skyHandle:setShaderUniform(name, value) end
    end)
    if not ok and not self.warned then
        self.warned = true
        print("[Clouds] The Sky rejected the uniform write: " .. tostring(err))
    end
end

return Clouds
