-- =============================================================================
-- PART 2 - HOW TO WRITE A quantum LUA SCRIPT
-- =============================================================================
-- This chapter is hand written in tools/reflector/lua_usage.lua and appended
-- verbatim to Types.lua by `quantum-reflector types --usage`. Every name below
-- was read from the bindings, so it can be cross-checked against Part 1.
--
-- Everything between --[[ and ]] is an example: this file is a definition file,
-- so never require() it and never attach it to an entity.
--
--   1.  the shape of a script file
--   2.  what the engine hands to your table
--   3.  inspector properties
--   4.  only init() runs by itself
--   5.  engine events and their arguments
--   6.  input
--   7.  moving a 2D body
--   8.  moving a 3D body
--   9.  camera, picking and raycasts
--   10. physics contacts and triggers
--   11. spawning, parenting and destroying
--   12. models, skeletal animation and morphs
--   13. sprites, particles and tweens
--   14. sound and music
--   15. UI: text, images, buttons, panels
--   16. textures, materials and shader uniforms
--   17. water
--   18. scenes: load, layer, preload
--   19. time, math and small helpers
--   20. require and file paths
--   21. logging and debugging a script that does nothing
--   22. gotchas that cost afternoons
--   23. procedural worlds: WorldGen, terrain and noise


-- ── 1. THE SHAPE OF A SCRIPT FILE ───────────────────────────────────────────
--
-- A script is one file that returns a table. The engine loads it, copies it
-- into an instance with __index, and calls init() on that instance.
--
--   scripts/Spin.lua
--[[
local Spin = {}

function Spin:init()
    -- Runs once, when the entity's script is created.
    RegisterEngineEvent(self, "onUpdate")
end

function Spin:onUpdate()
    local obj = Object(self.scene, self.entity)
    obj.rotation = obj.rotation * Quaternion(Engine.deltatime * 90.0, Vector3(0.0, 1.0, 0.0))
end

return Spin          -- without this line the engine logs "did not return a table"
]]
--
-- Attach it in the editor: select an entity, add the "Scripts" component, press
-- + and pick the file. The same thing written into a scene file by hand:
--[[
  - type: "Entity"
    entity: 3
    name: "Player"
    components:
      ...
      scripts:
        - type: "lua"
          path: "scripts/platformer_player.lua"
          enabled: true
]]


-- ── 2. WHAT THE ENGINE HANDS TO YOUR TABLE ─────────────────────────────────
--
-- Before init() runs, the instance already carries these fields:
--   self.scene    Scene     the scene owning the entity (same as Engine.scene for
--                           the main scene; use this one, layers have several)
--   self.entity   number    the entity id. It is a NUMBER, not an Entity object.
--   self.__name   string    the class name the editor shows in logs
--   self.<prop>             one field per entry of your `properties` table
--
-- Every wrapper class takes (scene, entity) and reads/writes that entity's
-- component, so the same id is passed straight through:
--[[
local obj  = Object(self.scene, self.entity)   -- transform: position, rotation, scale
local body = Body2D(self.scene, self.entity)   -- 2D physics body
local mesh = Mesh(self.scene, self.entity)     -- drawable mesh
]]
-- Calling a wrapper with the scene only creates a NEW entity that owns it:
--[[
local light = Light(self.scene)                -- new entity with a LightComponent
light:setType(LightType.POINT)
light:setColor(Vector3(1.0, 0.5, 0.2))
light:setRange(12.0)
]]
-- Because the wrapper owns the entity, keep it in a field or it is collected when
-- the local goes away. `obj.entityOwned` says who owns what.


-- ── 3. INSPECTOR PROPERTIES ─────────────────────────────────────────────────
--
-- A `properties` table in your module becomes rows in the Scripts section of the
-- Inspector. The editor runs your file to read it, so the table must be plain
-- data and the file must not do anything at load time.
--[[
local Mover = {
    properties = {
        { name = "speed",     displayName = "Speed",      type = "float",   default = 5.0 },
        { name = "jumps",     displayName = "Jumps",      type = "int",     default = 1 },
        { name = "grounded",  displayName = "Grounded",   type = "bool",    default = false },
        { name = "label",     displayName = "Label",      type = "string",  default = "hi" },
        { name = "offset",    displayName = "Offset",     type = "vector2", default = Vector2(0.0, 0.0) },
        { name = "target",    displayName = "Target",     type = "vector3", default = Vector3(0.0, 0.0, 0.0) },
        { name = "tint",      displayName = "Tint",       type = "color4",  default = Vector4(1, 1, 1, 1) },
        { name = "player",    displayName = "Player",     type = "entity",  default = nil },
    }
}

function Mover:init()
    -- Each property arrives as a field of the same name, already loaded from
    -- the scene file. `type = "entity"` arrives as an entity id you can pass
    -- straight into a wrapper - and it can be nil when nothing is picked.
    RegisterEngineEvent(self, "onUpdate")
end

function Mover:onUpdate()
    if not self.player then return end
    local other = Object(self.scene, self.player)
    Log.print("target is at " .. tostring(other.position))
end

return Mover
]]
-- Accepted `type` strings (case-insensitive): bool/boolean, int/integer,
-- float/number, string, vector2/vec2, vector3/vec3, vector4/vec4, color3,
-- color4, entity/entitypointer/pointer. Anything else is treated as an entity
-- reference of that name. Values edited in the Inspector are written to the
-- scene YAML, so they survive restarts; `default` is only the starting value.


-- ── 4. ONLY init() RUNS BY ITSELF ───────────────────────────────────────────
--
-- The engine calls init() and nothing else. A function named update, onUpdate,
-- start or render in your table is dead code until you subscribe it. This is the
-- single most common reason a quantum script "does nothing":
--[[
function Bad:update()          -- never called: there is no per-script update pump
    self.counter = (self.counter or 0) + 1
end
]]
-- Subscribe instead. RegisterEngineEvent(self, "name") looks up self.name and
-- binds it to Engine.<name>, with self as the first argument:
--[[
function Good:init()
    RegisterEngineEvent(self, "onUpdate")       -- every frame
    RegisterEngineEvent(self, "onFixedUpdate")  -- every physics step (1/60 s)
    RegisterEngineEvent(self, "onDraw")
    RegisterEngineEvent(self, "onSceneLoaded")
end

function Good:onUpdate()          -- onUpdate is FunctionSubscribe<void()>: no arguments
end
function Good:onFixedUpdate() end
]]
-- The name must exist on Engine or the call raises
-- "RegisterEngineEvent: Engine.onUppdate not found" - that error is the tell.
-- Pass a tag to identify the subscription later:
--[[
RegisterEngineEvent(self, "onUpdate", "hud.update")
Engine.removeSubscriptionsByTag("hud.")   -- drop it (e.g. on scene teardown)
]]
-- You can also assign the event directly. That is a *replacement*, not an
-- addition: only the last table assigned this way survives, and it receives the
-- raw arguments with no self.
--[[
Engine.onKeyDown = function(key, repeat, mods)
    if key == Input.KEY_ESCAPE then
        System.quit()
    end
end
]]


