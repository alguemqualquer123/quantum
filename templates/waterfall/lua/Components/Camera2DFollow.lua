-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- 2D follow camera: keeps an orthographic camera centred on the player, with wheel zoom, a deadzone
-- category: Camera & Controllers
-- and world bounds. Attach it to the entity that holds the Camera component and drag the character
-- into the Subject slot.
local Camera2DFollow = {
    properties = {
        { name = "subject", displayName = "Subject", type = "entity" },
        { name = "fallbackName", displayName = "Subject name if slot is empty", type = "string", default = "Player" },
        { name = "zoom", displayName = "View height (units)", type = "float", default = 8.0 },
        { name = "minZoom", displayName = "Minimum view height (units)", type = "float", default = 2.0 },
        { name = "maxZoom", displayName = "Maximum view height (units)", type = "float", default = 2000.0 },
        { name = "zoomSpeed", displayName = "Zoom per wheel notch (relative, 0 = off)", type = "float", default = 0.25 },
        { name = "smoothingX", displayName = "Horizontal smoothing (1/s)", type = "float", default = 8.0 },
        { name = "smoothingY", displayName = "Vertical smoothing (1/s)", type = "float", default = 8.0 },
        { name = "deadzoneX", displayName = "Horizontal deadzone (units)", type = "float", default = 0.6 },
        { name = "deadzoneY", displayName = "Vertical deadzone (units)", type = "float", default = 0.4 },
        { name = "lockX", displayName = "Do not follow horizontally", type = "bool", default = false },
        { name = "lockY", displayName = "Do not follow vertically", type = "bool", default = false },
        { name = "lookAhead", displayName = "Look ahead (seconds of travel)", type = "float", default = 0.2 },
        { name = "lookAheadLimit", displayName = "Look ahead limit (units)", type = "float", default = 2.0 },
        { name = "minX", displayName = "Left bound (0 = unbounded)", type = "float", default = 0.0 },
        { name = "maxX", displayName = "Right bound (0 = unbounded)", type = "float", default = 0.0 },
        { name = "minY", displayName = "Bottom bound (0 = unbounded)", type = "float", default = 0.0 },
        { name = "maxY", displayName = "Top bound (0 = unbounded)", type = "float", default = 0.0 }
    }
}

local function clamp(value, low, high) return math.min(math.max(value, low), high) end
local function number(value, fallback) return tonumber(value) or fallback end

-- A slot reaches Lua as a script table, an engine handle, or the bare id the inspector stored,
-- and 0 is NULL_ENTITY: normalise all three shapes to one.
local function entityId(reference)
    if type(reference) == "number" then return reference ~= 0 and reference or nil end
    if type(reference) ~= "table" and type(reference) ~= "userdata" then return nil end
    return reference.entity ~= 0 and reference.entity or nil
end

function Camera2DFollow:init()
    RegisterEngineEvent(self, "onUpdate")
    self.minZoom = math.max(number(self.minZoom, 2.0), 0.05)
    self.maxZoom = math.max(number(self.maxZoom, 2000.0), self.minZoom)
    self.zoom = clamp(number(self.zoom, 8.0), self.minZoom, self.maxZoom)
    self.smoothingX = math.max(number(self.smoothingX, 8.0), 0.0)
    self.smoothingY = math.max(number(self.smoothingY, 8.0), 0.0)
    self.deadzoneX = math.max(number(self.deadzoneX, 0.6), 0.0)
    self.deadzoneY = math.max(number(self.deadzoneY, 0.4), 0.0)
    self.lookAheadLimit = math.max(number(self.lookAheadLimit, 2.0), 0.0)
    self.lastWarn, self.snapped = 0.0, false
end

-- 0 means "no bound on this side", so a level that legitimately ends at 0 clamps the other axis.
function Camera2DFollow:clampToBounds(value, low, high)
    if number(low, 0.0) ~= 0.0 then value = math.max(value, low) end
    if number(high, 0.0) ~= 0.0 then value = math.min(value, high) end
    return value
end

function Camera2DFollow:setZoom(value)
    local wanted = number(value, self.zoom)
    if wanted > 0.0 then self.zoom = clamp(wanted, self.minZoom, self.maxZoom) end
    return self.zoom
