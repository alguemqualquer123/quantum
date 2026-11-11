-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Level transition that preloads the destination while the player walks in and only loads once the
-- category: Interaction & Combat
-- assets are ready. Attach it to the door or exit entity and drag the player into the Subject slot.
local ScenePortal = {
    properties = {
        { name = "destination", displayName = "Destination scene", type = "string", default = "Cave01" },
        { name = "loadingScene", displayName = "Loading scene (empty = none)", type = "string", default = "" },
        { name = "preloadEarly", displayName = "Preload from the start", type = "bool", default = true },
        { name = "subject", displayName = "Subject", type = "entity" },
        { name = "fallbackName", displayName = "Subject name if slot is empty", type = "string", default = "Player" },
        { name = "radius", displayName = "Trigger radius", type = "float", default = 3.0 },
        { name = "key", displayName = "Load key (0 = Enter)", type = "int", default = 0 },
        { name = "autoLoad", displayName = "Load as soon as it is ready", type = "bool", default = false },
        { name = "fade", displayName = "Fade (ScreenFade component)", type = "ScreenFade" },
        { name = "fadeTime", displayName = "Fade time (seconds)", type = "float", default = 0.6 },
        { name = "promptText", displayName = "Prompt text", type = "string", default = "Press Enter" },
        { name = "prompt", displayName = "Prompt: Text entity", type = "entity" },
        { name = "hidePromptWhenReady", displayName = "Hide the prompt once ready", type = "bool", default = false },
        { name = "bar", displayName = "Progress: Progressbar entity", type = "entity" },
        { name = "barText", displayName = "Progress: Text entity", type = "entity" }
    }
}

-- A slot reaches Lua as a script table, an engine handle or the bare stored id (0 = NULL_ENTITY).
local function entityId(reference)
    if type(reference) == "number" then return reference ~= 0 and reference or nil end
    if type(reference) ~= "table" and type(reference) ~= "userdata" then return nil end
    return reference.entity ~= 0 and reference.entity or nil
end
local function positionOf(scene, id)
    if not id then return nil end
    local ok, position = pcall(function() return Object(scene, id).position end)
    return (ok and position) or nil
end
-- Handles are cheap but not free: keep one per resolved id until the slot points somewhere else.
local function widget(scene, cache, key, reference, class)
    local id = entityId(reference)
    if not id then return nil end
    local entry = cache[key]
    if not entry or entry.id ~= id then
        local ok, handle = pcall(class, scene, id)
        if not ok or not handle then return nil end
        entry = { id = id, handle = handle }
        cache[key] = entry
    end
    return entry.handle
end

function ScenePortal:init()
    RegisterEngineEvent(self, "onUpdate")
    self.radius = math.max(tonumber(self.radius) or 3.0, 0.05)
    self.fadeTime = math.max(tonumber(self.fadeTime) or 0.6, 0.0)
    if not self.key or self.key == 0 then self.key = Input.KEY_ENTER end
    self.handles, self.lastWarn = {}, {}
    self.progress, self.preloading, self.inRange, self.previousKey = 0.0, false, false, false
    self.traveling, self.fadeLeft, self.held = false, 0.0, false
    local ok, object = pcall(function() return Object(self.scene, self.entity) end)
    self.object = (ok and object) or nil
    if self.preloadEarly == true then self:preload() end
end

function ScenePortal:warn(key, message)
    local last = self.lastWarn[key]
    if not last or Engine.systemTime - last > 1.0 then
        self.lastWarn[key] = Engine.systemTime
        print("[ScenePortal] " .. message)
    end
end

function ScenePortal:preload()
    if self.preloading or not self.destination or self.destination == "" then return false end
    local ok = pcall(function() return SceneManager.preloadScene(self.destination) end)
    self.preloading = ok
    return ok
end

function ScenePortal:getProgress()
    if self.progress >= 1.0 then return 1.0 end
    local ok, value = pcall(function() return SceneManager.getPreloadProgress(self.destination) end)
    if ok and type(value) == "number" then self.progress = math.max(0.0, math.min(1.0, value)) end
    return self.progress
end

function ScenePortal:release()
    if not self.held then return end
    pcall(function() SceneManager.releaseLoading() end)
    self.held = false
end

function ScenePortal:travel()
    if not self.destination or self.destination == "" then return false end
    self.traveling = true
    local loading = self.loadingScene
    if loading ~= nil and loading ~= "" then pcall(function() SceneManager.setLoadingScene(loading) end) end
    -- Whoever holds the loading must release it right before the swap, or the engine waits forever.
    self:release()
    return pcall(function() return SceneManager.loadScene(self.destination) end)
end

function ScenePortal:beginTravel()
    if self.traveling then return false end
    local fade = self.fade
    if self.fadeTime <= 0.0 or type(fade) ~= "table" or type(fade.fadeOut) ~= "function" then return self:travel() end
    self.traveling, self.fadeLeft = true, self.fadeTime
    -- holdLoading keeps this scene on screen while the fade runs, so the swap waits for it.
    self.held = pcall(function() return SceneManager.holdLoading() end) and true or false
    pcall(function() fade:fadeOut(self.fadeTime) end)
    return true
end

function ScenePortal:cancel()
    if self.preloading then pcall(function() SceneManager.cancelPreload(self.destination) end) end
    self:release()
    self.traveling, self.fadeLeft, self.progress, self.preloading = false, 0.0, 0.0, false
end

function ScenePortal:setDestination(name)
    if type(name) ~= "string" or name == "" or name == self.destination then return false end
    self:cancel()
    self.destination = name
    if self.preloadEarly == true then self:preload() end
    return true
end

function ScenePortal:report(inRange)
    local bar = widget(self.scene, self.handles, "bar", self.bar, Progressbar)
    if bar then bar.value = self.progress end
    local label = widget(self.scene, self.handles, "barText", self.barText, Text)
    if label then label.text = string.format("%d%%", math.floor(self.progress * 100.0 + 0.5)) end
    if inRange and not bar and not label then
        self:warn("progress", "wire this: a Progressbar or Text entity in Bar shows the load state")
    end
    local prompt = widget(self.scene, self.handles, "prompt", self.prompt, Text)
    if not prompt then if inRange then self:warn("prompt", "wire this: a Text entity in Prompt shows the load hint") end return end
    local ready = self.hidePromptWhenReady == true and self.progress >= 1.0
    prompt.text, prompt.alpha = self.promptText or "", (inRange and not ready) and 1.0 or 0.0
end

function ScenePortal:onUpdate()
    if self.traveling then
        if self.fadeLeft > 0.0 then
            self.fadeLeft = self.fadeLeft - math.min(Engine.deltatime, 0.05)
            if self.fadeLeft <= 0.0 then self:travel() end
        end
        return
    end
    -- The name is the fallback for an empty slot, so a freshly dragged-in player is not required.
    local id = entityId(self.subject) or entityId(self.scene:findEntity(self.fallbackName or ""))
    local point = positionOf(self.scene, id)
    if not point then self:warn("subject", "wire this: drag the player into the Subject slot") end
    self.inRange = point ~= nil and self.object ~= nil and self.object.position:distance(point) <= self.radius
    if self.inRange then self:preload() end
    -- Polling the preload every frame is only worth it while the player actually stands there.
    self:getProgress()
    self:report(self.inRange)
    if not self.inRange then return end
    if self.autoLoad == true and self.progress >= 1.0 then return self:beginTravel() end
    local pressed = Input.isKeyPressed(self.key)
    if pressed and not self.previousKey then self:beginTravel() end
    self.previousKey = pressed
end

return ScenePortal