-- ── 5. ENGINE EVENTS AND THEIR ARGUMENTS ─────────────────────────────────────
--
-- Argument order is the C++ declaration, not a guess. When you subscribe with
-- RegisterEngineEvent, Lua's `self` is already the first parameter of the method
-- (it is `function T:onX(...)`), so the list below starts after it.
--[[
  Engine.onViewLoaded()                    view created
  Engine.onCanvasChanged()                 window resized (Lua name != C++ onViewChanged)
  Engine.onViewDestroyed()
  Engine.onDraw()                          every frame, before the framebuffer is shown
  Engine.onUpdate()                        every frame
  Engine.onFixedUpdate()                   every 1/60 s physics step
  Engine.onPostUpdate()
  Engine.onPause() / Engine.onResume()
  Engine.onShutdown()
  Engine.onSceneLoaded()                   the loaded stack is ready, loading scene still open
  Engine.onTouchStart(pointer, x, y)       onTouchEnd / onTouchMove / onTouchCancel same
  Engine.onMouseDown(button, x, y, mods)   onMouseUp same
  Engine.onMouseMove(x, y, mods)
  Engine.onMouseScroll(xoffset, yoffset, mods)
  Engine.onMouseEnter() / Engine.onMouseLeave()
  Engine.onKeyDown(key, repeat, mods)      onKeyUp same
  Engine.onCharInput(codepoint)            typed character, as a number
  Engine.onGamepadConnect(id) / onGamepadDisconnect(id)
  Engine.onGamepadButtonDown(id, button)   onGamepadButtonUp same
  Engine.onGamepadAxisMove(id, axis, value)
]]


-- ── 6. INPUT ─────────────────────────────────────────────────────────────────
--
-- `isKeyPressed` answers "is it down right now" (held), so it is polled from an
-- update hook; the KEY_* names live on Input as constants.
--[[
function Player:onUpdate()
    local mx, my = 0.0, 0.0
    if Input.isKeyPressed(Input.KEY_A) or Input.isKeyPressed(Input.KEY_LEFT)  then mx = mx - 1.0 end
    if Input.isKeyPressed(Input.KEY_D) or Input.isKeyPressed(Input.KEY_RIGHT) then mx = mx + 1.0 end
    if Input.isKeyPressed(Input.KEY_W) or Input.isKeyPressed(Input.KEY_UP)    then my = my + 1.0 end
    if Input.isKeyPressed(Input.KEY_S) or Input.isKeyPressed(Input.KEY_DOWN)  then my = my - 1.0 end

    if Input.isKeyPressed(Input.KEY_SPACE) then self.wantJump = true end

    self.moveDir = Vector2(mx, my)
end
]]
-- Mouse, touch and gamepad queries:
--[[
local p     = Input.getMousePosition()     -- Vector2, canvas pixels
local wheel = Input.getMouseScroll()       -- Vector2
local held  = Input.isMousePressed(Input.MOUSE_BUTTON_LEFT)
local mods  = Input.getModifiers()         -- bitmask of Input.MODIFIER_SHIFT / _CONTROL / _ALT

if Input.isTouch() then
    local tp = Input.getTouchPosition(0)   -- first pointer
    local n  = Input.numTouches()
    for i, t in ipairs(Input.getTouches()) do
        -- a Touch is a plain table: t.pointer (number) and t.position (Vector2)
        Log.print("pointer " .. t.pointer .. " at " .. tostring(t.position))
    end
end

if Input.isGamepadConnected(0) then
    local stick = Input.getGamepadAxis(0, Input.GAMEPAD_AXIS_LEFT_X)
    local jump  = Input.isGamepadButtonPressed(0, Input.GAMEPAD_BUTTON_A)
end
]]
-- For locking the pointer to a first-person camera:
--[[
    Engine.mouseMode = MouseMode.CAPTURED   -- NORMAL / HIDDEN / CAPTURED / CONFINED
    Engine.mouseCursor = CursorType.ARROW
    RegisterEngineEvent(self, "onMouseMove")
]]


-- ── 7. MOVING A 2D BODY ─────────────────────────────────────────────────────
--
-- Body2D wraps the Box2D body of the entity; Object wraps its transform. Prefer
-- velocity/force for physics-driven motion, and only write Object.position for
-- kinematic or non-physical entities.
--[[
function Platformer:init()
    RegisterEngineEvent(self, "onUpdate")
    RegisterEngineEvent(self, "onFixedUpdate")
end

function Platformer:onUpdate()
    local body = Body2D(self.scene, self.entity)
    local vx   = self.moveDir.x * self.speed
    body.linearVelocity = Vector2(vx, body.linearVelocity.y)
    body.angularVelocity = 0.0
end

function Platformer:onFixedUpdate()
    local body = Body2D(self.scene, self.entity)
    if self.wantJump and self.grounded then
        -- (impulse, wake) - a linear impulse is an instant change of momentum
        body:applyLinearImpulseToCenter(Vector2(0.0, self.jumpForce), true)
        self.wantJump = false
        self.grounded = false
    end
    -- Continuous push instead of an impulse:
    -- body:applyForceToCenter(Vector2(0.0, -9.8 * 10.0), true)
    -- body:applyTorque(0.5, true)
end
]]
-- Shapes, filters and behaviour, all settable from a script:
--[[
local body = Body2D(self.scene, self.entity)
body:createCenteredBoxShape(1.0, 2.0)
body:createCircleShape(Vector2(0.0, 0.0), 0.5)
body.shapeDensity    = 1.0            -- area based, so it sets the mass too
body.shapeFriction   = 0.6
body.shapeRestitution = 0.0
body.shapeSensor     = false
body.shapeContactEvents = true        -- required for beginContact2D to fire
body.shapeEnableHitEvents = true      -- required for hitContact2D
body:setBitsFilter(0x0002, 0xFFFF)    -- categoryBits, maskBits
body.type        = BodyType.DYNAMIC   -- STATIC / KINEMATIC / DYNAMIC
body.gravityScale = 1.0
body.bullet      = true               -- continuous collision detection (fast bodies)
body.fixedRotation = true
body.linearDamping = 0.1
]]
-- 2D units: `pointsToMeterScale` on the body converts pixels to metres (default
-- 64), so a 64 px sprite is 1 m. Gravity defaults to -9.81 in metres.


