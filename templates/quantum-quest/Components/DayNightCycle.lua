-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Day/night cycle: one clock drives the sun, the moon, the sky shader, fog and ambient together.
-- category: Environment & Motion
-- Attach to the entity holding the directional Light, or wire the Light/Sky/Fog/Moon slots.
--
-- With the project's assets/shaders/sky_daynight fork assigned to the Sky entity (set the Sky's
-- Custom Shader row to it), this component also writes the fork's u_fs_customParams members, so
-- the dome, the sun and moon discs, the stars and the moon lighting all follow the same clock.
-- Without the fork those writes are inert and the sky stays the flat colour it always was.
local DayNightCycle = {
    properties = {
        { name = "dayLength", displayName = "Full day length (seconds)", type = "float", default = 240.0 },
        { name = "startHour", displayName = "Start hour (0..24)", type = "float", default = 9.0 },
        { name = "maxIntensity", displayName = "Noon intensity", type = "float", default = 1.2 },
        { name = "moonIntensity", displayName = "Moonlight intensity", type = "float", default = 0.12 },
        { name = "dawnColor", displayName = "Dawn colour (r, g, b)", type = "color3", default = { 0.95, 0.55, 0.35 } },
        { name = "noonColor", displayName = "Noon colour (r, g, b)", type = "color3", default = { 1.0, 0.97, 0.9 } },
        { name = "duskColor", displayName = "Dusk colour (r, g, b)", type = "color3", default = { 0.9, 0.45, 0.28 } },
        { name = "nightColor", displayName = "Night colour (r, g, b)", type = "color3", default = { 0.12, 0.15, 0.28 } },
        { name = "sunTilt", displayName = "Axis tilt (degrees)", type = "float", default = 23.5 },
        { name = "rotateAxis", displayName = "Sun path axis", type = "vector3", default = { 1.0, 0.0, 0.0 } },
        { name = "moonPhase", displayName = "Moon phase (0 new, 0.5 full)", type = "float", default = 0.5 },
        { name = "showStars", displayName = "Show stars", type = "bool", default = true },
        { name = "skyBrightness", displayName = "Sky brightness", type = "float", default = 1.0 },
        { name = "skyTint", displayName = "Zenith tint (r, g, b)", type = "color3", default = { 1.0, 1.0, 1.0 } },
        { name = "light", displayName = "Directional light entity", type = "entity" },
        { name = "moonLight", displayName = "Moon light entity", type = "entity" },
        { name = "sky", displayName = "Sky entity", type = "entity" },
        { name = "fog", displayName = "Fog entity", type = "entity" },
        { name = "fallbackLightName", displayName = "Light name if slot is empty", type = "string", default = "Sun" },
        { name = "fallbackMoonName", displayName = "Moon light name", type = "string", default = "Moon" },
        { name = "fallbackSkyName", displayName = "Sky name if slot is empty", type = "string", default = "Sky" },
        { name = "fallbackFogName", displayName = "Fog name if slot is empty", type = "string", default = "Fog" },
        { name = "setAmbient", displayName = "Drive ambient light", type = "bool", default = true },
        -- The clock owns the fog range, so these are the widest values it may reach: it pulls the
        -- far edge in as the sun drops. They are properties because the right distance is the
        -- scene's, not the clock's - a 900 m island and a 90 m room cannot share one number.
        { name = "fogNear", displayName = "Fog start at noon", type = "float", default = 220.0 },
        { name = "fogFar", displayName = "Fog end at noon", type = "float", default = 2400.0 },
        { name = "toggleKey", displayName = "Pause key (0 = none)", type = "int", default = 0 },
        { name = "onSunrise", displayName = "On sunrise: entity", type = "entity" },
        { name = "onSunriseMethod", displayName = "On sunrise: method", type = "string", default = "onSunrise" },
        { name = "onSunset", displayName = "On sunset: entity", type = "entity" },
        { name = "onSunsetMethod", displayName = "On sunset: method", type = "string", default = "onSunset" },
        { name = "onPhase", displayName = "On phase change: entity", type = "entity" },
        { name = "onPhaseMethod", displayName = "On phase change: method", type = "string", default = "onPhase" },
        { name = "onHour", displayName = "On hour: entity", type = "entity" },
        { name = "onHourMethod", displayName = "On hour: method", type = "string", default = "onHour" }
    }
}

local function clamp(v, lo, hi) if v < lo then return lo end; if v > hi then return hi end; return v end
-- A slot reaches Lua as a script table, an engine handle or a bare id, and 0 is NULL_ENTITY.
local function entityId(reference)
    if type(reference) == "number" then return reference ~= 0 and reference or nil end
    if reference and type(reference.entity) == "number" then
        return reference.entity ~= 0 and reference.entity or nil
    end
    return nil
