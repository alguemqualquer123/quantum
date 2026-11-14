-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Soundtrack with crossfade and per-state ducking (menu, combat, dead): attach it to any entity and
-- category: Audio & Particles
-- drag two entities that have a Sound component into Channel A and B, then call setState("combat").
local MusicPlayer = {
    properties = {
        { name = "tracks", displayName = "Track files (comma separated)", type = "string", default = "music/theme.ogg" },
        { name = "index", displayName = "Starting track (1 = first)", type = "int", default = 1 },
        { name = "loop", displayName = "Loop the track", type = "bool", default = true },
        { name = "volume", displayName = "Master volume", type = "float", default = 1.0 },
        { name = "crossfadeSeconds", displayName = "Crossfade (seconds)", type = "float", default = 1.5 },
        { name = "channelA", displayName = "Channel A: entity with Sound", type = "entity" },
        { name = "channelB", displayName = "Channel B: entity with Sound", type = "entity" },
        { name = "shuffle", displayName = "Shuffle the playlist", type = "bool", default = false },
        { name = "autoplay", displayName = "Play on scene start", type = "bool", default = true },
        { name = "stateVolumes", displayName = "State volumes (menu:1.0,combat:0.7)", type = "string", default = "" }
    }
}
local function number(value, fallback) return (type(value) == "number" and value == value) and value or fallback end
local function clamp(value, low, high) return math.min(math.max(value, low), high) end
local function split(text)
    local list = {}
    for entry in tostring(text or ""):gmatch("[^,]+") do
        local item = entry:match("^%s*(.-)%s*$")
        if item ~= "" then list[#list + 1] = item end
    end
    return list
end
function MusicPlayer:init()
    RegisterEngineEvent(self, "onUpdate")
    self.list = split(self.tracks)
    self.stateGain, self.stateWarned, self.stateGains = 1.0, false, {}
    -- a malformed "name:value" entry is dropped, so the volume kept for that state stands
    local malformed = 0
    for entry in tostring(self.stateVolumes or ""):gmatch("[^,]+") do
        local name, value = entry:match("^%s*(.-)%s*:%s*(.-)%s*$")
        local parsed = name and name ~= "" and tonumber(value)
        if parsed then self.stateGains[name] = clamp(parsed, 0.0, 1.0) else malformed = malformed + 1 end
    end
    self.cursor = clamp(math.floor(number(self.index, 1)), 1, math.max(#self.list, 1))
    self.channels, self.active, self.sinceWarning = {}, nil, 1.0
    local function addChannel(slot)
        local id = (type(slot) == "table" or type(slot) == "userdata") and slot.entity or slot
        local ok, handle = type(id) == "number" and id ~= 0, nil
        if ok then ok, handle = pcall(function() return Sound(self.scene, id) end) end
        if ok and handle then
            handle.sound3D, handle.volume, handle.looping = false, 0.0, self.loop ~= false
            self.channels[#self.channels + 1] = { sound = handle, track = nil, gain = 0.0, rate = 0.0, target = 0.0, pending = nil }
        end
    end
    addChannel(self.channelA) addChannel(self.channelB)
    if malformed > 0 then print("[MusicPlayer] " .. malformed .. " malformed stateVolumes entries ignored.") end
    if self.autoplay ~= false then self:play(self.cursor) end
end
function MusicPlayer:applyVolumes()
    local master = clamp(number(self.volume, 1.0), 0.0, 1.0) * self.stateGain
    for _, channel in ipairs(self.channels) do channel.sound.volume = clamp(channel.gain * master, 0.0, 1.0) end
end
function MusicPlayer:fadeSeconds() return math.max(number(self.crossfadeSeconds, 1.5), 0.05) end
function MusicPlayer:start(channel, track, rate)
    local path, loaded = self.list[track], nil
    local ok = pcall(function() loaded = channel.sound:loadSound(path) end)
    if not ok or loaded == nil or loaded == false then
        print("[MusicPlayer] Cannot load \"" .. tostring(path) .. "\".") return false end
    channel.track, channel.gain, channel.target, channel.rate = track, 0.0, 1.0, rate
    channel.sound.looping = self.loop ~= false
    pcall(function() channel.sound:play() end)
    self.active = channel
    return true
end

-- No mixer in the engine, so a crossfade is two voices: the idle one loads the new track and rises
-- while the current one sinks. A single voice queues the switch instead (pending): fade out, swap the
-- file once silent, fade back in.
function MusicPlayer:switchTo(track)
    local rate, outgoing = 1.0 / self:fadeSeconds(), self.active
    local incoming = self.channels[1] == outgoing and (self.channels[2] or self.channels[1]) or self.channels[1]
    for _, channel in ipairs(self.channels) do
        if channel.track == track and channel ~= outgoing then incoming = channel end end
    if incoming == outgoing then
        outgoing.pending, outgoing.target, outgoing.rate = track, 0.0, -rate
        return
    end
    if outgoing then outgoing.pending, outgoing.target, outgoing.rate = nil, 0.0, -rate end
    self:start(incoming, track, rate)
end
function MusicPlayer:play(indexOrName)
    if #self.channels == 0 or #self.list == 0 then return false end
    local track = self.cursor
    if type(indexOrName) == "number" then track = clamp(math.floor(indexOrName), 1, #self.list)
    elseif type(indexOrName) == "string" then
        for i, path in ipairs(self.list) do
            if path == indexOrName or path:find(indexOrName, 1, true) then track = i end end
    end
    if self.active and self.active.track == track then return true end
    self.cursor = track
    self:switchTo(track)
    return true
end
function MusicPlayer:next()
    local step = (self.shuffle ~= false and #self.list > 1) and math.random(#self.list - 1) or 1
    return self:play((self.cursor + step - 1) % math.max(#self.list, 1) + 1)
end
function MusicPlayer:previous() return self:play((self.cursor - 2) % math.max(#self.list, 1) + 1) end
function MusicPlayer:stop()
    for _, channel in ipairs(self.channels) do
        channel.gain, channel.track, channel.pending, channel.rate = 0.0, nil, nil, 0.0
        pcall(function() channel.sound:stop() end) end
    self.active = nil
end
function MusicPlayer:pause() if self.active then pcall(function() self.active.sound:pause() end) end end
function MusicPlayer:resume() if self.active then pcall(function() self.active.sound:play() end) end end
function MusicPlayer:setVolume(value) self.volume = clamp(number(value, 1.0), 0.0, 1.0) self:applyVolumes() end
function MusicPlayer:setState(name)
    local gain = self.stateGains[name]
    if not gain and not self.stateWarned then self.stateWarned = true
        print("[MusicPlayer] State missing from stateVolumes, volume kept: " .. tostring(name)) end
    self.stateGain = gain or self.stateGain
    self:applyVolumes()
end
function MusicPlayer:getTrack() return self.list[self.cursor] or "", self.cursor end
function MusicPlayer:getLength() return self:readTime("getLength") end
function MusicPlayer:getPlayingTime() return self:readTime("getPlayingTime") end
function MusicPlayer:readTime(method)
    local channel = self.active or self.channels[1]
    if not channel then return 0.0 end
    local ok, value = pcall(function() return channel.sound[method](channel.sound) end)
    return (ok and type(value) == "number") and value or 0.0
end

function MusicPlayer:onUpdate()
    local dt = math.min(Engine.deltatime, 0.05)
    if #self.channels == 0 then
        self.sinceWarning = self.sinceWarning + dt
        if self.sinceWarning >= 1.0 then self.sinceWarning = 0.0
            print("[MusicPlayer] Wire Channel A (and B for a crossfade): entities with a Sound component.") end
        return
    end
    for _, channel in ipairs(self.channels) do
        if channel.rate ~= 0.0 then
            channel.gain = clamp(channel.gain + channel.rate * dt, 0.0, 1.0)
            if channel.gain == channel.target then
                channel.rate = 0.0
                if channel.gain == 0.0 then
                    local track = channel.pending
                    channel.track, channel.pending = nil, nil
                    pcall(function() channel.sound:stop() end)
                    if track then self:start(channel, track, 1.0 / self:fadeSeconds()) end end
            end
        end
    end
    self:applyVolumes()
    local active = self.active
    if active and active.track and self.loop == false and active.rate == 0.0 then
        local length = self:getLength()
        if length > 0.0 and self:getPlayingTime() >= length - 0.05 then self:next() end end
end

return MusicPlayer
