-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- One-shot sound that other components poke: a hit, a footstep, a click, a random variation. Attach
-- category: Audio & Particles
-- it to the entity that has the Sound component and call self.sfx:play() from any other script.
local SoundOnEvent = {
    properties = {
        { name = "paths", displayName = "Sound files (comma separated)", type = "string", default = "sounds/hit.ogg" },
        { name = "sound", displayName = "Sound (0 = this entity)", type = "entity" },
        { name = "subject", displayName = "Position source", type = "entity" },
        { name = "volume", displayName = "Master volume", type = "float", default = 1.0 },
        { name = "minVolume", displayName = "Random volume: min", type = "float", default = 0.85 },
        { name = "maxVolume", displayName = "Random volume: max", type = "float", default = 1.0 },
        { name = "pitch", displayName = "Base pitch (playback speed)", type = "float", default = 1.0 },
        { name = "pitchJitter", displayName = "Pitch jitter (+/-)", type = "float", default = 0.08 },
        { name = "cooldownSeconds", displayName = "Retrigger cooldown (seconds)", type = "float", default = 0.05 },
        { name = "randomize", displayName = "Pick a random file per play", type = "bool", default = true },
        { name = "positioned", displayName = "Play at the subject position", type = "bool", default = true },
        { name = "triggerKey", displayName = "Play key (0 = off)", type = "int", default = 0 },
        { name = "sound3D", displayName = "3D positional", type = "bool", default = true },
        { name = "onPlay", displayName = "On play: entity", type = "entity" },
        { name = "onPlayMethod", displayName = "On play: method", type = "string", default = "onSoundPlayed" }
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
-- A point arrives as Vector3 userdata from the engine or as a table from Lua callers and the
-- Inspector; the pcall keeps a handle with no such channel from raising.
local function isPoint(value)
    if type(value) ~= "table" and type(value) ~= "userdata" then return false end
    local ok, held = pcall(function() return type(value.x) == "number" end)
    return ok and held or false
end
local function splitPaths(text)
    local list = {}
    for entry in tostring(text or ""):gmatch("[^,]+") do
        local path = entry:match("^%s*(.-)%s*$")
        if path ~= "" then list[#list + 1] = path end
    end
    return list
end
local function callMethod(reference, methodName, entity)
    local method = type(reference) == "table" and type(reference[methodName]) == "function" and reference[methodName]
    if not method then return false end
    method(reference, entity)
    return true
end
-- One Sound voice per entity is all Lua gets (no mixer, no pool): maxConcurrent is effectively 1, a
-- second play() steals the voice instead of layering it, and only the last loaded file stays here.
function SoundOnEvent:init()
    RegisterEngineEvent(self, "onUpdate")
    self.files = splitPaths(self.paths)
    self.loadedPath, self.lastPlay = nil, -1000.0
    self.keyWasDown, self.warningShown = false, false
    local ok, handle = pcall(function() return Sound(self.scene, entityId(self.sound) or self.entity) end)
    self.sound = (ok and handle) or nil
    if self.sound then
        self.sound.sound3D, self.sound.looping = self.sound3D ~= false, false
        self.sound.speed = number(self.pitch, 1.0)
    end
end
function SoundOnEvent:warn(text)
    if not self.warningShown then
        self.warningShown = true
        print("[SoundOnEvent] " .. text)
    end
end
function SoundOnEvent:load(path)
    local loaded
    local ok = pcall(function() loaded = self.sound:loadSound(path) end)
    if not ok or loaded == nil or loaded == false then return false end
    self.loadedPath = path
    return true
end
function SoundOnEvent:preload() return self.sound ~= nil and #self.files > 0 and self:load(self.files[1]) end
function SoundOnEvent:pickPath()
    if #self.files == 0 then return nil end
    if self.randomize == false or #self.files == 1 then return self.files[1] end
    return self.files[math.random(#self.files)]
end
function SoundOnEvent:setEmitter(point)
    local id = entityId(self.sound) or self.entity
    pcall(function() Object(self.scene, id).position = point end)
end

function SoundOnEvent:play(pointOrNil)
    if not self.sound then
        self:warn("The entity needs a Sound component.")
        return false
    end
    if Engine.systemTime - self.lastPlay < math.max(number(self.cooldownSeconds, 0.05), 0.0) then
        return false
    end
    local path = self:pickPath()
    if not path then
        self:warn("No sound file set.")
        return false
    end
    if path ~= self.loadedPath and not self:load(path) then
        self:warn("Cannot load \"" .. tostring(path) .. "\".")
        return false
    end
    self.lastPlay = Engine.systemTime

    local low = number(self.minVolume, 0.85)
    local high = math.max(low, number(self.maxVolume, 1.0))
    self.sound.volume = clamp(number(self.volume, 1.0) * (low + (high - low) * math.random()), 0.0, 1.0)
    local jitter = clamp(number(self.pitchJitter, 0.08), 0.0, 0.5)
    self.sound.speed = math.max(number(self.pitch, 1.0) * (1.0 + (math.random() * 2.0 - 1.0) * jitter), 0.05)

    if self.positioned ~= false then
        local point = isPoint(pointOrNil) and pointOrNil or positionOf(self.scene, self.subject)
        if point then self:setEmitter(point) end
    end
    pcall(function() self.sound:play() end)
    callMethod(self.onPlay, self.onPlayMethod, self.entity)
    return true
end

function SoundOnEvent:playAt(path, position)
    if not self.sound or type(path) ~= "string" or path == "" then return false end
    if not self:load(path) then return false end
    self.lastPlay = Engine.systemTime
    self.sound.volume = clamp(number(self.volume, 1.0), 0.0, 1.0)
    if isPoint(position) then self:setEmitter(position) end
    pcall(function() self.sound:play() end)
    callMethod(self.onPlay, self.onPlayMethod, self.entity)
    return true
end
function SoundOnEvent:stopAll()
    if not self.sound then return false end
    pcall(function() self.sound:stop() end)
    return true
end
function SoundOnEvent:setVolume(value)
    self.volume = clamp(number(value, 1.0), 0.0, 1.0)
    if self.sound then self.sound.volume = self.volume end
end

function SoundOnEvent:onUpdate()
    local key = number(self.triggerKey, 0)
    if key <= 0 then return end
    local down = Input.isKeyPressed(key)
    if down and not self.keyWasDown then self:play(nil) end
    self.keyWasDown = down
end

return SoundOnEvent