end
-- The engine delivers a colour or axis row as Vector3 userdata and the Inspector default as a
-- table; the pcall keeps an entity handle from raising when it has no such channel.
local function asVec3(value, fallback)
    local ok, out = pcall(function()
        if type(value) ~= "table" and type(value) ~= "userdata" then return fallback end
        if type(value.x) == "number" then return Vector3(value.x, value.y or 0.0, value.z or 0.0) end
        if type(value[1]) == "number" then return Vector3(value[1], value[2] or 0.0, value[3] or 0.0) end
        return fallback
    end)
    return ok and out or fallback
end

local function lerp(a, b, t) return a + (b - a) * t end
local function smooth(t) t = clamp(t, 0.0, 1.0) return t * t * (3.0 - 2.0 * t) end
local function mix(a, b, t) return Vector3(lerp(a.x, b.x, t), lerp(a.y, b.y, t), lerp(a.z, b.z, t)) end
local function warn(self, message)
    if Engine.systemTime - self.lastWarn > 1.0 then
        self.lastWarn = Engine.systemTime; print("[DayNightCycle] " .. message)
    end
end
local function callHook(target, method, value)
    -- A hook slot only holds a script table when the target really carries that script.
    if type(target) ~= "table" or not method or method == "" then return end
    pcall(function()
        local fn = target[method]
        if type(fn) == "function" then fn(target, value) end
    end)
end
-- Lua cannot enumerate entities, so the sun/sky/fog must arrive as slots or names.
local function bind(self, cache, slot, fallbackName, class)
    if self[cache] then return self[cache] end
    local id = entityId(slot)
    if not id and fallbackName and fallbackName ~= "" then
        id = entityId(self.scene:findEntity(fallbackName))
    end
    if not id then return nil end
    local maker = class == "SkyBox" and SkyBox or class == "Fog" and Fog or Light
    local ok, handle = pcall(function() return maker(self.scene, id) end)
    if ok and handle then self[cache] = handle; return handle end
    return nil
end

-- Sorted by the hour each one opens at, so the clock's last passed entry wins and the range runs
-- to the next entry's start. Named in English because a game shows its own labels - the demo maps
-- these to "madrugada / manhã / dia / tarde / entardecer / noite".
local PHASES = {
    { name = "dawn", from = 4.5 },
    { name = "morning", from = 6.5 },
    { name = "day", from = 11.0 },
    { name = "afternoon", from = 15.0 },
    { name = "dusk", from = 18.5 },
    { name = "night", from = 20.0 }
}
local function phaseAt(hour)
    -- The seed is the phase that opens before midnight, so 03:00 answers "night" rather than
    -- falling through to the first entry of the table.
    local phase = "night"
    for _, entry in ipairs(PHASES) do
        if hour >= entry.from then phase = entry.name end
    end
    return phase
end

function DayNightCycle:init()
    RegisterEngineEvent(self, "onUpdate")
    self.dayLength = math.max(self.dayLength or 240.0, 1.0)
    self.fogNear, self.fogFar = math.max(self.fogNear or 220.0, 1.0), math.max(self.fogFar or 2400.0, 2.0)
    if self.fogFar <= self.fogNear then self.fogFar = self.fogNear * 8.0 end
    self.maxIntensity, self.moonIntensity = self.maxIntensity or 1.2, self.moonIntensity or 0.12
    self.cDawn = asVec3(self.dawnColor, Vector3(0.95, 0.55, 0.35))
    self.cNoon = asVec3(self.noonColor, Vector3(1.0, 0.97, 0.9))
    self.cDusk = asVec3(self.duskColor, Vector3(0.9, 0.45, 0.28))
    self.cNight = asVec3(self.nightColor, Vector3(0.12, 0.15, 0.28))
    self.hour = clamp(self.startHour or 9.0, 0.0, 23.999)
    self.elevation, self.paused, self.previousKey, self.lastWarn = 0.0, false, false, 0.0
    self.lastWholeHour, self.sunUp = math.floor(self.hour), self.hour >= 6.0 and self.hour < 18.0
    self.moonElevation, self.illumination = 0.0, 1.0
    self.sunVector, self.moonVector = Vector3(0.0, 1.0, 0.0), Vector3(0.0, -1.0, 0.0)
    self.phase = phaseAt(self.hour)
end

function DayNightCycle:setTime(hour)
    if type(hour) ~= "number" then return end
    self.hour = (hour % 24.0 + 24.0) % 24.0
    self.lastWholeHour = math.floor(self.hour)
end
function DayNightCycle:addHours(amount) if type(amount) == "number" then self:setTime(self.hour + amount) end end
function DayNightCycle:getHour() return self.hour end
function DayNightCycle:getSunElevation() return self.elevation end
function DayNightCycle:getMoonElevation() return self.moonElevation end
function DayNightCycle:getMoonPhase() return clamp(self.moonPhase or 0.5, 0.0, 0.999) end
function DayNightCycle:setMoonPhase(value)
    if type(value) == "number" then self.moonPhase = (value % 1.0 + 1.0) % 1.0 end
