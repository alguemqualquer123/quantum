-- (c) SR VINIX
-- SPDX-License-Identifier: MIT
-- Lightning whose thunder arrives late: a point light flickers, a jagged bolt is drawn with
-- Lines, and the crack is scheduled by the speed of sound. Needs no wiring - it makes its own
-- light, bolt and sound entities - but every one of them can be replaced by a slot.
-- category: Environment & Motion
local Lightning = {
    properties = {
        { name = "weather", displayName = "WeatherSystem component", type = "WeatherSystem" },
        { name = "fallbackWeatherName", displayName = "Weather entity name if slot is empty", type = "string", default = "Weather" },
        { name = "followWeather", displayName = "Strike rate follows the weather intensity", type = "bool", default = true },
        { name = "stormThreshold", displayName = "Minimum intensity to strike (0..1)", type = "float", default = 0.35 },
        { name = "strikesPerMinute", displayName = "Strikes per minute at full storm", type = "float", default = 14.0 },
        { name = "minDistance", displayName = "Nearest strike (m)", type = "float", default = 30.0 },
        { name = "maxDistance", displayName = "Farthest strike (m)", type = "float", default = 900.0 },
        { name = "nearDistance", displayName = "Distance counted as close (m)", type = "float", default = 160.0 },
        { name = "cloudHeight", displayName = "Cloud height the bolt starts at (m)", type = "float", default = 65.0 },
        { name = "flashColor", displayName = "Flash colour (r, g, b)", type = "color3", default = { 0.78, 0.85, 1.0 } },
        { name = "flashIntensity", displayName = "Flash peak intensity", type = "float", default = 1400.0 },
        { name = "flashRange", displayName = "Flash range (m)", type = "float", default = 520.0 },
        { name = "flickerCount", displayName = "Flickers per strike", type = "int", default = 3 },
        { name = "flashDuration", displayName = "Flash duration (s)", type = "float", default = 0.45 },
        { name = "skyFlicker", displayName = "Faint flicker from distant strikes", type = "bool", default = true },
        { name = "boltSegments", displayName = "Bolt segments", type = "int", default = 14 },
        { name = "boltJitter", displayName = "Bolt sideways jitter (m)", type = "float", default = 9.0 },
        { name = "boltForks", displayName = "Fork branches", type = "int", default = 2 },
        { name = "boltLife", displayName = "Bolt visible for (s)", type = "float", default = 0.22 },
        { name = "boltEntity", displayName = "Lines entity (empty = make one)", type = "entity" },
        { name = "thunderPath", displayName = "Thunder sound file ('' = none)", type = "string", default = "sounds/thunder.ogg" },
        { name = "thunderVolume", displayName = "Thunder volume at close range", type = "float", default = 0.9 },
        { name = "speedOfSound", displayName = "Speed of sound (m/s, for the delay)", type = "float", default = 343.0 },
        { name = "rumbleRepeat", displayName = "Echo rumbles after the first crack", type = "int", default = 1 },
        { name = "thunderEntity", displayName = "Sound entity (empty = make one)", type = "entity" },
        { name = "shake", displayName = "CameraShake component", type = "CameraShake" },
        { name = "shakeAmount", displayName = "Trauma of a close strike", type = "float", default = 0.55 },
        { name = "fade", displayName = "ScreenFade component", type = "ScreenFade" },
        { name = "screenFlash", displayName = "Screen flash alpha of a close strike", type = "float", default = 0.3 },
        { name = "triggerKey", displayName = "Force a strike (key code, 0 = none)", type = "int", default = 0 },
        { name = "autoStart", displayName = "Storm as soon as the weather is over the threshold", type = "bool", default = true },
        { name = "onStrikeEntity", displayName = "On strike: entity", type = "entity" },
        { name = "onStrikeMethod", displayName = "On strike: method", type = "string", default = "onStrike" }
    }
}

local function clamp(v, lo, hi) if v < lo then return lo end; if v > hi then return hi end; return v end
local function number(v, fallback)
    if type(v) ~= "number" or v ~= v then return fallback end
    return v
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
        self.lastWarn = Engine.systemTime; print("[Lightning] " .. message)
    end
end
local function callHook(target, method, value)
    if type(target) ~= "table" or not method or method == "" then return end
    pcall(function()
        local fn = target[method]
        if type(fn) == "function" then fn(target, value) end
    end)
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

