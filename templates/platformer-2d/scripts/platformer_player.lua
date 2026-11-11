local PlatformerPlayer = {}

function PlatformerPlayer:init()
    self.isGrounded = false
    self.canJump = true

    Engine.onKeyDown = function(s, key)
        if key == Input.KEY_SPACE and self.isGrounded and self.canJump then
            local body = Body2D(self.scene, self.entity)
            body:applyLinearImpulseToCenter(Vector2(0.0, self.jumpForce))
            self.isGrounded = false
            self.canJump = false
        end
    end

    Engine.onKeyUp = function(s, key)
        if key == Input.KEY_SPACE then
            self.canJump = true
        end
    end

    local physicsSys = self.scene:getPhysicsSystem()
    physicsSys.beginContact2D = function(bodyA, shapeIdxA, bodyB, shapeIdxB)
        local playerBody = Body2D(self.scene, self.entity)
        if bodyA == playerBody or bodyB == playerBody then
            self.isGrounded = true
        end
    end

    physicsSys.endContact2D = function(bodyA, shapeIdxA, bodyB, shapeIdxB)
        local playerBody = Body2D(self.scene, self.entity)
        if bodyA == playerBody or bodyB == playerBody then
            self.isGrounded = false
        end
    end
end

function PlatformerPlayer:update()
    local body = Body2D(self.scene, self.entity)
    local obj = Object(self.scene, self.entity)
    local pos = obj.position

    local moveX = 0.0

    if Input.isKeyPressed(Input.KEY_A) or Input.isKeyPressed(Input.KEY_LEFT) then
        moveX = moveX - 1.0
    end
    if Input.isKeyPressed(Input.KEY_D) or Input.isKeyPressed(Input.KEY_RIGHT) then
        moveX = moveX + 1.0
    end

    local vel = body.linearVelocity
    body.linearVelocity = Vector2(moveX * self.moveSpeed, vel.y)
end

return PlatformerPlayer