end
function DayNightCycle:getIllumination() return self.illumination end
function DayNightCycle:getPhase() return self.phase end
function DayNightCycle:getDayProgress() return self.hour / 24.0 end
function DayNightCycle:getSunDirection() return self.sunVector end
function DayNightCycle:getMoonDirection() return self.moonVector end
function DayNightCycle:setPaused(value) self.paused = value == true end

-- "07:24" - what a HUD prints, and what a headless run prints to prove the clock advanced.
function DayNightCycle:getHourText()
    local whole = math.floor(self.hour)
    local minutes = math.floor((self.hour - whole) * 60.0 + 0.5)
    if minutes > 59 then minutes = 59 end
    return string.format("%02d:%02d", whole, minutes)
end

-- The fork's u_fs_customParams members. Names must match the shader exactly: an unmatched member
-- keeps whatever the component holds, and a missing one is uploaded as zero.
function DayNightCycle:writeSkyUniforms(sky, sun, daylight, moon, moonlight, sunColour, tint)
    local exposure = 14.0
    local values = {
        sunDir = Vector4(sun.x, sun.y, sun.z, daylight),
        moonDir = Vector4(moon.x, moon.y, moon.z, moonlight),
        skyColour = Vector4(tint.x, tint.y, tint.z, 1.0),
        sunDisc = Vector4(sunColour.x * exposure, sunColour.y * exposure, sunColour.z * exposure, 0.011),
        moonDisc = Vector4(1.9, 1.95, 2.1, 0.011),
        atmosphere = Vector4(90.0, 0.5, clamp(self.skyBrightness or 1.0, 0.0, 8.0), 0.22),
        stars = Vector4(self.showStars ~= false and 1.0 or 0.0, 1.4, 0.55, 0.35)
    }
    for name, value in pairs(values) do
        sky:setShaderUniform(name, value)
    end
    local cameraId = entityId(self.scene.camera)
    if cameraId then
        local eye = Object(self.scene, cameraId)
        local p = eye.position
        -- w is the blend into the Sky's cubemap, which stays 0 for a purely procedural dome.
        sky:setShaderUniform("camPos", Vector4(p.x, p.y, p.z, 0.0))
    end
end

