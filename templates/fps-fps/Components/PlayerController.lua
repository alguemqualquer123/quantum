-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- One-drop player preset. Attach this script to an entity with a Transform and it creates the
-- dynamic capsule, the built-in BananaMan model and a third-person camera at runtime. Animation
-- names remain properties so the controller can drive another model later.
local PlayerController = {
    properties = {
        { name = "modelPath", displayName = "Model", type = "string", default = "builtin://BananaMan.glb" },
        { name = "moveSpeed", displayName = "Walk speed (m/s)", type = "float", default = 3.5 },
        { name = "runSpeed", displayName = "Run speed (m/s)", type = "float", default = 7.0 },
        { name = "jumpHeight", displayName = "Jump height (m)", type = "float", default = 1.2 },
        { name = "capsuleHeight", displayName = "Capsule height (m)", type = "float", default = 1.8 },
        { name = "capsuleRadius", displayName = "Capsule radius (m)", type = "float", default = 0.38 },
        { name = "cameraDistance", displayName = "Camera distance (m)", type = "float", default = 5.5 },
        { name = "cameraHeight", displayName = "Camera height (m)", type = "float", default = 2.0 },
        { name = "lookHeight", displayName = "Look height (m)", type = "float", default = 1.1 },
        { name = "mouseSensitivity", displayName = "Camera sensitivity", type = "float", default = 0.15 },
        { name = "idleAnimation", displayName = "Idle animation", type = "string", default = "Idle" },
        { name = "walkAnimation", displayName = "Walk animation", type = "string", default = "Walk" },
        { name = "runAnimation", displayName = "Run animation", type = "string", default = "Run" },
        { name = "crouchAnimation", displayName = "Crouch animation", type = "string", default = "Crouch" },
        { name = "jumpAnimation", displayName = "Jump animation", type = "string", default = "Jump" },
        { name = "fallAnimation", displayName = "Fall animation", type = "string", default = "Fall" },
        { name = "crouchKey", displayName = "Crouch key (0 = Ctrl)", type = "int", default = 0 },
        { name = "sprintKey", displayName = "Sprint key (0 = Shift)", type = "int", default = 0 },
        { name = "jumpKey", displayName = "Jump key (0 = Space)", type = "int", default = 0 }
    }
}

local function clamp(value, low, high)
    if value < low then return low end
    if value > high then return high end
    return value
end

local function keyOr(value, fallback) return (not value or value == 0) and fallback or value end

local function warn(self, message)
    if Engine.systemTime - self.lastWarning > 1.0 then
        self.lastWarning = Engine.systemTime
        print("[PlayerController] " .. message)
    end
end

function PlayerController:init()
    RegisterEngineEvent(self, "onFixedUpdate")
    RegisterEngineEvent(self, "onUpdate")
    self.moveSpeed = math.max(self.moveSpeed or 3.5, 0.0)
    self.runSpeed = math.max(self.runSpeed or 7.0, self.moveSpeed)
    self.jumpHeight = math.max(self.jumpHeight or 1.2, 0.05)
    self.capsuleHeight = math.max(self.capsuleHeight or 1.8, 0.5)
    self.capsuleRadius = math.max(self.capsuleRadius or 0.38, 0.05)
    self.cameraDistance = math.max(self.cameraDistance or 5.5, 0.5)
    self.cameraHeight, self.lookHeight = self.cameraHeight or 2.0, self.lookHeight or 1.1
    self.mouseSensitivity = self.mouseSensitivity or 0.15
    self.crouchKey = keyOr(self.crouchKey, Input.KEY_LEFT_CONTROL)
    self.sprintKey = keyOr(self.sprintKey, Input.KEY_LEFT_SHIFT)
    self.jumpKey = keyOr(self.jumpKey, Input.KEY_SPACE)
    self.lastWarning, self.currentAnimation = 0.0, ""
    self.yaw, self.pitch, self.previousMouse = 0.0, 12.0, nil
    self.grounded, self.jumpWasDown, self.velocityY = true, false, 0.0
    self:ensureBody()
    self:ensureModel()
    self:ensureCamera()
end

function PlayerController:ensureBody()
    local ok, body = pcall(function() return Body3D(self.scene, self.entity) end)
    if not ok or not body then
        local created = pcall(function() self.scene:getPhysicsSystem():createBody3D(self.entity) end)
        if not created then warn(self, "Could not create the player Body3D."); return end
        body = Body3D(self.scene, self.entity)
    end
    self.body = body
    pcall(function()
        body.type = BodyType.DYNAMIC
        body:createCapsuleShape(Vector3(0.0, self.capsuleHeight * 0.5, 0.0), Quaternion.IDENTITY,
            math.max(self.capsuleHeight * 0.5 - self.capsuleRadius, 0.05), self.capsuleRadius)
        body:setAllowedDOFs(true, true, true, false, true, false)
        self.scene:getPhysicsSystem():loadBody3D(self.entity)
    end)
end

