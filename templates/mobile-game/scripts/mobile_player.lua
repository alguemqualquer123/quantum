local MobilePlayer = {}

function MobilePlayer:init()
end

function MobilePlayer:update()
    local moveX = 0.0
    local moveY = 0.0

    if Input.isKeyPressed(Input.KEY_W) or Input.isKeyPressed(Input.KEY_UP) then
        moveY = moveY + 1.0
    end
    if Input.isKeyPressed(Input.KEY_S) or Input.isKeyPressed(Input.KEY_DOWN) then
        moveY = moveY - 1.0
    end
    if Input.isKeyPressed(Input.KEY_A) or Input.isKeyPressed(Input.KEY_LEFT) then
        moveX = moveX - 1.0
    end
    if Input.isKeyPressed(Input.KEY_D) or Input.isKeyPressed(Input.KEY_RIGHT) then
        moveX = moveX + 1.0
    end

    if moveX ~= 0.0 or moveY ~= 0.0 then
        local len = math.sqrt(moveX * moveX + moveY * moveY)
        if len > 1.0 then
            moveX = moveX / len
            moveY = moveY / len
        end
        local body = Body2D(self.scene, self.entity)
        body.linearVelocity = Vector2(moveX * self.moveSpeed, moveY * self.moveSpeed)
    else
        local body = Body2D(self.scene, self.entity)
        body.linearVelocity = Vector2(0.0, 0.0)
    end
end

return MobilePlayer
