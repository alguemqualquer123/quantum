local ThirdPersonCamera = {}

function ThirdPersonCamera:init()
    self.currentAngle = 0.0
    self.playerObj = nil

    local cam = Camera(self.scene, self.entity)
    cam:setPerspective(60.0, 1.778, 0.1, 1000.0)
    cam:disableTarget()
    cam:activate()
end

function ThirdPersonCamera:update()
    if not self.playerObj then
        local playerEntity = Engine.scene:findEntity("Player")
        if not playerEntity then return end
        self.playerObj = Object(self.scene, playerEntity)
    end

    local dt = Engine.deltatime
    local playerPos = self.playerObj.position

    if Input.isKeyPressed(Input.KEY_LEFT) then
        self.currentAngle = self.currentAngle + 2.0 * dt
    end
    if Input.isKeyPressed(Input.KEY_RIGHT) then
        self.currentAngle = self.currentAngle - 2.0 * dt
    end

    local camX = playerPos.x + math.sin(self.currentAngle) * self.distance
    local camY = playerPos.y + self.height
    local camZ = playerPos.z + math.cos(self.currentAngle) * self.distance

    local obj = Object(self.scene, self.entity)
    obj.position = Vector3(camX, camY, camZ)

    local cam = Camera(self.scene, self.entity)
    cam:setTarget(playerPos.x, playerPos.y + 1.0, playerPos.z)
end

return ThirdPersonCamera
