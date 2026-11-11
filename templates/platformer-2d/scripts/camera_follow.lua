local CameraFollow = {}

function CameraFollow:init()
    self.playerObj = nil
end

function CameraFollow:update()
    if not self.playerObj then
        local playerEntity = Engine.scene:findEntity("Player")
        if not playerEntity then return end
        self.playerObj = Object(self.scene, playerEntity)
    end

    local playerPos = self.playerObj.position
    local obj = Object(self.scene, self.entity)
    local currentPos = obj.position

    local t = math.min(1.0, self.followSpeed * Engine.deltatime)
    local newX = currentPos.x + (playerPos.x - currentPos.x) * t

    obj.position = Vector3(newX, currentPos.y, currentPos.z)
end

return CameraFollow