-- ── 8. MOVING A 3D BODY ─────────────────────────────────────────────────────
--[[
function Walker:onUpdate()
    local dt   = Engine.deltatime
    local obj  = Object(self.scene, self.entity)
    local body = Body3D(self.scene, self.entity)

    local dir = Vector3(self.moveDir.x, 0.0, self.moveDir.y)
    if dir:length() > 0.0 then
        dir = dir:normalize()
        obj.position = obj.position + dir * (self.speed * dt)   -- teleport the transform
        body.position = obj.position                            -- and carry the body with it
        local yaw = math.atan(dir.x, dir.z)
        -- slerp takes (t, from, to): it is a static, and Quaternion has no lerp
        obj.rotation = Quaternion.slerp(math.min(1.0, 12.0 * dt), obj.rotation,
                                       Quaternion(0.0, yaw, 0.0))
    end

    if self.wantJump then
        body:applyImpulse(Vector3(0.0, self.jumpStrength, 0.0))
        self.wantJump = false
    end
end
]]
-- Or let the solver do it, which is what you want on slopes and while pushing crates:
--[[
    body.linearVelocity = Vector3(dir.x * self.speed, body.linearVelocity.y, dir.y * self.speed)
    body:applyForce(Vector3(0.0, 0.0, self.speed * 50.0))
    body.type = BodyType.DYNAMIC
    body.mass = 70.0
    body.gravityFactor = 1.0
    body.friction = 0.5
    body.restitution = 0.0
    body.motionQuality = Body3DMotionQuality.LINEAR_CAST   -- stops tunnelling through walls
    body:setAllowedDOFs2DPlane()                          -- lock all but one axis
    body:createCapsuleShape(0.9, 0.35)                    -- half height, radius
]]


-- ── 9. CAMERA, PICKING AND RAYCASTS ─────────────────────────────────────────
-- The scene's active camera entity is `scene.camera`; Camera wraps it.
--[[
function Aim:init()
    RegisterEngineEvent(self, "onMouseDown")
end

function Aim:onMouseDown(button, x, y, mods)
    if button ~= Input.MOUSE_BUTTON_RIGHT then return end

    local cam = Camera(Engine.scene, Engine.scene.camera)
    local ray = cam:screenToRay(x, y)                -- Ray from the eye through that pixel

    -- A scene query always hands back a RayReturn, never nil: test `.hit`.
    local res = ray:intersects(Engine.scene, RayFilter.BODY_3D)
    if res.hit then
        Log.print("hit entity " .. tostring(res.body) .. " at " .. tostring(res.point))
        self.target = res.point
    else
        -- Nothing there: intersect the y = 0 plane by hand so a click still moves you.
        if ray.direction.y ~= 0.0 then
            local t = -ray.origin.y / ray.direction.y
            self.target = Vector3(ray.origin.x + ray.direction.x * t, 0.0,
                                  ray.origin.z + ray.direction.z * t)
        end
    end
end
]]
-- Other query shapes, and the filters you can hand to a scene ray:
--[[
local ray = Ray(Vector3(0, 5, 0), Vector3(0, -1, 0))   -- origin, direction (its length is the range)
local p   = ray:getPoint(3.0)                          -- Vector3 along the ray
local hit = ray:intersects(sphere)                     -- Sphere, AABB, OBB, Plane, Body2D, Body3D
local hit = ray:intersects(body3D, shapeIndex)         -- one body, one shape of it
local res = ray:intersects(Engine.scene, RayFilter.BODY_3D)     -- every 3D body
local res = ray:intersects(Engine.scene, RayFilter.BODY_2D)     -- every 2D body
local res = ray:intersects(Engine.scene, RayFilter.BOUNDS)      -- mesh AABBs, no physics needed
local res = ray:intersects(Engine.scene, RayFilter.BODY_3D, ignoreEntity)     -- skip one
local res = ray:intersects(Engine.scene, RayFilter.BODY_3D, {e1, e2})         -- skip a list
local res = ray:intersects(Engine.scene, RayFilter.BODY_3D, true)             -- only static
local res = ray:intersects(Engine.scene, RayFilter.BODY_3D, 0x0001, 0xFFFF)   -- category, mask
Ray.NO_HIT            -- a RayReturn with .hit false
RayReturn             -- .hit .distance .point .normal .body .shapeIndex
]]
-- Driving the camera itself:
--[[
cam:setPerspective(60.0, cam.aspect, 0.1, 500.0)
cam:setOrtho(-8.0, 8.0, -4.5, 4.5, 0.1, 100.0)
cam:setType(CameraType.CAMERA_ORTHO)        -- or CAMERA_PERSPECTIVE / CAMERA_UI
cam:rotateView(self.yawDelta)               -- around the target
cam:elevateView(self.pitchDelta)
cam:walkForward(self.scroll)
cam:setTarget(Vector3(0.0, 1.0, 0.0))
local forward = cam:getWorldDirection()
cam:updateCamera()
]]


