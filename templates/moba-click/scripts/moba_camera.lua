local MobaCamera = {}

function MobaCamera:init()
    self.playerObj = nil

    local cam = Camera(self.scene, self.entity)
    cam:setPerspective(45.0, 1.778, 0.1, 100.0)
    cam:disableTarget()
    cam:activate()
end

function MobaCamera:update()
    if not self.playerObj then
        local playerEntity = Engine.scene:findEntity("Player")
        if not playerEntity then return end
        self.playerObj = Object(self.scene, playerEntity)
    end

    local playerPos = self.playerObj.position
    local obj = Object(self.scene, self.entity)
    local currentPos = obj.position

    local targetX = playerPos.x
    local targetZ = playerPos.z + 12.0
    local targetY = 18.0

    local t = math.min(1.0, self.followSpeed * Engine.deltatime)
    local newX = currentPos.x + (targetX - currentPos.x) * t
    local newZ = currentPos.z + (targetZ - currentPos.z) * t

    obj.position = Vector3(newX, targetY, newZ)

    local cam = Camera(self.scene, self.entity)
    cam:setTarget(playerPos.x, 0.0, playerPos.z)
end

return MobaCamera
