-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Looping 3D ambience (wind, rain, machinery) that fades with the distance to the listener and takes
-- category: Audio & Particles
-- an external intensity. Attach it to the entity that has the Sound component.
local AmbientSound = {
    properties = {
        { name = "file", displayName = "Sound file", type = "string", default = "" },
        { name = "sound", displayName = "Sound (0 = this entity)", type = "entity" },
        { name = "listener", displayName = "Listener", type = "entity" },
        { name = "fallbackName", displayName = "Listener name if slot is empty", type = "string", default = "Player" },
        { name = "volume", displayName = "Master volume", type = "float", default = 1.0 },
        { name = "nearVolume", displayName = "Volume when close", type = "float", default = 1.0 },
        { name = "farVolume", displayName = "Volume at max distance", type = "float", default = 0.0 },
        { name = "minDistance", displayName = "Min distance (full volume below)", type = "float", default = 4.0 },
        { name = "maxDistance", displayName = "Max distance (silent above)", type = "float", default = 60.0 },
        { name = "stereo", displayName = "Stereo pan toward the listener", type = "bool", default = false },
        { name = "looping", displayName = "Loop", type = "bool", default = true },
        { name = "doppler", displayName = "Doppler factor", type = "float", default = 0.0 },
        { name = "startOnLoad", displayName = "Play on scene start", type = "bool", default = true },
        { name = "intensity", displayName = "External intensity (0..1)", type = "float", default = 1.0 },
        { name = "speed", displayName = "Playback speed", type = "float", default = 1.0 }
    }
}

local function number(value, fallback) return (type(value) == "number" and value == value) and value or fallback end
local function clamp(value, low, high) return math.min(math.max(value, low), high) end

local function entityId(reference)
    if type(reference) == "number" then return reference ~= 0 and reference or nil end
    if reference and type(reference.entity) == "number" then return reference.entity ~= 0 and reference.entity or nil end
    return nil
end

local function positionOf(scene, reference)
    local id = entityId(reference)
    if not id then return nil end
    local ok, position = pcall(function() return Object(scene, id).position end)
    return (ok and position) or nil
end

function AmbientSound:init()
    RegisterEngineEvent(self, "onUpdate")

    self.gain = 1.0
    self.rate = nil
    self.fadeTarget = 1.0
    self.running = true
    self.intensity = clamp(number(self.intensity, 1.0), 0.0, 1.0)

    local ok, handle = pcall(function()
        return Sound(self.scene, entityId(self.sound) or self.entity)
    end)
    self.sound = (ok and handle) or nil
    if not self.sound then
        self:disable("The entity needs a Sound component.")
    elseif not self.file or self.file == "" then
        self:disable("No sound file set.")
    else
        local loaded
        local okLoad = pcall(function() loaded = self.sound:loadSound(self.file) end)
        if not okLoad or loaded == nil or loaded == false then
            self:disable("Cannot load \"" .. self.file .. "\".")
        else
            self.sound.sound3D = true
            self.sound.looping = self.looping ~= false
            self.sound.speed = number(self.speed, 1.0)
            self.sound.dopplerFactor = number(self.doppler, 0.0)
            self.sound:setMinMaxDistance(number(self.minDistance, 4.0), number(self.maxDistance, 60.0))
            if self.startOnLoad ~= false then pcall(function() self.sound:play() end) end
            self:attenuate()
        end
    end
end

function AmbientSound:disable(text)
    self.running = false
    print("[AmbientSound] " .. text .. " Disabling.")
end

-- The engine gives Lua one voice and no mixer: distance, intensity and master all multiply here.
function AmbientSound:applyVolume()
    if self.sound then
        self.sound.volume = clamp(self.gain * self.intensity * number(self.volume, 1.0), 0.0, 1.0)
    end
end

function AmbientSound:attenuate()
    if not self.sound then return end
    local id = entityId(self.listener)
    if not id and self.fallbackName and self.fallbackName ~= "" then
        id = entityId(self.scene:findEntity(self.fallbackName))
    end
    local emitter = entityId(self.sound) or self.entity
    local listener = positionOf(self.scene, id or entityId(self.scene.camera) or emitter)
    local source = positionOf(self.scene, emitter)
    if not listener or not source then return end

    local minimum = number(self.minDistance, 4.0)
    local maximum = math.max(number(self.maxDistance, 60.0), minimum + 0.01)
    local dx, dy, dz = listener.x - source.x, listener.y - source.y, listener.z - source.z
    local t = clamp((math.sqrt(dx * dx + dy * dy + dz * dz) - minimum) / (maximum - minimum), 0.0, 1.0)
    -- smoothstep keeps the fall-off from clipping hard at both ends
    local near = number(self.nearVolume, 1.0)
    self.gain = near + (number(self.farVolume, 0.0) - near) * t * t * (3.0 - 2.0 * t)

    if self.stereo then
        self.sound.pan = clamp((listener.x - source.x) / maximum, -1.0, 1.0)
    end
    self:applyVolume()
end

function AmbientSound:onUpdate()
    if not self.running then return end

    local rate = self.rate
    if rate then
        self.intensity = clamp(self.intensity + rate * math.min(Engine.deltatime, 0.05), 0.0, 1.0)
        if (rate > 0.0 and self.intensity >= self.fadeTarget)
            or (rate < 0.0 and self.intensity <= self.fadeTarget) then
            self.intensity, self.rate, self.fadeTarget = self.fadeTarget, nil, nil
        end
    end
    self:attenuate()
end

function AmbientSound:setIntensity(value)
    self.rate, self.fadeTarget = nil, nil
    self.intensity = clamp(number(value, 1.0), 0.0, 1.0)
    self:applyVolume()
end

function AmbientSound:rampTo(target, seconds)
    self.fadeTarget = target
    local span = number(seconds, 1.0)
    if span <= 0.0 or span ~= span then
        self:setIntensity(target)
    else
        self.rate = (target - self.intensity) / span
    end
end

function AmbientSound:fadeIn(seconds) self:rampTo(1.0, seconds) end
function AmbientSound:fadeOut(seconds) self:rampTo(0.0, seconds) end

function AmbientSound:stop()
    if self.sound then pcall(function() self.sound:stop() end) end
end

function AmbientSound:resume()
    if self.sound then pcall(function() self.sound:play() end) end
end

return AmbientSound