-- ── 10. PHYSICS CONTACTS AND TRIGGERS ────────────────────────────────────────
-- Contacts are fields on the PhysicsSystem, one handler per event (it is an
-- assignment, so two scripts picking the same hook overwrite each other).
--[[
function Hazard:init()
    local physics = self.scene:getPhysicsSystem()

    physics.beginContact2D = function(shapeA, indexA, shapeB, indexB)
        if shapeA == self.body or shapeB == self.body then self.grounded = true end
    end
    physics.endContact2D = function(shapeA, indexA, shapeB, indexB)
        if shapeA == self.body or shapeB == self.body then self.grounded = false end
    end
    physics.hitContact2D = function(shapeA, indexA, shapeB, indexB, worldManifold)
        -- one-shot impact; needs shapeEnableHitEvents on the shape
    end
    physics.preSolve2D = function(contact, oldManifold)
        return true            -- returning false skips this solve
    end
    physics.shouldCollide2D = function(fixtureA, fixtureB)
        return true
    end
    physics.beginSensorContact2D = function(a, ia, b, ib) end
    physics.endSensorContact2D   = function(a, ia, b, ib) end

    -- 3D
    physics.onContactAdded3D    = function(bodyA, bodyB, contact, piercing) end
    physics.onContactPersisted3D = function(bodyA, bodyB, contact, piercing) end
    physics.onContactRemoved3D  = function(subShapeIDPair) end
    physics.onBodyActivated3D   = function(bodyID) end
    physics.onBodyDeactivated3D = function(bodyID) end
    physics.shouldCollide3D     = function(bodyA, bodyB) return true end

    physics.gravity2D = Vector2(0.0, -9.81)
    physics.gravity3D = Vector3(0.0, -9.81, 0.0)
    physics.pointsToMeterScale2D = 64.0
end
]]
-- A sensor volume is the usual pickup trigger:
--[[
local body = Body2D(self.scene, self.entity)
body:createCenteredBoxShape(1.0, 1.0)
body.shapeSensor = true
body.shapeSensorEvents = true
]]


-- ── 11. SPAWNING, PARENTING AND DESTROYING ───────────────────────────────────
-- Two routes: build an entity from the registry, or create a wrapper that owns one.
--[[
function Spawner:spawnPrefab(x, y)
    -- Route A: raw entity + the components you need, by hand.
    local e = self.scene:createEntity()          -- number
    self.scene:setEntityName(e, "Bullet")
    local obj = Object(self.scene, e)
    obj.position = Vector3(x, y, 0.0)
    local mesh = Mesh(self.scene, e)
    mesh:setTexture("Textures/bullet.png")
    local body = Body2D(self.scene, e)
    body:createCircleShape(Vector2(0.0, 0.0), 0.1)
    body:applyLinearImpulseToCenter(Vector2(0.0, 20.0), true)
    return e
end

function Spawner:spawnModel(path, pos)
    -- Route B: the wrapper creates the entity and owns it.
    local m = Model(self.scene)
    m:loadModel(path)
    Object(self.scene, m.entity).position = pos
    return m
end
]]
-- Hierarchy and cleanup:
--[[
self.scene:addEntityChild(parent, child, true)   -- true keeps the world transform
self.scene:moveChildToIndex(child, 0)
local root = self.scene:findOldestParent(entity)
local isAncestor = self.scene:isParentOf(root, child)
local list = self.scene:getEntityList()          -- every live entity id
local sig  = self.scene:getSignature(entity)
self.scene:destroyEntity(entity)                 -- destroys its children too
self.scene:setEntityName(entity, "NewName")
-- findEntity answers with NULL_ENTITY (0), not nil: compare against the global.
local found = self.scene:findEntity("Player")
if found ~= NULL_ENTITY then Log.print("found " .. tostring(found)) end
]]
-- An entity bundle (a prefab saved from the editor) is instanced by name:
--[[
local root = BundleManager.createBundle("Enemy", self.scene)   -- NULL_ENTITY if unknown
if root ~= NULL_ENTITY then
    Object(self.scene, root).position = Vector3(0.0, 1.0, 0.0)
    BundleManager.destroyBundle(self.scene, root)
end
BundleManager.getBundleNames()               -- every bundle the project registered
]]


-- ── 12. MODELS, SKELETAL ANIMATION AND MORPHS ────────────────────────────────
--[[
function Avatar:init()
    self.model = Model(self.scene, self.entity)   -- wraps the entity's ModelComponent
    self.model:loadModel("Models/BananaManAnim.glb")   -- or loadGLTF / loadOBJ
    self.idle = self.model:findAnimation("Idle")
    self.run  = self.model:findAnimation("Run")
    if self.run then self.model:playAnimation(self.run) end
    RegisterEngineEvent(self, "onUpdate")
end

function Avatar:onUpdate()
    if self.moving ~= self.lastMoving then
        self.lastMoving = self.moving
        self.model:playAnimation(self.moving and self.run or self.idle)
    end
    -- Morphs are indexed by id:
    self.model:setMorphWeight(0, self.blink)
    -- Bones, for attaching weapons to a hand:
    local bone = self.model:getBone("hand_r")
end
]]
-- The animation entity component (blend, fade) is `Animation`:
--[[
local anim = Animation(self.scene, self.entity)
anim.loop = false
anim.defaultFadeTime = 0.25
anim:fadeIn(0.3)
anim:fadeOut(0.3)
]]
-- Meshes expose the same animation surface plus LOD and instancing:
--[[
mesh.lodEnabled = true
mesh.castShadows = true
mesh:setFadeRange(40.0, 60.0)
mesh:createInstancedMesh()
mesh:addInstance(Vector3(1.0, 0.0, 2.0))   -- returns an instance index
mesh:updateInstance(0, Vector3(0.0, 0.0, 0.0))
mesh:setInstanceVisible(0, false)
mesh:updateInstances()
local n = mesh:getNumInstances()
]]


-- ── 13. SPRITES, PARTICLES AND TWEENS ───────────────────────────────────────
--[[
-- Sprite sheet animation: frames are rectangle ids inside the texture, in order.
local anim = SpriteAnimation(self.scene, self.entity)
anim:setAnimation({ 0, 1, 2, 3 }, 0.1, true)   -- frames, seconds per frame, loop

-- Particles are configured through initializers and modifiers.
local ps = Particles(self.scene, self.entity)
ps.rate = 40.0
ps.maxPerUpdate = 200.0
ps.loop = true
ps.localSpace = false
ps:setLifeInitializer(1.5)
ps:setSpherePositionInitializer(0.2)
ps:setVelocityInitializer(Vector3(0.0, 3.0, 0.0))
ps:setAccelerationInitializer(Vector3(0.0, -9.8, 0.0))
ps:setColorInitializer(Vector4(1.0, 0.6, 0.1, 1.0))
ps:setColorModifier(0.0, 1.5, Vector4(1.0, 0.6, 0.1, 1.0), Vector4(0.2, 0.2, 0.2, 0.0))
ps:setSizeModifier(0.0, 1.5, Vector3(0.3, 0.3, 0.3), Vector3(0.01, 0.01, 0.01))
ps:addBurst(0.0, 64)          -- one-shot burst at t = 0
ps:reset()

-- Tweens: an Action subclass wrapped around the entity, started by the system.
local move = PositionAction(self.scene, self.entity)
move:setAction(obj.position, obj.position + Vector3(4.0, 0.0, 0.0), 1.5, false)
move.loop = false
local fade = AlphaAction(self.scene, self.entity)
fade:setAction(1.0, 0.0, 0.6, false)
local scale = ScaleAction(self.scene, self.entity)
scale:setAction(Vector3(1.0, 1.0, 1.0), Vector3(1.3, 1.3, 1.3), 0.2, false)

-- Easing lives on TimedAction:
fade:setFunctionType(EaseType.BACK_OUT)     -- QUAD_IN, BACK_IN_OUT, BOUNCE_OUT, ...
fade.pingPong = true

local actions = self.scene:getActionSystem()
actions:actionStart(self.entity)            -- start everything the entity owns
actions:actionPause(self.entity)
actions:actionStop(self.entity)
local dur = actions:getDuration(self.entity)
]]


