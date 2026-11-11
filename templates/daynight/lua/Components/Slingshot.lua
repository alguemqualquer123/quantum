-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Pull a body back on an invisible elastic band and let it go: the draw is a real spring force and
-- category: Physics
-- the shot is one impulse on a Jolt body. Attach it to the launcher and fill the Projectile slot.
local Slingshot = {
    properties = {
        { name = "subject", displayName = "Projectile (0 = none)", type = "entity" },
        { name = "fallbackSubjectName", displayName = "Projectile name if slot is empty", type = "string", default = "Projectile" },
        { name = "anchor", displayName = "Anchor (0 = this entity)", type = "entity" },
        { name = "maxStretch", displayName = "Maximum draw (m)", type = "float", default = 3.0 },
        { name = "power", displayName = "Shot power (m/s at full draw)", type = "float", default = 12.0 },
        { name = "pullKey", displayName = "Draw key (0 = R)", type = "int", default = 0 },
        { name = "holdKey", displayName = "Second draw key (0 = space)", type = "int", default = 0 },
        { name = "useMouse", displayName = "Draw and aim with the mouse", type = "bool", default = false },
        { name = "camera", displayName = "Aiming camera", type = "entity" },
        { name = "fallbackCameraName", displayName = "Camera name if slot is empty", type = "string", default = "Camera" },
        { name = "autoReturn", displayName = "Bring the bucket back", type = "bool", default = true },
        { name = "resetDelay", displayName = "Return delay (seconds)", type = "float", default = 1.5 },
        { name = "releaseImpulseUpward", displayName = "Extra upward speed (m/s)", type = "float", default = 0.0 },
        { name = "shotSound", displayName = "Sound to play on release", type = "entity" },
        { name = "band", displayName = "Rope component entity (optional)", type = "entity" }
    }
}

local function clamp(value, low, high) return math.min(math.max(value, low), high) end
local function num(value, fallback) return type(value) == "number" and value or fallback end

local function entityId(reference)
    local id = (type(reference) == "number") and reference
        or ((reference and type(reference.entity) == "number") and reference.entity or 0)
    return id ~= 0 and id or nil
end

local function resolve(scene, reference, fallbackName)
    local id = entityId(reference)
    if id or not fallbackName or fallbackName == "" then return id end
    return entityId(scene:findEntity(fallbackName))
end

function Slingshot:init()
    RegisterEngineEvent(self, "onFixedUpdate")
    self.maxStretch = math.max(0.05, num(self.maxStretch, 3.0))
    self.power = math.max(0.0, num(self.power, 12.0))
    self.resetDelay = math.max(0.0, num(self.resetDelay, 1.5))
    self.pullKey = num(self.pullKey, 0) ~= 0 and self.pullKey or Input.KEY_R
    self.holdKey = num(self.holdKey, 0) ~= 0 and self.holdKey or Input.KEY_SPACE
    self.stretch, self.shotAt, self.drawn, self.lastWarn = 0.0, -1.0, false, -1.0
    self.aim = Vector3(0.0, 0.0, 1.0)

    local ok, object = pcall(function()
        return Object(self.scene, resolve(self.scene, self.anchor, nil) or self.entity)
    end)
    self.object = (ok and object) or nil
    self:load(self.subject)
end

function Slingshot:warn(message)
    local now = num(Engine.systemTime, 0.0)
    if now - num(self.lastWarn, -1.0) < 1.0 then return end
    self.lastWarn = now
    print("[Slingshot] " .. message)
end

function Slingshot:load(reference)
    self.projectileId = resolve(self.scene, reference, self.fallbackSubjectName)
    local ok, body = pcall(function() return Body3D(self.scene, self.projectileId) end)
    self.body = (ok and body) or nil
    if not self.body then
        self:warn("no Body3D on the projectile: fill the Projectile slot or name it")
    end
    return self.body ~= nil
end

