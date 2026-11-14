-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Side scroller controller for a Body2D: run, jump with coyote time and a variable height, and
-- category: Camera & Controllers
-- optionally flip the sprite with the facing. Needs a dynamic body with shapes on the same
-- entity. Distances are world units, not metres: with the default pointsToMeterScale2D of 64 a
-- jumpHeight of 120 is a little under two metres.
local PlatformerController2D = {
    properties = {
        { name = "moveSpeed", displayName = "Speed (units/s)", type = "float", default = 320.0 },
        { name = "acceleration", displayName = "Acceleration (1/s)", type = "float", default = 18.0 },
        { name = "jumpHeight", displayName = "Jump height (units)", type = "float", default = 120.0 },
        { name = "coyoteTime", displayName = "Coyote time (seconds)", type = "float", default = 0.1 },
        { name = "jumpCut", displayName = "Short hop when the key is released", type = "bool", default = true },
        { name = "groundSpeed", displayName = "Grounded vertical speed (units/s)", type = "float", default = 12.0 },
        { name = "flipSprite", displayName = "Flip the sprite with the facing", type = "bool", default = true },
        { name = "keyLeft", displayName = "Left key (0 = A)", type = "int", default = 0 },
        { name = "keyRight", displayName = "Right key (0 = D)", type = "int", default = 0 },
        { name = "keyJump", displayName = "Jump key (0 = space)", type = "int", default = 0 }
    }
}

local function clamp(value, low, high)
    if value < low then return low end
    if value > high then return high end
    return value
end

local function defaultKey(value, fallback)
    if not value or value == 0 then
        return fallback
    end
    return value
end

function PlatformerController2D:init()
    RegisterEngineEvent(self, "onFixedUpdate")

    self.moveSpeed = self.moveSpeed or 320.0
    self.acceleration = self.acceleration or 18.0
    self.jumpHeight = self.jumpHeight or 120.0
    self.coyoteTime = self.coyoteTime or 0.1
    self.groundSpeed = self.groundSpeed or 12.0

    self.keyLeft = defaultKey(self.keyLeft, Input.KEY_A)
    self.keyRight = defaultKey(self.keyRight, Input.KEY_D)
    self.keyJump = defaultKey(self.keyJump, Input.KEY_SPACE)

    self.facing = 1.0
    self.airborneFor = 0.0
    self.previousJump = false
    self.jumpHeldFor = 0.0
    self.warningShown = false

    local ok, body = pcall(function()
        return Body2D(self.scene, self.entity)
    end)
    self.body = (ok and body) or nil

    local okObject, attached = self.body and pcall(function()
        return self.body:getAttachedObject()
    end)
    self.object = (okObject and attached) or nil
    self.baseScaleX = self.object and math.abs(self.object.scale.x) or nil
end

function PlatformerController2D:onFixedUpdate()
    if not self.body then
        if not self.warningShown then
            self.warningShown = true
            print("[PlatformerController2D] The entity needs a Body2D component.")
        end
        return
    end

    local dt = 1.0 / 60.0
    local velocity = self.body.linearVelocity
    local grounded = math.abs(velocity.y) < self.groundSpeed

    if grounded then
        self.airborneFor = 0.0
    else
        self.airborneFor = self.airborneFor + dt
    end

    local axis = 0.0
    if Input.isKeyPressed(self.keyLeft) then axis = axis - 1.0 end
    if Input.isKeyPressed(self.keyRight) then axis = axis + 1.0 end

    local rate = clamp(self.acceleration * dt, 0.0, 1.0)
    self.body.linearVelocity = Vector2(
        velocity.x + (axis * self.moveSpeed - velocity.x) * rate,
        velocity.y
    )

    if axis ~= 0.0 then
        self.facing = axis > 0.0 and 1.0 or -1.0
    end

    if self.flipSprite ~= false and self.object and self.baseScaleX then
        local scale = self.object.scale
        scale.x = self.baseScaleX * self.facing
        self.object.scale = scale
    end

    local jumpPressed = Input.isKeyPressed(self.keyJump)
    local canJump = grounded or self.airborneFor <= self.coyoteTime

    if jumpPressed and not self.previousJump and canJump then
        local scale = self.body.pointsToMeterScale or 64.0
        local gravity = math.max(math.abs(self.scene.gravity2D.y) * scale, 1.0)
        self.body.linearVelocity = Vector2(self.body.linearVelocity.x,
            math.sqrt(2.0 * gravity * math.max(self.jumpHeight, 1.0)))
        self.jumpHeldFor = 0.0
        self.airborneFor = self.coyoteTime + 1.0
    elseif jumpPressed then
        self.jumpHeldFor = self.jumpHeldFor + dt
    elseif self.previousJump and self.jumpHeldFor < 0.25 and self.jumpCut ~= false then
        -- Releasing early clips the rise, which is what makes a tap and a hold feel different.
        local velocity = self.body.linearVelocity
        if velocity.y > 0.0 then
            self.body.linearVelocity = Vector2(velocity.x, velocity.y * 0.35)
        end
        self.jumpHeldFor = 0.25
    end

    self.previousJump = jumpPressed
end

return PlatformerController2D
