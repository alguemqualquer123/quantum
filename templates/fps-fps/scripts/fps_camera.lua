local FpsCamera = {}

function FpsCamera:init()
    self.sensitivity = 0.002
    self.moveSpeed = 5.0
    self.yaw = 0.0
    self.pitch = 0.0
    self.playerBody = nil
    
    self.camera = Camera(self.scene, self.entity)
    self.camera:setPerspective(70.0, 1.778, 0.1, 1000.0)
    self.camera:disableTarget()
    self.camera:activate()

    Engine.mouseMode = MouseMode.LOCKED

    RegisterEngineEvent(self, "onUpdate")
    RegisterEngineEvent(self, "onFixedUpdate")

    Engine.onMouseMove = function(_, x, y)
        self.yaw = self.yaw - x * self.sensitivity
        self.pitch = self.pitch - y * self.sensitivity
        if self.pitch > 1.5 then self.pitch = 1.5 end
        if self.pitch < -1.5 then self.pitch = -1.5 end
    end
end

function FpsCamera:onUpdate()
    if not self.playerBody then
        local ent = Engine.scene:findEntity("Player")
        if not ent then return end
        self.playerBody = Body3D(self.scene, ent)
    end

    local playerPos = self.playerBody.position
    -- self.camera.position = Vector3(playerPos.x, playerPos.y + 1.7, playerPos.z)

    local q = Quaternion(self.pitch, self.yaw, 0.0)
    self.camera.rotation = q
end

function FpsCamera:onFixedUpdate()
    if not self.playerBody then return end

    local forward = self.camera:getDirection()
    local right = self.camera:getRight()
    local moveDir = Vector3(0.0, 0.0, 0.0)

    if Input.isKeyPressed(Input.KEY_W) then
        moveDir = moveDir + Vector3(forward.x, 0.0, forward.z)
    end
    if Input.isKeyPressed(Input.KEY_S) then
        moveDir = moveDir - Vector3(forward.x, 0.0, forward.z)
    end
    if Input.isKeyPressed(Input.KEY_A) then
        moveDir = moveDir - Vector3(right.x, 0.0, right.z)
    end
    if Input.isKeyPressed(Input.KEY_D) then
        moveDir = moveDir + Vector3(right.x, 0.0, right.z)
    end

    if moveDir.x ~= 0.0 or moveDir.z ~= 0.0 then
        local len = math.sqrt(moveDir.x * moveDir.x + moveDir.z * moveDir.z)
        if len > 0.0 then
            moveDir = moveDir / len * self.moveSpeed
        end
        self.playerBody:applyForce(moveDir)
    end
end

return FpsCamera