function PlayerController:ensureModel()
    local child
    pcall(function() child = self.scene:findEntityInParent("BananaMan", self.entity) end)
    if child and child ~= NULL_ENTITY then
        self.modelEntity, self.model = child, Model(self.scene, child)
        return
    end
    local ok = pcall(function() Model(self.scene, self.entity):findAnimation(self.idleAnimation) end)
    if ok then self.modelEntity, self.model = self.entity, Model(self.scene, self.entity); return end
    local loaded, model = pcall(function()
        local created = Model(self.scene)
        created.name = "BananaMan"
        created:loadModel(self.modelPath or "builtin://BananaMan.glb")
        self.scene:addEntityChild(self.entity, created.entity, true)
        return created
    end)
    if not loaded then warn(self, "Could not create the BananaMan model."); return end
    self.modelEntity, self.model = model.entity, model
end

function PlayerController:ensureCamera()
    local ok, camera = pcall(function()
        local created = Camera(self.scene)
        created.name = "PlayerCamera"
        local width, height = Engine.getScreenWidth(), Engine.getScreenHeight()
        created:setPerspective(60.0, (height > 0 and width / height or 1.7778), 0.1, 300.0)
        created.autoResize = true
        self.scene:setCamera(created)
        return created
    end)
    if ok then self.camera = camera end
end

function PlayerController:input()
    local x, z = 0.0, 0.0
    if Input.isKeyPressed(Input.KEY_W) then z = z - 1.0 end
    if Input.isKeyPressed(Input.KEY_S) then z = z + 1.0 end
    if Input.isKeyPressed(Input.KEY_A) then x = x - 1.0 end
    if Input.isKeyPressed(Input.KEY_D) then x = x + 1.0 end
    local length = math.sqrt(x * x + z * z)
    if length > 1.0 then x, z = x / length, z / length end
    return x, z, length
end

function PlayerController:onFixedUpdate()
    if not self.body then return end
    local dt = math.min(Engine.deltatime, 1.0 / 30.0)
    local x, z, inputLength = self:input()
    local running = Input.isKeyPressed(self.sprintKey) and inputLength > 0.0
    local speed = running and self.runSpeed or self.moveSpeed
    local yaw = math.rad(self.yaw)
    local forwardX, forwardZ = math.sin(yaw), -math.cos(yaw)
    local rightX, rightZ = math.cos(yaw), math.sin(yaw)
    local velocity = self.body.linearVelocity
    velocity.x, velocity.z = (rightX * x + forwardX * (-z)) * speed, (rightZ * x + forwardZ * (-z)) * speed
    local jumpDown = Input.isKeyPressed(self.jumpKey)
    if jumpDown and not self.jumpWasDown and self.grounded then
        self.velocityY = math.sqrt(2.0 * math.abs(self.scene.gravity3D.y) * self.jumpHeight)
        self.grounded = false
    end
    self.jumpWasDown = jumpDown
    if not self.grounded then self.velocityY = self.velocityY + self.scene.gravity3D.y * dt end
    if self.grounded then self.velocityY = 0.0 end
    velocity.y = self.velocityY
    self.body.linearVelocity = velocity

    local position = self.body.position
    local hit = pcall(function()
        local result = Ray(Vector3(position.x, position.y + 0.2, position.z), Vector3(0.0, -1.0, 0.0))
            :intersects(self.scene, RayFilter.BODY_3D, self.entity)
        if result and result.hit and self.velocityY <= 0.0 then self.grounded = true end
    end)
    if not hit then self.grounded = math.abs(velocity.y) < 0.05 end

    local state = "Idle"
    if not self.grounded then state = self.velocityY > 0.0 and "Jump" or "Fall"
    elseif Input.isKeyPressed(self.crouchKey) then state = "Crouch"
    elseif inputLength > 0.0 then state = running and "Run" or "Walk" end
    self:setAnimation(state)
end

function PlayerController:setAnimation(state)
    if not self.model or self.currentAnimation == state then return end
    local names = {Idle = self.idleAnimation, Walk = self.walkAnimation, Run = self.runAnimation,
        Crouch = self.crouchAnimation, Jump = self.jumpAnimation, Fall = self.fallAnimation}
    local name = names[state]
    if name and name ~= "" and pcall(function() self.model:playAnimation(name, 0.15) end) then
        self.currentAnimation = state
    end
end

function PlayerController:onUpdate()
    if not self.camera or not self.body then return end
    local mouse = Input.getMousePosition()
    if self.previousMouse then
        self.yaw = self.yaw - (mouse.x - self.previousMouse.x) * self.mouseSensitivity
        self.pitch = clamp(self.pitch + (mouse.y - self.previousMouse.y) * self.mouseSensitivity, -15.0, 65.0)
    end
    self.previousMouse = {x = mouse.x, y = mouse.y}
    local position = self.body.position
    local yaw, pitch = math.rad(self.yaw), math.rad(self.pitch)
    local focus = Vector3(position.x, position.y + self.lookHeight, position.z)
    local horizontal = self.cameraDistance * math.cos(pitch)
    self.camera.position = Vector3(focus.x - math.sin(yaw) * horizontal,
        focus.y + self.cameraHeight + self.cameraDistance * math.sin(pitch),
        focus.z + math.cos(yaw) * horizontal)
    self.camera.target = focus
end

return PlayerController