-- ── 14. SOUND AND MUSIC ─────────────────────────────────────────────────────
--[[
-- Non positional music: the bool overload asks for a 2D voice.
local music = Sound(self.scene, false)
music:loadSound("Sounds/theme.ogg")
music.looping = true
music.volume = 0.7
music:play()

-- Positional effect: created on an entity that already exists.
local sfx = Sound(self.scene, self.entity)
sfx:loadSound("Sounds/step.wav")
sfx.sound3D = true
sfx.minDistance = 1.0
sfx.maxDistance = 25.0
sfx.attenuationModel = SoundAttenuation.INVERSE_DISTANCE
sfx:play()

-- Transport: play() on a paused voice resumes it, stop() sends you back to 0.
if music:isPlaying() then music:pause() else music:play() end
music:seek(30.0)
local length = music:getLength()
local played = music:getPlayingTime()

-- Through the subsystem:
self.scene:getAudioSystem():stopSceneSounds()
AudioSystem.stopAll()
AudioSystem.pauseAll()
AudioSystem.resumeAll()
]]


-- ── 15. UI: TEXT, IMAGES, BUTTONS, PANELS ────────────────────────────────────
-- UI lives in its own scene or in the scene flagged for UI. A script widget is
-- built from a UILayout (anchors) plus a drawable (Text/Image/Button).
--[[
function Hud:init()
    local label = Text(self.scene)              -- owns a new entity
    label.text = "Score: 0"
    label.fontSize = 24.0
    label.color = Vector4(1.0, 1.0, 1.0, 1.0)
    local lay = UILayout(self.scene, label.entity)
    lay.anchorPreset = AnchorPreset.TOP_LEFT
    lay:setSize(220.0, 32.0)
    self.scoreLabel = label

    local button = Button(self.scene)
    local bl = UILayout(self.scene, button.entity)
    bl.anchorPreset = AnchorPreset.BOTTOM_RIGHT
    bl:setSize(160.0, 48.0)
    button.label = "Pause"
    button:setColorNormal(Vector4(0.2, 0.2, 0.25, 1.0))
    button:setColorHovered(Vector4(0.3, 0.3, 0.4, 1.0))
    button:setTextureNormal("Textures/button.png")

    -- Pointer events come from the UIComponent of the widget.
    local ui = button:getUIComponent()
    ui.onClick = function()
        self.paused = not self.paused
        button.label = self.paused and "Resume" or "Pause"
    end
    ui.onPointerEnter = function() end
    ui.onPointerDown  = function() end
    ui.onDrag         = function() end

    -- The scene must let UI eat events before the world sees them.
    self.scene:setEnableUIEvents(true)
    self.label = label
    RegisterEngineEvent(self, "onUpdate")
end

function Hud:onUpdate()
    self.scoreLabel.text = "Score: " .. tostring(self.score)
end
]]
-- Other widgets, all created the same way (layout + widget on one entity):
--[[
local img = Image(self.scene);  img:setTexture("Textures/heart.png"); img:setPatchMargin(8.0)
local bar = Progressbar(self.scene); bar.value = 0.75; bar:setFillColor(Vector4(0.1, 0.8, 0.3, 1.0))
local ed  = TextEdit(self.scene);    ed.placeholder = "Name"; ed.password = false
local pan = Panel(self.scene);       pan.title = "Inventory"; pan:setWindowProperties(true, true, true)
local box = Container(self.scene)
local sb  = Scrollbar(self.scene)
-- Layout helpers that a script uses most:
lay:setSize(w, h); lay:setAnchorPoints(0, 1, 0, 1); lay:setAnchorOffsets(l, t, r, b)
lay.anchorPreset = AnchorPreset.CENTER
lay.usingAnchors = true
local ui = self.scene:getUISystem()
ui:setAnchorReferenceSize(1280.0, 720.0)   -- design at a fixed resolution
local min = ui:getTextMinSize(self.label)
]]


-- ── 16. TEXTURES, MATERIALS AND SHADER UNIFORMS ─────────────────────────────
--[[
local tex = Texture("Textures/water_normal.png")    -- path is project relative
tex:load()
local w, h = tex:getWidth(), tex:getHeight()

local mesh = Mesh(self.scene, self.entity)
mesh:setTexture("Textures/stone.png")               -- or mesh.material = mat
mat.baseColorFactor = Vector4(1.0, 1.0, 1.0, 1.0)
mat.metallicFactor = 0.1
mat.roughnessFactor = 0.8
mat.emissiveFactor = Vector3(0.0, 0.0, 0.0)
mat.normalTexture = tex
mat.alphaMode = MaterialAlphaMode.BLEND

-- Per-object uniforms reach the shader of the drawn type:
mesh:setShaderUniform("u_vs_customParams", Vector4(1.0, 2.0, 0.5, 0.0))
local v = mesh:getShaderUniform("u_vs_customParams")
mesh:removeShaderUniform("u_vs_customParams")

-- Per-scene defaults for every mesh of a type:
self.scene:setDefaultShaderUniform(ShaderType.MESH, "u_vs_customParams", Vector4(1, 1, 1, 1))

-- Post-process chain: a pass is (shader, enabled) plus uniforms.
self.scene:setPostProcessPassEnabled(0, true)
self.scene:setPostProcessUniform(0, "u_fs_postParams", Vector4(1.0, 1.0, 1.0, 1.0))
self.scene:setPostProcessUniform(0, "u_tex_postprocess", tex)
]]
-- Custom shaders per scene, forked from the engine ones:
--[[
self.scene:setDefaultCustomShader(ShaderType.MESH, "Shaders/my_mesh.frag")
local path = self.scene:getDefaultCustomShader(ShaderType.MESH)
]]


