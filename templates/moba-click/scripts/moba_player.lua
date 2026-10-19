local MobaPlayer = {}

function MobaPlayer:init()
    self.targetPos = nil
    self.targetRotation = 0.0
    self.isMoving = false

    Engine.onMouseDown = function(s, button, x, y)
        if button == Input.MOUSE_BUTTON_RIGHT then
            local cam = Camera(Engine.scene, Engine.scene.camera)
            local ray = cam:screenToRay(x, y)
            local result = ray:intersects(Engine.scene, RayFilter.BODY_3D)
            if result then
                self.targetPos = Vector3(result.x, 0.0, result.z)
                self.isMoving = true
            else
                local groundY = -ray.origin.y / ray.direction.y
                local hitX = ray.origin.x + ray.direction.x * groundY
                local hitZ = ray.origin.z + ray.direction.z * groundY
                self.targetPos = Vector3(hitX, 0.0, hitZ)
                self.isMoving = true
            end
        end
    end
end

function MobaPlayer:update()
    if not self.isMoving or not self.targetPos then return end

    local dt = Engine.deltatime
    local obj = Object(self.scene, self.entity)
    local body = Body3D(self.scene, self.entity)
    local pos = obj.position

    local dirX = self.targetPos.x - pos.x
    local dirZ = self.targetPos.z - pos.z
    local dist = math.sqrt(dirX * dirX + dirZ * dirZ)

    if dist < 0.2 then
        self.isMoving = false
        return
    end

    local nx = dirX / dist
    local nz = dirZ / dist

    self.targetRotation = math.atan(nx, nz)

    local step = math.min(self.moveSpeed * dt, dist)
    local newX = pos.x + nx * step
    local newZ = pos.z + nz * step

    obj.position = Vector3(newX, pos.y, newZ)
    body.position = Vector3(newX, pos.y, newZ)

    local currentRot = obj.rotation
    local targetQ = Quaternion(0.0, self.targetRotation, 0.0)
    local t = math.min(1.0, self.rotationSpeed * dt)
    obj.rotation = currentRot:lerp(targetQ, t)
end

return MobaPlayer