end

function Camera2DFollow:setBounds(minX, maxX, minY, maxY)
    self.minX, self.maxX, self.minY, self.maxY = number(minX, 0.0), number(maxX, 0.0),
        number(minY, 0.0), number(maxY, 0.0)
end

function Camera2DFollow:snapToSubject() self.snapped = false end

-- Ortho in world units: zoom is the full view height, the width follows the canvas aspect, and the
-- depth window matches the engine's 2D default (-10..10) so sprites at z = 0 stay inside it.
function Camera2DFollow:applyOrtho(camera)
    local width = number(Engine.canvasWidth, 0.0)
    local height = number(Engine.canvasHeight, 0.0)
    local aspect = (height > 0.0 and width > 0.0) and (width / height) or 1.0
    local halfHeight = math.max(self.zoom, 0.01) * 0.5
    pcall(function()
        camera:setOrtho(-halfHeight * aspect, halfHeight * aspect, -halfHeight, halfHeight, -10.0, 10.0)
    end)
end

function Camera2DFollow:onUpdate()
    local ok, camera = pcall(function() return Camera(self.scene, self.entity) end)
    if not ok or not camera then return end

    -- The name is the fallback for an empty slot, so the component works out of the box.
    local id = entityId(self.subject) or entityId(self.scene:findEntity(self.fallbackName or ""))
    local okPoint, point = pcall(function() return Object(self.scene, id).position end)
    if not id or not okPoint or not point then
        if Engine.systemTime - self.lastWarn > 1.0 then
            self.lastWarn = Engine.systemTime
            print("[Camera2DFollow] wire this: drag the player into the Subject slot")
        end
        return
    end

    local dt = math.min(Engine.deltatime, 0.05)
    -- Lua cannot read a 2D body velocity from here, so travel is estimated from the transform,
    -- which works for a scripted, a physics and a static subject alike.
    local velocityX, velocityY = 0.0, 0.0
    if self.lastPoint and dt > 0.0001 then
        velocityX = (point.x - self.lastPoint.x) / dt
        velocityY = (point.y - self.lastPoint.y) / dt
    end
    self.lastPoint = { x = point.x, y = point.y, z = point.z }

    local current = camera.position
    local peek = math.max(number(self.lookAhead, 0.0), 0.0)
    local wantedX, wantedY = current.x, current.y

    if self.lockX ~= true then
        wantedX = point.x + clamp(velocityX * peek, -self.lookAheadLimit, self.lookAheadLimit)
        if math.abs(wantedX - current.x) < self.deadzoneX then wantedX = current.x end
    end
    if self.lockY ~= true then
        wantedY = point.y + clamp(velocityY * peek, -self.lookAheadLimit, self.lookAheadLimit)
        if math.abs(wantedY - current.y) < self.deadzoneY then wantedY = current.y end
    end

    wantedX = self:clampToBounds(wantedX, self.minX, self.maxX)
    wantedY = self:clampToBounds(wantedY, self.minY, self.maxY)

    local alphaX, alphaY = 1.0, 1.0
    if not self.snapped then
        alphaX = 1.0 - math.exp(-self.smoothingX * dt)
        alphaY = 1.0 - math.exp(-self.smoothingY * dt)
    end
    self.snapped = true

    -- The depth axis belongs to the scene setup: this component only moves X and Y.
    camera.position = Vector3(current.x + (wantedX - current.x) * alphaX,
        current.y + (wantedY - current.y) * alphaY, current.z)
    if not self.targetDisabled then
        -- A camera target would re-aim the view every frame and fight these pose writes.
        pcall(function() camera:disableTarget() end)
        self.targetDisabled = true
    end

    local speed = number(self.zoomSpeed, 0.25)
    if speed > 0.0 then
        local okScroll, scroll = pcall(function() return Input.getMouseScroll() end)
        if okScroll and scroll and type(scroll.y) == "number" and scroll.y ~= 0.0 then
            self:setZoom(self.zoom * math.exp(-scroll.y * speed))
        end
    end
    self:applyOrtho(camera)
end

return Camera2DFollow