-- ── 17. WATER ────────────────────────────────────────────────────────────────
-- Two layers: the drawn Gerstner surface and the CPU shallow-water field.
--[[
local water = Water(self.scene, self.entity)
water.size = Vector2(200.0, 200.0)
water.waveHeight = 0.35
water.waveLength = 12.0
water.waveSpeed = 1.4
water.waveDirection = 0.0
water.waveSteepness = 0.6
water.shallowColor = Vector3(0.35, 0.75, 0.75)
water.deepColor = Vector3(0.02, 0.13, 0.2)
water:setNormalTexture("Textures/water_normal.png")
water:load()

local h = water:getHeight(x, z)              -- surface height at a world position
local n = water:getNormal(x, z)
local depth  = water:getSimWaterDepth(x, z)  -- the shallow field, not the surface
local flow   = water:getSimVelocity(x, z)
water:addDisturbance(Vector3(px, 0.0, pz), 1.5, 0.4)   -- a splash where a body lands

-- Floating: Body3D already carries a buoyancy impulse.
local body = Body3D(self.scene, self.entity)
body.buoyancy = 1.0
body.waterDrag = 0.5
body.waterAngularDrag = 0.5
local surface = Vector3(pos.x, water:getHeight(pos.x, pos.z), pos.z)
body:applyBuoyancyImpulse(surface, n, body.buoyancy, body.waterDrag,
                          body.waterAngularDrag, flow, Vector3(0.0, -9.81, 0.0),
                          Engine.deltatime)

-- Keep a body out of the field so it can swim in a pool:
local excl = WaterExclusion(self.scene)
]]

-- Voxel editing is available to runtime Lua without the editor:
local voxels = VoxelWorld(305419896)
voxels:update(0, 0, 1)
voxels:setBlock(0, 0, 0, 7)
local block = voxels:getBlock(0, 0, 0)
local pendingMeshes = voxels.meshDirtyChunkCount
voxels:rebuildLighting()


-- ── 18. SCENES: LOAD, LAYER, PRELOAD ─────────────────────────────────────────
--[[
-- A scene must be registered before it can be loaded by name. The editor does
-- that for every scene of the project, so from a script you normally just load.
SceneManager.loadScene("Menu")               -- replaces the stack
SceneManager.reloadCurrentScene()            -- rebuilds the active stack through its factory
SceneManager.setLoadingScene("Loading")      -- shown while the new stack loads
local id   = SceneManager.getSceneId("Menu")
local name = SceneManager.getSceneName(id)
local all  = SceneManager.getSceneNames()
SceneManager.preloadScene("Level2")          -- async load of resources
local progress = SceneManager.getPreloadProgress("Level2")
SceneManager.cancelPreload("Level2")

-- Several scenes at once (HUD over world):
Engine.addSceneLayer(hudScene)               -- drawn on top
Engine.executeSceneOnce(oneShot)             -- run and drop
Engine.removeScene(hudScene)
Engine.removeAllSceneLayers(true)
local main = Engine.getMainScene()
local last = Engine.getLastScene()

-- Build a scene entirely from Lua:
local s = Scene()
s:setBackgroundColor(Vector4(0.1, 0.1, 0.12, 1.0))
s:setGravity3D(Vector3(0.0, -9.81, 0.0))
s.ssrEnabled = true
s.ssaoEnabled = true
Engine.addSceneLayer(s)
]]


