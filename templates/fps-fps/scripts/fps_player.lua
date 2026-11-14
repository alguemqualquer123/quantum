local FpsPlayer = {}

function FpsPlayer:init()
    self.velocityY = 0.0
    self.isGrounded = false
    self.gravity = -20.0
end

function FpsPlayer:update(dt)
    local body = Body3D(self.scene, self.entity)
    -- local dt = Engine.deltatime

    if Input.isKeyPressed(Input.KEY_SPACE) and self.isGrounded then
        self.velocityY = self.jumpForce
        self.isGrounded = false
    end

    self.velocityY = self.velocityY + self.gravity * dt

    local pos = body.position
    local newY = pos.y + self.velocityY * dt

    if newY <= 0.0 then
        newY = 0.0
        self.velocityY = 0.0
        self.isGrounded = true
    end

    body.position = Vector3(pos.x, newY, pos.z)
end

return FpsPlayer
