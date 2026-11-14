-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Walk up and press a key: doors, levers, chests, talk prompts, button pads. Attach it to the thing
-- category: Interaction & Combat
-- the player uses, drag the player into Subject, and drop the HUD widgets in the prompt slots.
local Interactable = {
    properties = {
        { name = "subject", displayName = "Subject", type = "entity" },
        { name = "fallbackName", displayName = "Subject name if slot is empty", type = "string", default = "Player" },
        { name = "camera", displayName = "Camera (look check only)", type = "entity" },
        { name = "radius", displayName = "Use radius", type = "float", default = 3.0 },
        { name = "requireLooking", displayName = "Require looking at it", type = "bool", default = false },
        { name = "lookTolerance", displayName = "Look tolerance (degrees)", type = "float", default = 12.0 },
        { name = "key", displayName = "Use key (0 = E)", type = "int", default = 0 },
        { name = "once", displayName = "Usable once only", type = "bool", default = false },
        { name = "cooldown", displayName = "Cooldown between uses (seconds)", type = "float", default = 0.5 },
        { name = "promptText", displayName = "Prompt text", type = "string", default = "Press E" },
        { name = "prompt", displayName = "Prompt: Text entity", type = "entity" },
        { name = "promptPanel", displayName = "Prompt: panel or image entity", type = "entity" },
        { name = "highlight", displayName = "Highlight entity", type = "entity" },
        { name = "target", displayName = "Target", type = "entity" },
        { name = "targetMethod", displayName = "Target method", type = "string", default = "onInteract" },
        { name = "damageOnUse", displayName = "Damage per use (0 = none)", type = "float", default = 0.0 },
        { name = "damageHealth", displayName = "Health to damage", type = "Health" }
    }
}

-- Slots arrive as a script table, an engine handle or the bare stored id (0 = NULL_ENTITY): normalise.
local function entityId(reference)
    if type(reference) == "number" then return reference ~= 0 and reference or nil end
    if type(reference) ~= "table" and type(reference) ~= "userdata" then return nil end
    return reference.entity ~= 0 and reference.entity or nil
end
local function slotId(scene, reference, fallbackName)
    local id = entityId(reference)
    if not id and fallbackName and fallbackName ~= "" then id = entityId(scene:findEntity(fallbackName)) end
    return id
end
local function positionOf(scene, id)
    if not id then return nil end
    local ok, position = pcall(function() return Object(scene, id).position end)
    return (ok and position) or nil
end
local function callMethod(reference, methodName, first)
    if type(reference) ~= "table" or not methodName or methodName == "" then return false end
    local method = reference[methodName]
    if type(method) ~= "function" then return false end
    method(reference, first)
    return true
end
local function takesDamage(reference)
    return type(reference) == "table" and type(reference.takeDamage) == "function"
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

function Interactable:init()
    RegisterEngineEvent(self, "onUpdate")
    self.radius = math.max(tonumber(self.radius) or 3.0, 0.05)
    self.cooldown = math.max(tonumber(self.cooldown) or 0.0, 0.0)
    self.lookTolerance = math.max(tonumber(self.lookTolerance) or 12.0, 0.5)
    if not self.key or self.key == 0 then self.key = Input.KEY_E end
    self.handles, self.lastWarn, self.used, self.sinceUse = {}, {}, false, self.cooldown
    self.shown, self.promptForced, self.previousKey = nil, nil, false
    local ok, object = pcall(function() return Object(self.scene, self.entity) end)
    self.object = (ok and object) or nil
end

function Interactable:warn(key, message)
    local last = self.lastWarn[key]
    if not last or Engine.systemTime - last > 1.0 then
        self.lastWarn[key] = Engine.systemTime
        print("[Interactable] " .. message)
    end
end

-- Lua has no overlap queries, so "standing at it" is a plain distance test against the subject.
function Interactable:isInRange()
    if self.used and self.once == true then return false end
    local here = self.object and self.object.position
    local point = positionOf(self.scene, slotId(self.scene, self.subject, self.fallbackName))
    return here ~= nil and point ~= nil and here:distance(point) <= self.radius
end

function Interactable:isLooking()
    if self.requireLooking ~= true then return true end
    local here = self.object and self.object.position
    -- The scene's active camera is the last resort, so an unwired slot still works in a level.
    local id = slotId(self.scene, self.camera, "Camera") or entityId(self.scene.camera)
    local camera = widget(self.scene, self.handles, "camera", id, Camera)
    if not camera or not here then
        self:warn("camera", "wire a Camera entity to use the look check")
        return false
    end
    local mouse = Input.getMousePosition()
    local ok, ray = pcall(function() return camera:screenToRay(mouse.x, mouse.y) end)
    if not ok or not ray then return false end
    local toward = Vector3(here.x - ray.origin.x, here.y - ray.origin.y, here.z - ray.origin.z)
    local dot = math.max(-1.0, math.min(1.0, ray.direction:dotProduct(toward:normalized())))
    return math.deg(math.acos(dot)) <= self.lookTolerance
end

function Interactable:canUse()
    return self.sinceUse >= self.cooldown and self:isInRange() and self:isLooking()
end

function Interactable:trigger()
    if (self.used and self.once == true) or self.sinceUse < self.cooldown then return false end
    self.sinceUse = 0.0
    if self.once == true then self.used = true end
    if not callMethod(self.target, self.targetMethod, self.entity) then
        self:warn("target", "wire a Target entity with a " .. tostring(self.targetMethod) .. " method")
    end
    local damage = tonumber(self.damageOnUse) or 0.0
    if damage > 0.0 then
        local health = takesDamage(self.damageHealth) and self.damageHealth or self.target
        if takesDamage(health) then pcall(function() health:takeDamage(damage, self.entity) end) end
    end
    return true
end

function Interactable:setPrompt(text)
    if type(text) == "string" then self.promptText, self.shown = text, nil end
end

-- nil hands control back to the proximity check; true/false pins the prompt open or shut.
function Interactable:showPrompt(value)
    if value ~= nil then self.promptForced = value == true else self.promptForced = nil end
    self.shown = nil
end

function Interactable:setWidgets(visible)
    local label = widget(self.scene, self.handles, "prompt", self.prompt, Text)
    if label then label.text, label.alpha = self.promptText or "", (visible and 1.0 or 0.0)
    elseif visible and (self.promptText or "") ~= "" then
        self:warn("prompt", "wire a Text entity into the Prompt slot to show the hint")
    end
    for _, key in ipairs({ "promptPanel", "highlight" }) do
        local object = widget(self.scene, self.handles, key, self[key], Object)
        if object then object.visible = visible end
    end
end

function Interactable:onUpdate()
    if not self.object then
        self:warn("object", "this entity needs a Transform component")
        return
    end
    self.sinceUse = self.sinceUse + math.min(Engine.deltatime, 0.05)
    local wanted = self.promptForced
    if wanted == nil then wanted = self:canUse() end
    if wanted ~= self.shown then self.shown = wanted self:setWidgets(wanted) end
    local pressed = Input.isKeyPressed(self.key)
    if pressed and not self.previousKey and self:canUse() then self:trigger() end
    self.previousKey = pressed
end

return Interactable