function Lightning:init()
    RegisterEngineEvent(self, "onUpdate")
    self.minDistance = clamp(number(self.minDistance, 30.0), 1.0, 4000.0)
    self.maxDistance = math.max(number(self.maxDistance, 900.0), self.minDistance + 1.0)
    self.nearDistance = clamp(number(self.nearDistance, 160.0), 1.0, self.maxDistance)
    self.cloudHeight = math.max(number(self.cloudHeight, 65.0), 5.0)
    self.flashIntensity = math.max(number(self.flashIntensity, 1400.0), 1.0)
    self.flashRange = math.max(number(self.flashRange, 520.0), 1.0)
    self.flashDuration = clamp(number(self.flashDuration, 0.45), 0.05, 4.0)
    self.boltLife = clamp(number(self.boltLife, 0.22), 0.02, 3.0)
    self.boltSegments = math.floor(clamp(number(self.boltSegments, 14), 3, 64))
    self.boltForks = math.floor(clamp(number(self.boltForks, 2), 0, 8))
    self.speedOfSound = math.max(number(self.speedOfSound, 343.0), 1.0)
    self.color = vec3(self.flashColor, { 0.78, 0.85, 1.0 })

    self.strikes = 0
    self.lastDistance = 0.0
    self.lastWarn = 0.0
    self.previousTrigger = false
    self.flash = nil
    self.bolt = nil
    self.pending = {}
    self.nextAt = Engine.systemTime + math.max(number(self.strikesPerMinute, 14.0), 0.0) * 0.0
    self.enabled = self.autoStart ~= false
    self:bindParts()
end

-- The flash light is the one engine object a strike cannot be faked without, so it is always
-- made here; the bolt and the thunder are optional and only built when first asked for.
function Lightning:bindParts()
    if self.flashLight then return end
    local scene = self.scene
    local ok, light = pcall(function()
        local handle = Light(scene)
        handle.name = "LightningFlash"
        handle.type = LightType.POINT
        handle.range = self.flashRange
        handle.intensity = 0.0
        handle.color = self.color
        handle.shadows = false
        return handle
    end)
    self.flashLight = (ok and light) or nil
    if not self.flashLight then warn(self, "could not create the flash light") end
end

function Lightning:bindBolt()
    if self.boltLines then return true end
    local scene = self.scene
    local id = entityId(self.boltEntity)
    if not id then id = entityId(scene:findEntity("LightningBolt")) end
    local ok, lines = pcall(function()
        if id then return Lines(scene, id) end
        local handle = Lines(scene)
        handle.name = "LightningBolt"
        return handle
    end)
    if not ok or not lines then
        self.boltLines = nil
        return false
    end
    pcall(function() lines.maxLines = self.boltSegments * (self.boltForks + 1) + 8 end)
    self.boltLines = lines
    return true
end

function Lightning:bindThunder()
    if self.thunderHandled then return self.thunderSound ~= nil end
    if self.thunderUnavailable then return false end
    if not self.thunderPath or self.thunderPath == "" then
        self.thunderUnavailable = true
        return false
    end
    local scene = self.scene
    local id = entityId(self.thunderEntity)
    if not id then id = entityId(scene:findEntity("Thunder")) end
    local ok, sound = pcall(function()
        if id then return Sound(scene, id) end
        local handle = Sound(scene, true)
        handle.name = "Thunder"
        return handle
    end)
    if not ok or not sound then
        self.thunderUnavailable = true
        return false
    end
    self.thunderId = sound.entity
    -- Sound(scene, true) is the only form that adds a Transform, so a thunder entity supplied
    -- through a slot has no position to place the crack at.
    self.thunderPlaced = (id == nil)
    local loaded = -1
    pcall(function() loaded = sound:loadSound(self.thunderPath) end)
    if type(loaded) ~= "number" or loaded < 0 then
        self.thunderUnavailable = true
        warn(self, "no thunder sample at '" .. tostring(self.thunderPath) .. "' (flashes stay silent)")
        return false
    end
    pcall(function()
        sound.looping = false
        sound.minDistance = 20.0
        sound.maxDistance = self.maxDistance * 1.6
    end)
    self.thunderSound = sound
    self.thunderHandled = true
    return true
end

-- A random ground point in the ring around the camera, so strikes frame the view instead of
-- landing on the player's head.
function Lightning:pickGround(eye)
    local angle = math.random() * math.pi * 2.0
    local span = self.maxDistance - self.minDistance
    local distance = self.minDistance + math.random() * span
    return Vector3(eye.x + math.cos(angle) * distance, 0.0,
        eye.z + math.sin(angle) * distance)
end