-- ── 19. TIME, MATH AND SMALL HELPERS ────────────────────────────────────────
--[[
local dt = Engine.deltatime          -- seconds since last frame (clamped by maxDeltatime)
local fps = Engine.framerate
local t   = Engine.systemTime        -- seconds since the engine started

-- Frame-rate independent smoothing: never `pos = pos + delta * k`.
local k = 1.0 - math.exp(-self.stiffness * dt)
pos = pos + (target - pos) * k

-- Timers from an update hook:
self.elapsed = (self.elapsed or 0.0) + dt
if self.elapsed >= self.every then
    self.elapsed = self.elapsed - self.every
    self:fire()
end

-- There is no MathUtilities in Lua: clamp with math.min/math.max, and use the
-- value types' own helpers (Vector3:lerp, :moveTowards, :distance).
local k = math.max(0.0, math.min(1.0, raw))            -- clamp
local u = (v - a) / (b - a)                            -- inverse lerp

-- Easing is a class with statics and a callable instance:
Ease.linear(t); Ease.easeInQuad(t); Ease.easeOutBack(t); Ease.easeInOutBounce(t)
local ease = Ease(EaseType.BACK_OUT)   -- an Ease object wraps an EaseType
local y = ease(0.5)                    -- __call
ease.type = EaseType.QUAD_IN_OUT       -- and the actions take the same enum

System.getScreenWidth(); System.getScreenHeight()
System.isTouchDevice(); System.isFullscreen(); System.requestFullscreen()
System.setWindowSize(1280, 720); System.setWindowTitle("My game")
System.showVirtualKeyboard("Type something")           -- touch devices
System.getAssetPath(); System.getLuaPath(); System.getShaderPath(); System.getUserDataPath()
System.quit()

-- Files: paths go through FileData::getSystemPath, so a prefix picks the root
-- (data:// = user data, asset:// = the project, lua:// = the script root,
-- shader:// = the engine shaders). A bare relative path is asset://.
local f = File()
if f:open("data://save.bin", true) == 0 then   -- 0 = FileErrors.FILEDATA_OK
    f:writeString("1")
    f:close()
end
local g = File()
if g:open("asset://Data/level.bin") == 0 then
    local text = g:readString()
    local word = g:read32()
    g:close()
end
FileData.newFile("asset://x.bin", false)       -- static helper
FileData.getSystemPath("data://save.bin")      -- what the prefixes resolve to

UserSettings                                   -- string-keyed persisted values:
UserSettings.setBoolForKey("hints", true); UserSettings.getBoolForKey("hints")
UserSettings.setStringForKey("name", "Ada"); UserSettings.getStringForKey("name")

ThreadPoolManager                              -- background work (engine threading rules)
local pool = ThreadPoolManager.getInstance()
]]
-- Math types are value objects: every operator and every method returns a NEW
-- value, nothing mutates in place.
--[[
Vector2(1.0, 2.0)  Vector3(1.0, 2.0, 3.0)  Vector4(0.0, 0.0, 0.0, 1.0)
    Vector3(v2, 0.0)                        -- a 2D vector plus a height
    .x .y .z                                -- fields, writable
    + - * / unary-minus == <  all bound, so `a + b` and `v * 2.0` work
    :length() :squaredLength() :distance(o) :squaredDistance(o)
    :dotProduct(o) :absDotProduct(o) :crossProduct(o)     -- NOT dot / cross
    :normalize() :normalized() :lerp(o, t) :moveTowards(target, maxDelta)
    :midPoint(o) :perpendicular() :reflect(n) :makeFloor(o) :makeCeil(o) :isValid()
    Vector3.ZERO Vector3.UNIT_X Vector3.UNIT_Y Vector3.UNIT_Z Vector3.UNIT_SCALE

Quaternion(0.0, yaw, 0.0)                   -- Euler triple, in Engine.useDegrees units
Quaternion(0.7, Vector3(0.0, 1.0, 0.0))     -- axis-angle
Quaternion(w, x, y, z) Quaternion(Matrix4 m) Quaternion(m.xAxis, m.yAxis, m.zAxis)
    + - * unary-minus ==                       -- q * v rotates the vector
    :getEulerAngles(order) :inverse() :unitInverse() :normalize() :normalized()
    :norm() :dot(o) :equals(o) :xAxis() :yAxis() :zAxis()
    :getRoll() :getPitch() :getYaw()
    Quaternion.slerp(t, from, to)  Quaternion.nlerp(t, from, to)   -- t FIRST
    Quaternion.lookRotation(forward) Quaternion.squad(t, p, a, b, q)
    Quaternion.fromEulerAngles(...) is a method: q:fromEulerAngles(x, y, z, order)

Matrix4.translateMatrix(pos) Matrix4.rotateMatrix(angle, axis)
Matrix4.rotateXMatrix(a) Matrix4.rotateYMatrix(a) Matrix4.rotateZMatrix(a)
Matrix4.scaleMatrix(sf) Matrix4.lookAtMatrix(eye, center, up)
Matrix4.perspectiveMatrix(yfov, aspect, near, far) Matrix4.orthoMatrix(l,r,b,t,n,f)
Matrix4.frustumMatrix(l,r,b,t,n,f) Matrix4.reflectMatrix(plane)
    -- identity is per object: local m = Matrix4(); m:identity()
    m:inverse() m:transpose() m:determinant() m:linear() m:isValid()
    m:row(i) m:column(i) m:set(col, row, v) m:get(col, row)
    m:decompose(position, scale, rotation)     -- fills the three arguments
    + - * (matrix product) ==                  -- no / and no unary minus

Rect(x, y, width, height)   .x .y .width .height
    r:contains(point) r:getVector() r:fitOnRect(other) r:isNormalized() r:isZero()
Sphere(center, radius)  .center .radius
AABB(minimum, maximum)  AABB(minX, minY, minZ, maxX, maxY, maxZ)  .minimum .maximum
OBB(center, halfExtents, orientation)  .center .halfExtents .orientation
Plane(normal, d) Plane(a, b, c, d) Plane(p1, p2, p3)  p:getSide(obb)
Angle.degToRad(90.0) Angle.radToDeg(math.pi) Angle.degToDefault(d) Angle.defaultToDeg(d)
Color.linearTosRGB(rgb) Color.sRGBToLinear(rgb)   -- Vector3 or Vector4 in, same out
    -- Color has no constructor: a colour in Lua is a Vector3 or a Vector4
]]


-- ── 20. REQUIRE AND FILE PATHS ───────────────────────────────────────────────
-- The engine installs one searcher (LuaBinding::moduleLoader) and it replaces the
-- defaults, so package.path is never read: a name is looked up as
-- `lua://<name>.lua`, then `lua://lua/<name>.lua`, both under the project's Lua
-- root. Dots become directory separators, so the folder form is a dotted name.
--[[
local Vec = require("Math3D")            -- <lua root>/Math3D.lua
local Net = require("lib.net")           -- <lua root>/lib/net.lua
-- Modules load once and package.loaded caches them. Return the table: a module
-- that returns nothing yields `true`, so `local m = require("x")` gives no fields.
]]
-- Every path handed to an engine call goes through FileData::getSystemPath:
--[[
  "asset://Textures/x.png"   the project directory (the default, so a bare
  "Models/x.glb"             "Textures/x.png" means the same thing)
  "data://save.bin"          the per-user writable data directory
  "lua://scripts/player.lua" the project's Lua root
  "shader://my.frag"         the engine shader directory
  "builtin://BananaMan.glb"  a model the engine carries in its own binary
  Absolute paths (/x or C:\x) pass through unchanged. Case matters on Linux and
  Android even though Windows forgives it.
]]


-- ── 21. LOGGING AND DEBUGGING A SCRIPT THAT DOES NOTHING ─────────────────────
--[[
Log.print("one string only")             -- Log.print takes ONE argument
Log.verbose(m) Log.debug(m) Log.warn(m) Log.error(m)
-- There is no printf-style vararg binding, so format with concatenation and
-- tostring - a table concatenated as-is raises an error:
Log.print("hp=" .. tostring(self.hp) .. " pos=" .. tostring(obj.position))
]]
-- When a script is silent, check in this order:
--   1. Does the file return a table? ("did not return a table" in the log.)
--   2. Is the script enabled and is the path right for the scene's Lua root?
--   3. Did init() actually run? Put a Log.print in it - if that line is missing,
--      the component or the path is wrong, not your logic.
--   4. Is the per-frame method *subscribed*? (section 4 - by far the usual cause.)
--   5. Did RegisterEngineEvent raise? Its message names the event it could not find.
--   6. Does the entity really have the component you wrapped? Wrapping an entity
--      that lacks it does not add it; check `self.scene:getSignature(entity)`.
--   7. Run it without the editor:
--      quantum-engine.exe --project <dir> --scene main.lua --window 1280x720
--      and read stdout - Lua errors are printed with a traceback.


-- ── 22. GOTCHAS THAT COST AFTERNOONS ─────────────────────────────────────────
-- * self.entity is a number. `Object(self.scene, self.entity)` is the idiom; the
--   wrapper does not accept an entity id on its own.
-- * Only init is dispatched. `function T:update()` never runs unless you
--   subscribe it - several example scripts in templates/ still show the old shape.
-- * Assigning Engine.onX = f replaces every earlier assignment to that event;
--   RegisterEngineEvent adds a handler instead. Prefer subscribing in scripts.
-- * Component hooks (ui.onClick, physics.beginContact2D) are also single
--   assignments, and they are per-scene, not per-entity: filter by self.entity
--   inside the handler.
-- * Input.isKeyPressed means "held", not "pressed this frame". For one-shot
--   reactions use Engine.onKeyDown / onKeyUp, which carry `repeat`.
-- * Engine.deltatime is read-only. Pause logic by not running, not by writing it.
-- * Vector/Quaternion operators return new values: `obj.rotation.x = 1` does not
--   write back into the transform. Assign the whole value.
-- * Angles: Engine.useDegrees decides whether Quaternion/Euler take degrees or
--   radians, so a script that mixes the two spins differently per project.
-- * 2D physics scale is pointsToMeterScale2D = 64 by default and it is NOT
--   serialized in the scene file; 2D densities are per area, 3D ones are kg/m3.
-- * Body3D.position and Object.position are two views of one entity. Moving the
--   transform of a dynamic body fights the solver - pick one (prefer the body).
-- * Sensor and contact hooks need the matching shape flag
--   (shapeContactEvents / shapeEnableHitEvents / shapeSensorEvents) or no event
--   is generated at all.
-- * The queries you reach for first do not answer nil for "not found":
--   `ray:intersects(scene, ...)` gives a RayReturn (always truthy - read `.hit`), and
--   `findEntity` gives `NULL_ENTITY` (0). `if hit then` / `if found then` is the most
--   common Lua bug here: it is always true, and the next line reads garbage out of
--   `.point`.
-- * Shapes added from a script are also in the scene file if the editor authored
--   them; createBody2D/loadBody2D and `body:load()` re-read the component, so do
--   not add a shape per frame.
-- * Destroying an entity invalidates every wrapper that owned it; a later field
--   write on a dangling wrapper raises. Clear your references in the same call.
-- * The editor executes your file to read the `properties` table, so nothing at
--   file scope may assume it is running in a game (no Engine.scene, no spawning).
-- * Types.lua is a definition file: never require it, never attach it to a scene.


-- ── 23. PROCEDURAL WORLDS: WORLDGEN, TERRAIN AND NOISE ───────────────────────
--
-- A heightmap is a 16-bit grayscale PNG. `Terrain:setHeightMap` reads it, the vertex shader
-- displaces the CDLOD grid by `u_heightMap.r * terrain.maxHeight`, and the same file is what the
-- terrain editor's brush writes - so a generated map can be painted over afterwards.
--
-- WorldGen writes that file. `options.out` is required and is a filesystem path relative to the
-- running project directory ("assets/textures/..."), while a component texture path is relative to
-- the asset root ("textures/..."). Two spellings, one file; mixing them up is the usual reason a
-- generated map seems to do nothing.
--
--   WorldGen.presets()                       -> { "continents", "island", ... }
--   WorldGen.params(name)                    -> the preset as a table of fields
--   WorldGen.generate(options)               -> ok, error, size
--   WorldGen.waterHeight(optionsOrName, max) -> the world y a water plane needs
--
-- `options` is a preset name plus any fields to override: preset, seed, size, noise, octaves,
-- lacunarity, gain, frequency, base, amplitude, curve, ridged, warp, warpFrequency, seaLevel,
-- island, droplets, erosionScale, erosionSeed, talus, thermalRate. `size` is clamped to 8..4096 and
-- the third return value is the size actually written.
--[[
local scene = Engine.getScene()

local params = WorldGen.params("island")
params.seed = 7313                      -- the whole map comes out of this one number
params.size = 512

local ok, err, size = WorldGen.generate({
    out = "assets/textures/my_island.png",
    preset = "island",
    seed = 7313,
    size = 512,
    erosionSeed = 99,                   -- rivers, separately seeded from the relief
})
if not ok then
    Log.error("generation failed: " .. err)
end

-- The camera goes in before the terrain: a Lua-built terrain with no scene camera goes through
-- MeshSystem::resetTerrainToUnbuilt, which clears the update flag for good. A Camera built from a
-- script is a UI camera with a 10-unit far clip, so it needs an explicit perspective projection.
local camera = Camera(scene)
camera.position = Vector3(0.0, 120.0, -330.0)
camera.target = Vector3(0.0, 25.0, 0.0)
camera:setPerspective(55.0, 1280.0 / 720.0, 0.5, 3000.0)
camera.autoResize = true
scene:setCamera(camera)

local terrain = Terrain(scene)
terrain.size = 400.0                    -- metres across
terrain.maxHeight = 60.0                -- the elevation range, from the entity's y upwards
terrain.resolution = 64                 -- rounded up to a multiple of 4
terrain.levels = 8
terrain:setHeightMap("textures/my_island.png")
terrain:createTerrain()

local sea = Water(scene)
sea:setPosition(0.0, WorldGen.waterHeight(params, terrain.maxHeight), 0.0)
sea.size = Vector2(1400.0, 1400.0)
]]
--
-- The same generator without a window or a project, from the command line:
--   quantum-editor-cmd terrain generate --out map.png --preset mountains --size 1024 --seed 4242
--   quantum-editor-cmd terrain info map.png            -- decodes it and prints min/mean/max
--
-- The fields underneath it are `quantum::Noise`, all deterministic on the integer hash alone, so
-- the same (coordinates, seed) gives the same value on MSVC, MinGW and from a pooled task:
-- * `Noise.perlin2/3`, `simplex2/3`, `value2/3` in [-1, 1]; `hash2/3` in [0, 1] and discontinuous.
-- * `Noise.fbm(type, x, y, seed, octaves, lacunarity, gain)` - the smooth sum, hills.
-- * `Noise.turbulence(...)` - the same octaves of |sample|, ridged but rounded.
-- * `Noise.ridged(...)` - (1 - |sample|)^2 per octave: sharp mountain divides. [0, 1].
-- * `Noise.warp2(x, y, seed, amplitude, frequency)` moves the coordinates themselves (by a simplex
--   field), which is what makes a coastline bend instead of following a lattice; `Noise.warpedFbm`
--   is the fbm of a warped point. `amplitude = 0` is exactly the identity, so it costs nothing
--   unused.
-- * Sample at world position divided by a scale, never at a pixel index, and `octaves` is clamped
--   to `Noise.MAX_OCTAVES` = 16 because these run inside per-pixel loops.
--
-- There is still no noise inside a shader (`engine/shaderlib/includes/`), so per-pixel procedural
-- detail is either a texture generated at load - which is what the block above writes - or a
-- hand-written GLSL hash.