function Slingshot:anchorPoint()
    if not self.object then return Vector3(0.0, 0.0, 0.0) end
    return self.object.position
end

-- With the mouse the shot leaves along the camera ray; otherwise along the launcher's own forward
-- axis, so a placed slingshot works with nothing wired.
function Slingshot:aimDirection()
    if self.useMouse then
        local id = resolve(self.scene, self.camera, self.fallbackCameraName) or entityId(self.scene.camera)
        local mouse = Input.getMousePosition()
        -- screenToRay is bound as (float x, float y); the Vector2 form the header shows is not a Lua overload.
        local ok, ray = pcall(function()
            return Camera(self.scene, id):screenToRay(mouse.x, mouse.y)
        end)
        if ok and ray then return ray.direction:normalize() end
    end
    if self.object then
        local ok, axis = pcall(function() return self.object.rotation:zAxis() end)
        if ok and axis then return axis:normalize() end
    end
    return self.aim
end

function Slingshot:pull(direction, amount)
    if direction then
        self.aim = Vector3(num(direction.x, 0.0), num(direction.y, 0.0), num(direction.z, 0.0)):normalize()
    end
    self.stretch = clamp(num(amount, self.stretch), 0.0, 1.0)
    self.drawn = true
end

function Slingshot:release()
    if not self.body or self.stretch <= 0.0 then
        self.drawn, self.stretch = false, 0.0
        return false
    end
    local mass = math.max(num(self.body.mass, 0.0), 0.01)
    local impulse = self.aim * (self.power * self.stretch * mass)
        + Vector3(0.0, num(self.releaseImpulseUpward, 0.0) * mass, 0.0)
    pcall(function() self.body:applyImpulse(impulse) end)

    self.shotAt = num(Engine.systemTime, 0.0)
    self.drawn, self.stretch = false, 0.0
    local soundId = entityId(self.shotSound)
    if soundId then pcall(function() Sound(self.scene, soundId):play() end) end
    return true
end

function Slingshot:reset()
    self.stretch, self.drawn, self.shotAt = 0.0, false, -1.0
    if not self.body then return end
    local point, id = self:anchorPoint(), self.projectileId
    pcall(function()
        if id then Object(self.scene, id).position = point end
        self.body.linearVelocity = Vector3(0.0, 0.0, 0.0)
        self.body.angularVelocity = Vector3(0.0, 0.0, 0.0)
    end)
end

function Slingshot:getStretch()
    return self.stretch
end

function Slingshot:onFixedUpdate()
    if not self.body then return end
    local dt = clamp(num(Engine.deltatime, 1.0 / 60.0), 0.0005, 0.05)
    local holding = Input.isKeyPressed(self.pullKey) or Input.isKeyPressed(self.holdKey)
    if self.useMouse then holding = holding or Input.isMousePressed(Input.MOUSE_BUTTON_LEFT) end

    if holding then
        self.aim = self:aimDirection()
        self.stretch = clamp(self.stretch + dt, 0.0, 1.0)
        self.drawn = true
        -- The bucket is spring-held at the draw point, so it still collides on the way back.
        local pullPoint = self:anchorPoint() - self.aim * (self.stretch * self.maxStretch)
        local mass = math.max(num(self.body.mass, 0.0), 0.01)
        local ok, com = pcall(function() return self.body:getCenterOfMassPosition() end)
        if ok and com then
            pcall(function() self.body:applyForce((pullPoint - com) * (40.0 * mass)) end)
        end
        local band = self.band
        if type(band) == "table" and type(band.setLength) == "function" then
            pcall(function() band:setLength(self.stretch * self.maxStretch) end)
        end
    elseif self.drawn then
        self:release()
    end

    -- A shot is spent: after the delay the bucket comes home, ready to be drawn again.
    if self.autoReturn ~= false and self.shotAt >= 0.0
        and num(Engine.systemTime, 0.0) - self.shotAt >= self.resetDelay then
        self:reset()
    end
end

return Slingshot
