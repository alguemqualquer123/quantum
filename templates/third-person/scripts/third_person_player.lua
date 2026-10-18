local ThirdPersonPlayer = {}

function ThirdPersonPlayer:init()
    self.targetRotation = 0.0
end

function ThirdPersonPlayer:update()
    local dt = Engine.deltatime
    local obj = Object(self.scene, self.entity)
    local body = Body3D(self.scene, self.entity)
    local pos = obj.position

    local moveX = 0.0
    local moveZ = 0.0

    if Input.isKeyPressed(Input.KEY_W) then moveZ = moveZ - 1.0 end
    if Input.isKeyPressed(Input.KEY_S) then moveZ = moveZ + 1.0 end
    if Input.isKeyPressed(Input.KEY_A) then moveX = moveX - 1.0 end
    if Input.isKeyPressed(Input.KEY_D) then moveX = moveX + 1.0 end

    if moveX ~= 0.0 or moveZ ~= 0.0 then
        local len = math.sqrt(moveX * moveX + moveZ * moveZ)
        moveX = moveX / len
        moveZ = moveZ / len

        self.targetRotation = math.atan(moveX, moveZ)

        local newX = pos.x + moveX * self.moveSpeed * dt
        local newZ = pos.z + moveZ * self.moveSpeed * dt
        obj.position = Vector3(newX, pos.y, newZ)
        body.position = Vector3(newX, pos.y, newZ)
    end

    local currentRot = obj.rotation
    local targetQ = Quaternion(0.0, self.targetRotation, 0.0)
    local t = math.min(1.0, self.rotationSpeed * dt)
    local newRot = currentRot:lerp(targetQ, t)
    obj.rotation = newRot
end

return ThirdPersonPlayer