function Lightning:boltPoints(from, to)
    local points = { from }
    local segments = self.boltSegments
    local jitter = math.max(number(self.boltJitter, 9.0), 0.0)
    for i = 1, segments - 1 do
        local t = i / segments
        -- Ends stay fixed so the bolt still leaves the cloud and reaches the ground; only the
        -- middle of the path is free to zig-zag.
        local reach = jitter * math.sin(t * math.pi)
        points[#points + 1] = Vector3(
            from.x + (to.x - from.x) * t + (math.random() - 0.5) * 2.0 * reach,
            from.y + (to.y - from.y) * t + (math.random() - 0.5) * reach * 0.4,
            from.z + (to.z - from.z) * t + (math.random() - 0.5) * 2.0 * reach)
    end
    points[#points + 1] = to
    return points
end

function Lightning:drawBolt(from, to, alpha)
    if not self:bindBolt() then return end
    local lines = self.boltLines
    local peak = Vector4(self.color.x, self.color.y, self.color.z, alpha)
    local dim = Vector4(self.color.x, self.color.y, self.color.z, 0.0)
    pcall(function() lines:clearLines() end)
    local points = self:boltPoints(from, to)
    local ok = true
    pcall(function()
        for i = 1, #points - 1 do
            lines:addLine(points[i], points[i + 1], peak, peak)
        end
    end)
    if not ok then return end
    local forks = self.boltForks
    if forks > 0 then
        pcall(function()
            for f = 1, forks do
                local start = points[math.max(2, math.floor(#points * (0.25 + 0.5 * math.random())))]
                local branch = Vector3(to.x + (math.random() - 0.5) * self.nearDistance * 0.6, 0.0,
                    to.z + (math.random() - 0.5) * self.nearDistance * 0.6)
                local path = self:boltPoints(start, branch)
                for i = 1, #path - 1 do
                    lines:addLine(path[i], path[i + 1], peak, dim)
                end
            end
        end)
    end
end

function Lightning:queueThunder(point, distance)
    if not self:bindThunder() then return end
    local delay = distance / self.speedOfSound
    local volume = clamp(number(self.thunderVolume, 0.9), 0.0, 1.0) *
        clamp(1.0 - distance / (self.nearDistance * 3.0), 0.08, 1.0)
    local repeats = math.max(math.floor(number(self.rumbleRepeat, 1)), 0)
    for i = 0, repeats do
        self.pending[#self.pending + 1] = {
            at = Engine.systemTime + delay + i * (0.4 + math.random() * 0.7),
            point = point,
            volume = volume * (i == 0 and 1.0 or (0.45 / i))
        }
    end
end

-- Strikes are placed around the camera, so the eye comes from the scene camera entity: the
-- Engine namespace has no camera, and Scene.camera is a bare Entity.
function Lightning:cameraEye()
    local out
    pcall(function()
        local id = entityId(self.scene.camera)
        if id then
            local position = Object(self.scene, id).position
            if position and type(position.x) == "number" then out = position end
        end
    end)
    return out or Vector3(0.0, 0.0, 0.0)
end

function Lightning:weatherIntensity()
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

-- Public: fire one strike. A number is a distance from the camera, a point is the ground
-- location, an entity handle is struck where that entity stands, and nothing at all picks a
-- random spot. The three forms are tested in this order because a number must never be read
-- as an entity id - looking up a non-existent entity logs an engine error on every strike.
function Lightning:strike(target)
    local eye = self:cameraEye()
    local ground
    if type(target) == "number" then
        local angle = math.random() * math.pi * 2.0
        ground = Vector3(eye.x + math.cos(angle) * target, 0.0, eye.z + math.sin(angle) * target)
    elseif type(target) == "table" and type(target.x) == "number" then
        ground = Vector3(target.x, number(target.y, 0.0), number(target.z, target.x))
    else
        local id = entityId(target)
        if id then
            pcall(function() ground = Object(self.scene, id).position end)
        end
    end
    ground = ground or self:pickGround(eye)

    local dx, dz = ground.x - eye.x, ground.z - eye.z
    local distance = math.sqrt(dx * dx + dz * dz)
    self.lastDistance = distance
    self.strikes = self.strikes + 1

    local close = clamp(1.0 - distance / self.nearDistance, 0.0, 1.0)
    local cloud = Vector3(ground.x, self.cloudHeight, ground.z)
    self.flash = {
        started = Engine.systemTime,
        cloud = cloud,
        close = close,
        pulses = self:flickers()
    }
    if distance <= self.nearDistance * 1.6 then
        self:drawBolt(cloud, ground, 1.0)
        self.bolt = { at = Engine.systemTime, until_ = Engine.systemTime + self.boltLife }
    elseif self.skyFlicker then
        -- Far enough that the ground is out of sight: the cloud lights up and nothing else does.
        self.bolt = nil
    end
    self:queueThunder(ground, distance)

    local shake = self.shake
    if type(shake) == "table" and type(shake.addTrauma) == "function" and close > 0.0 then
        pcall(function() shake:addTrauma(close * math.max(number(self.shakeAmount, 0.55), 0.0)) end)
    elseif type(shake) == "userdata" then
        warn(self, "wire this: the entity in the CameraShake slot has no CameraShake script")
    end
    local fade = self.fade
    if type(fade) == "table" and type(fade.flashTo) == "function" and close > 0.05 then
        local alpha = close * math.max(number(self.screenFlash, 0.3), 0.0)
        pcall(function() fade:flashTo(self.color, alpha, self.flashDuration * 0.6) end)
    elseif type(fade) == "userdata" then
        warn(self, "wire this: the entity in the ScreenFade slot has no ScreenFade script")
    end

    callHook(self.onStrikeEntity, self.onStrikeMethod, ground)
    return distance
end

-- A strike is never one flash: the first is the brightest and each later one is weaker,
-- which is what makes it read as lightning rather than a bulb switching on.
function Lightning:flickers()
    local count = math.floor(clamp(number(self.flickerCount, 3), 1, 6))
    local pulses = {}
    for i = 1, count do
        pulses[i] = {
            at = (i - 1) * (self.flashDuration / count) * (0.6 + math.random() * 0.5),
            strength = i == 1 and 1.0 or clamp((1.0 / i) * (0.5 + math.random() * 0.7), 0.05, 0.9)
        }
    end
    return pulses
end

function Lightning:setEnabled(value) self.enabled = value == true end
function Lightning:start() self.enabled = true end
function Lightning:stop()
    self.enabled = false
    self.flash = nil
    if self.flashLight then pcall(function() self.flashLight.intensity = 0.0 end) end
    if self.boltLines then pcall(function() self.boltLines:clearLines() end) end
end
function Lightning:getStrikeCount() return self.strikes end
function Lightning:getDistance() return self.lastDistance end

function Lightning:onUpdate()
    local dt = math.min(Engine.deltatime, 0.05)
    local now = Engine.systemTime

    local key = number(self.triggerKey, 0)
    if key > 0 then
        local down = Input.isKeyPressed(key)
        if down and not self.previousTrigger then self:strike() end
        self.previousTrigger = down
    end

    if self.enabled then
        local intensity = self:weatherIntensity()
        if intensity >= self.stormThreshold then
            if self.nextAt <= now then
                self:strike()
                local rate = math.max(number(self.strikesPerMinute, 14.0), 0.01) * intensity
                -- Exponential gaps: storms cluster their strikes instead of metronoming them.
                self.nextAt = now - math.log(1.0 - math.random()) / (rate / 60.0)
            end
        else
            self.nextAt = now
        end
    end

    if self.flash then
        local elapsed = now - self.flash.started
        local value = 0.0
        for _, pulse in ipairs(self.flash.pulses) do
            if elapsed >= pulse.at and elapsed < pulse.at + self.flashDuration / 3.0 then
                local local_t = (elapsed - pulse.at) / (self.flashDuration / 3.0)
                value = math.max(value, pulse.strength * (1.0 - local_t))
            end
        end
        pcall(function()
            self.flashLight.position = self.flash.cloud
            self.flashLight.intensity = value * self.flashIntensity
        end)
        if elapsed >= self.flashDuration then
            pcall(function() self.flashLight.intensity = 0.0 end)
            self.flash = nil
        end
    end

    if self.bolt and now >= self.bolt.until_ then
        if self.boltLines then pcall(function() self.boltLines:clearLines() end) end
        self.bolt = nil
    end

    local index = 1
    while index <= #self.pending do
        local crack = self.pending[index]
        if now >= crack.at then
            -- Sound is not an Object in the binding, so its 3D position goes through the
            -- entity Transform; the play call stays out of that pcall so a failed move cannot
            -- mute the crack.
            if self.thunderPlaced then
                pcall(function() Object(self.scene, self.thunderId).position = crack.point end)
            end
            pcall(function()
                self.thunderSound.volume = crack.volume
                self.thunderSound:play()
            end)
            table.remove(self.pending, index)
        else
            index = index + 1
        end
    end
end

return Lightning