function DayNightCycle:onUpdate()
    local dt = math.min(Engine.deltatime, 0.05)
    if self.toggleKey and self.toggleKey > 0 then
        local down = Input.isKeyPressed(self.toggleKey)
        if down and not self.previousKey then self.paused = not self.paused end
        self.previousKey = down
    end
    if not self.paused then self.hour = (self.hour + dt * 24.0 / self.dayLength) % 24.0 end

    -- Sun on the great circle spanned by the rotate axis: 6 h rises, 12 h culminates, 18 h sets.
    local arc = (self.hour - 6.0) / 12.0 * math.pi
    local axis = asVec3(self.rotateAxis, Vector3(1.0, 0.0, 0.0))
    local east = axis:perpendicular():normalized()
    local zenith = east:crossProduct(axis):normalized()
    if zenith:length() < 0.001 then east, zenith = Vector3(0.0, 0.0, 1.0), Vector3(0.0, 1.0, 0.0) end
    local tilt = math.rad(self.sunTilt or 23.5)
    local pathUp = zenith * math.cos(tilt) + axis * math.sin(tilt)
    local sunPos = east * math.cos(arc) + pathUp * math.sin(arc)
    local length = math.max(sunPos:length(), 0.0001)
    local sun = sunPos / length
    self.sunVector = sun
    self.elevation = math.deg(math.asin(clamp(sun.y, -1.0, 1.0)))
    local dayFactor = smooth(self.elevation / 12.0)
    -- The lamp is the sun, so it is occluded the instant the sun drops under the horizon. The sky is
    -- not: scattered light keeps the dome, the ambient and the fog lit through about twelve degrees
    -- of twilight on each side. Using dayFactor for those makes sunset a cliff - one frame of glow
    -- and then full night - which is why dusk scenes read as a flat dark sheet.
    local skyDay = smooth((self.elevation + 12.0) / 24.0)

    -- The moon rides the same circle, pushed ahead of the sun by its elongation. That is the whole
    -- model: full moon (phase 0.5) is the antipode and so is overhead at midnight, and the shader
    -- gets the phase from the two directions instead of from a number that can disagree.
    local phase = clamp(self.moonPhase or 0.5, 0.0, 0.999)
    local elongation = phase * 2.0 * math.pi
    local moonArc = arc + elongation
    local moonPos = east * math.cos(moonArc) + pathUp * math.sin(moonArc)
    local moonLength = math.max(moonPos:length(), 0.0001)
    local moon = moonPos / moonLength
    self.moonVector = moon
    self.moonElevation = math.deg(math.asin(clamp(moon.y, -1.0, 1.0)))
    self.illumination = (1.0 - math.cos(elongation)) * 0.5
    local moonlight = smooth(self.moonElevation / 12.0) * (0.05 + 0.95 * self.illumination)

    -- Piecewise colour ramp: night 0 h, dawn 6 h, noon 12 h, dusk 18 h, night 24 h.
    local palette = { self.cNight, self.cDawn, self.cNoon, self.cDusk, self.cNight }
    local segment = clamp(math.floor(self.hour / 6.0), 0, 3)
    local sunColor = mix(palette[segment + 1], palette[segment + 2], smooth((self.hour % 6.0) / 6.0))

    local up = self.elevation > 0.0
    if up ~= self.sunUp then
        self.sunUp = up
        callHook(up and self.onSunrise or self.onSunset, up and self.onSunriseMethod or self.onSunsetMethod)
    end
    local wholeHour = math.floor(self.hour)
    if wholeHour ~= self.lastWholeHour then
        self.lastWholeHour = wholeHour
        callHook(self.onHour, self.onHourMethod, wholeHour)
    end
    local namedPhase = phaseAt(self.hour)
    if namedPhase ~= self.phase then
        self.phase = namedPhase
        callHook(self.onPhase, self.onPhaseMethod, namedPhase)
    end

    local light = bind(self, "lightHandle", self.light, self.fallbackLightName, "Light")
    local moonHandle = bind(self, "moonHandle", self.moonLight, self.fallbackMoonName, "Light")
    local sky = bind(self, "skyHandle", self.sky, self.fallbackSkyName, "SkyBox")
    local fog = bind(self, "fogHandle", self.fog, self.fallbackFogName, "Fog")
    local tint = asVec3(self.skyTint, Vector3(1.0, 1.0, 1.0))

    if not light then
        warn(self, "No sun: add a directional Light here or wire the Light slot (fallback: "
            .. tostring(self.fallbackLightName) .. ")")
    end
    local ok, err = pcall(function()
        -- Light/SkyBox/Fog colour setters are bound as scalar arguments, so the vector values go
        -- through the properties instead.
        if light then
            -- Without a Moon entity the night key light is the old one: the sun lamp swings down to
            -- moonIntensity and the scene keeps the behaviour it had before the moon existed.
            local nightFloor = moonHandle and 0.0 or self.moonIntensity
            local intensity = lerp(nightFloor, self.maxIntensity, dayFactor)
            light.direction = -sun; light:setIntensity(intensity); light.color = sunColor
        end
        if moonHandle then
            -- The moon lamp is a real second light, so a full moon casts shadows and a new moon
            -- casts nothing at all - which is the difference the single-lamp model could not make.
            moonHandle.direction = -moon
            moonHandle:setIntensity(self.moonIntensity * self.illumination * smooth(self.moonElevation / 12.0))
            moonHandle.color = Vector3(0.62, 0.72, 1.0)
        end
        if sky then
            -- Sky brightens with the sun but keeps a night floor so the horizon never goes black.
            -- With the sky_daynight fork assigned this row is not what you see - the fork owns the
            -- dome - but it is still what the lake reflects and what the ambient is scaled by.
            local s = 0.15 + 0.85 * skyDay
            sky.color = Vector4(sunColor.x * s, sunColor.y * s, sunColor.z * s, 1.0)
            self:writeSkyUniforms(sky, sun, skyDay, moon, moonlight, sunColor, tint)
        end
        if fog then
            -- Fog takes the sun's colour while there is sun and the night's colour after it. The raw
            -- palette entry stays orange well past sunset, and at the ranges a landscape needs that
            -- tint floods every distant hill with dusk light; hard-coded short ranges used to do the
            -- same thing by saturating the whole scene instead.
            fog.color = mix(sunColor, self.cNight, 1.0 - skyDay)
            fog:setLinearStartEnd(self.fogNear * lerp(0.27, 1.0, skyDay),
                                  self.fogFar * lerp(0.31, 1.0, skyDay))
        end
    end)
    if not ok then warn(self, "The wired Light/SkyBox/Fog rejected the write: " .. tostring(err)) end
    -- The Lua binding for ambient light is (intensity, colour): the single-vector overload exists in
    -- C++ but is not registered, so passing only the colour raises and the pcall hides it.
    if self.setAmbient ~= false then
        local ambient = 0.08 + 0.32 * skyDay + 0.05 * moonlight * self.illumination
        pcall(function() self.scene:setGlobalIllumination(ambient, sunColor) end)
    end
end

return DayNightCycle
