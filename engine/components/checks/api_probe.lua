-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- The harness proves a component asks for a member that exists; this proves the calls also match the
-- argument shapes LuaBridge really registered. build/generated/lua_api.md renders C++ header
-- signatures, so an overloaded setter such as Water:setSize(Vector2) looks fine there and is rejected
-- in Lua (only the float overload is registered). Every probe runs inside onViewLoaded, because
-- Water:load() and the Jolt joints need a live view. Run with:
--   quantum-engine.exe --project <KitLive> --scene api_probe
local keep = {}

local checks = {}
local function check(name, body) checks[#checks + 1] = { name = name, body = body, bad = false } end
-- A trap probe is expected to fail: it documents the shape that must NOT be used, and the line turns
-- into a warning if the bindings ever gain the overload.
local function trap(name, body) checks[#checks + 1] = { name = name, body = body, bad = true } end

local function main()
    local scene = Scene()
    Engine.scene = scene
    keep[#keep + 1] = scene

    local function entity(name, x, y, z)
        local object = Object(scene)
        object.position = Vector3(x or 0.0, y or 0.0, z or 0.0)
        object.name = name
        keep[#keep + 1] = object
        return object.entity
    end

    -- ---- environment ------------------------------------------------------
    check("scene.backgroundColor = Vector4", function() scene.backgroundColor = Vector4(0.55, 0.72, 0.88, 1.0) end)
    trap("Scene:setBackgroundColor(Vector4)", function() scene:setBackgroundColor(Vector4(0.55, 0.72, 0.88, 1.0)) end)

    local water = Water(scene)
    water.name = "Water"
    keep[#keep + 1] = water
    check("water.size = Vector2", function() water.size = Vector2(80, 80) end)
    check("water.shallowColor = Vector3", function() water.shallowColor = Vector3(0.2, 0.6, 0.6) end)
    check("water.deepColor = Vector3", function() water.deepColor = Vector3(0.01, 0.15, 0.28) end)
    check("water.foamColor = Vector3", function() water.foamColor = Vector3(1, 1, 1) end)
    check("water.waveHeight", function() water.waveHeight = 0.4 end)
    check("water.simulate", function() water.simulate = true end)
    check("Water:load", function() return water:load() == true end)
    check("Water:addDisturbance(Vector3,number,number)", function() water:addDisturbance(Vector3(0, 0, 0), 1.5, 0.4) end)
    check("Water:getHeight", function() return type(water:getHeight(0, 0)) == "number" end)
    check("Water:getNormal", function() return water:getNormal(0, 0) ~= nil end)
    check("Water:getSimWaterDepth", function() return water:getSimWaterDepth(0, 0) ~= nil end)
    check("Water:getSimVelocity", function() return water:getSimVelocity(0, 0) ~= nil end)
    check("Water:getSimVolume", function() return type(water:getSimVolume()) == "number" end)
    check("Water:setShaderUniform", function() water:setShaderUniform("u_test", 1.0) end)
    trap("Water:setSize(Vector2)", function() water:setSize(Vector2(80, 80)) end)
    trap("Water:setShallowColor(Vector3)", function() water:setShallowColor(Vector3(0.2, 0.6, 0.6)) end)

    local sky = SkyBox(scene)
    keep[#keep + 1] = sky
    check("sky.color = Vector4", function() sky.color = Vector4(0.45, 0.68, 0.92, 1.0) end)
    trap("SkyBox:setColor(Vector4)", function() sky:setColor(Vector4(0.45, 0.68, 0.92, 1.0)) end)

    local fog = Fog(scene)
    keep[#keep + 1] = fog
    check("fog.color = Vector3", function() fog.color = Vector3(0.75, 0.87, 0.95) end)
    check("Fog:setLinearStartEnd(float,float)", function() fog:setLinearStartEnd(40, 420) end)
    trap("Fog:setColor(Vector3)", function() fog:setColor(Vector3(0.75, 0.87, 0.95)) end)

    local terrain = Terrain(scene)
    terrain.name = "Terrain"
    keep[#keep + 1] = terrain
    check("terrain.size", function() terrain.size = 96 end)
    check("terrain.maxHeight", function() terrain.maxHeight = 5 end)
    check("terrain.resolution", function() terrain.resolution = 32 end)
    check("terrain.levels", function() terrain.levels = 5 end)
    check("terrain.textureBaseTiles", function() terrain.textureBaseTiles = 6 end)
    check("Terrain:setTextureDetailBlue(path)", function() terrain:setTextureDetailBlue("missing.png") end)

    local sun = Light(scene)
    keep[#keep + 1] = sun
    check("Light:setType(LightType.DIRECTIONAL)", function() sun:setType(LightType.DIRECTIONAL) end)
    check("sun.direction = Vector3", function() sun.direction = Vector3(-0.4, -0.86, -0.28) end)
    check("sun.color = Vector3", function() sun.color = Vector3(1, 0.96, 0.9) end)
    check("Light:setIntensity", function() sun:setIntensity(1.2) end)
    check("Light:setShadows", function() sun:setShadows(true) end)
    trap("Light:setDirection(Vector3)", function() sun:setDirection(Vector3(-0.4, -0.86, -0.28)) end)
    trap("Light:setColor(Vector3)", function() sun:setColor(Vector3(1, 0.96, 0.9)) end)

    local camera = Camera(scene)
    camera.name = "Camera"
    keep[#keep + 1] = camera
    check("camera.target = Vector3", function() camera.target = Vector3(0, 0.5, 0) end)
    check("camera.yfov", function() camera.yfov = 60 end)
    check("Scene:setCamera(Camera)", function() scene:setCamera(camera) end)
    check("Camera:screenToRay(float,float)", function() return camera:screenToRay(10, 10) ~= nil end)
    trap("Camera:setTarget(Vector3)", function() camera:setTarget(Vector3(0, 0.5, 0)) end)
    trap("Camera:screenToRay(Vector2)", function() return camera:screenToRay(Vector2(10, 10)) ~= nil end)

    -- ---- particles and sound ---------------------------------------------
    local particles = Particles(scene)
    keep[#keep + 1] = particles
    check("particles.emitter = bool", function() particles.emitter = true end)
    check("particles.loop", function() particles.loop = false end)
    check("particles.rate", function() particles.rate = 20 end)
    check("Particles:setPositionInitializer(Vector3)", function() particles:setPositionInitializer(Vector3(0, 1, 0)) end)
    check("Particles:setVelocityInitializer(Vector3)", function() particles:setVelocityInitializer(Vector3(0, 2, 0)) end)
    check("Particles:setColorInitializer(Vector3)", function() particles:setColorInitializer(Vector3(1, 1, 1)) end)
    check("Particles:setLifeInitializer(float,float)", function() particles:setLifeInitializer(0.4, 1.2) end)
    check("Particles:setSizeInitializer(float,float)", function() particles:setSizeInitializer(0.1, 0.4) end)
    check("Particles:setSpriteIntializer(int,int)", function() particles:setSpriteIntializer(0, 3) end)
    check("Particles:addBurst(float,int)", function() particles:addBurst(0.0, 12) end)
    check("Particles:clearBursts", function() particles:clearBursts() end)
    check("Particles:reset", function() particles:reset() end)
    trap("Particles:setEmitter(Vector3)", function() particles:setEmitter(Vector3(0, 1, 0)) end)
    trap("Particles:clear", function() particles:clear() end)
    trap("Particles:setSpriteIntializer(path)", function() particles:setSpriteIntializer("missing.png") end)

    local sound = Sound(scene)
    keep[#keep + 1] = sound
    check("sound.sound3D", function() sound.sound3D = true end)
    check("sound.volume", function() sound.volume = 0.5 end)
    check("sound.pan", function() sound.pan = 0.0 end)
    check("sound.dopplerFactor", function() sound.dopplerFactor = 1.0 end)
    check("Sound:setMinMaxDistance", function() sound:setMinMaxDistance(4, 60) end)

    -- ---- physics ----------------------------------------------------------
    local crateId = entity("Crate", 4, 3, 6)
    local physics = scene:getPhysicsSystem()
    keep[#keep + 1] = physics
    check("PhysicsSystem:createBody3D", function() physics:createBody3D(crateId) end)
    local body = Body3D(scene, crateId)
    keep[#keep + 1] = body
    check("body.type = BodyType.DYNAMIC", function() body.type = BodyType.DYNAMIC end)
    check("Body3D:createBoxShape(float,float,float)", function() body:createBoxShape(1, 1, 1) end)
    check("PhysicsSystem:loadBody3D", function() return physics:loadBody3D(crateId) ~= nil end)
    check("body.linearVelocity = Vector3", function() body.linearVelocity = Vector3(0, 0, 0) end)
    check("body.mass", function() return type(body.mass) == "number" end)
    check("body.buoyancy", function() body.buoyancy = 1.0 end)
    check("body.gravityFactor", function() body.gravityFactor = 1.0 end)
    check("body.friction", function() body.friction = 0.7 end)
    check("Body3D:getCenterOfMassPosition", function() return body:getCenterOfMassPosition() ~= nil end)
    check("Body3D:applyImpulse(Vector3)", function() body:applyImpulse(Vector3(1, 2, 3)) end)
    check("Body3D:applyForce(Vector3)", function() body:applyForce(Vector3(0, 5, 0)) end)
    check("Body3D:activate", function() body:activate() end)
    check("Body3D:applyBuoyancyImpulse", function()
        body:applyBuoyancyImpulse(Vector3(0, 0, 0), Vector3(0, 1, 0), 1.0, 0.5, 0.5, Vector3.ZERO, Vector3(0, -9.81, 0), 0.016)
    end)
    check("Vector3 math", function()
        local v = body.linearVelocity
        local d = Vector3(0, -1, 0)
        v = v - d * (2.0) + d / 2.0
        return type(v) == "userdata" and type(v.x) == "number"
    end)

    check("scene.gravity3D = Vector3", function() scene.gravity3D = Vector3(0, -9.81, 0) end)
    check("physics.pointsToMeterScale2D", function() physics.pointsToMeterScale2D = 64 end)

    -- A Jolt joint needs a body on BOTH ends: loadPointJoint3D checks the two signatures and only
    -- leaves needUpdateJoint set when it refuses, so the failure is invisible without this probe.
    local anchorId = entity("Anchor", 0, 6, 0)
    check("anchor static body", function()
        physics:createBody3D(anchorId)
        local anchor = Body3D(scene, anchorId)
        keep[#keep + 1] = anchor
        anchor.type = BodyType.STATIC
        anchor:createBoxShape(0.5, 0.5, 0.5)
        return physics:loadBody3D(anchorId) ~= nil
    end)
    local joint = Joint3D(scene)
    keep[#keep + 1] = joint
    check("Joint3D:setPointJoint(Entity,Entity,Vector3)", function()
        joint:setPointJoint(anchorId, crateId, Vector3(0, 6, 0))
        return true
    end)
    check("Joint3D:setDistanceJoint(Entity,Entity)", function() joint:setDistanceJoint(anchorId, crateId) end)
    trap("joint.enabled", function() joint.enabled = true end)
    -- Joint3DType has no LuaBridge Stack push, so getType() raises instead of returning the enum.
    trap("Joint3D:getType", function() return joint:getType() end)

    -- ---- the shapes every component has to read ---------------------------
    -- The engine pushes vectors, handles and the scene as userdata and only the script instances as
    -- tables, so `type(value) == "table"` is never a safe test for a value a component receives.
    check("Vector3 is userdata", function() return type(Vector3(1, 2, 3)) == "userdata" end)
    check("Vector2 is userdata", function() return type(Input.getMousePosition()) == "userdata" end)
    check("entity id is a number", function() return type(crateId) == "number" end)
    check("entity handle is userdata", function() return type(Object(scene, crateId)) == "userdata" end)
    check("scene is userdata", function() return type(scene) == "userdata" end)
    check("findEntity(missing) is 0", function() return scene:findEntity("nothing here") == 0 end)
    -- The Lua function is bound as (float, Vector3); only the differently named setter and the
    -- properties take a bare vector, so the header shape in the generated reference is wrong here.
    check("scene:setGlobalIllumination(float,Vector3)", function() scene:setGlobalIllumination(0.2, Vector3(0.2, 0.2, 0.25)) end)
    check("scene:setGlobalIlluminationColor(Vector3)", function() scene:setGlobalIlluminationColor(Vector3(0.2, 0.2, 0.25)) end)
    check("scene.globalIlluminationColor = Vector3", function() scene.globalIlluminationColor = Vector3(0.2, 0.2, 0.25) end)
    trap("scene:setGlobalIllumination(Vector3)", function() scene:setGlobalIllumination(Vector3(0.2, 0.2, 0.25)) end)
    -- getWorldDirection is not registered on Object, so a facing read must go through the rotation.
    check("Object:getWorldRotation", function() return Object(scene, crateId):getWorldRotation() ~= nil end)
    trap("Object:getWorldDirection", function() return Object(scene, crateId):getWorldDirection() ~= nil end)
    -- loadSound answers true even when the file is not there: a component cannot trust it and must
    -- treat the sound as silent until it actually plays.
    trap("sound:loadSound(missing) is falsy", function() return not sound:loadSound("missing.wav") end)

    -- ---- the vector algebra the components build their math from ----------
    -- DayNightCycle draws the sun on a great circle, WindZone rotates a direction and Rope solves a
    -- pivot, so all of them live or die by the metamethods registered on the userdata.
    check("Vector3 arithmetic", function()
        local a, b = Vector3(1, 2, 3), Vector3(4, 5, 6)
        local sum = a + b
        local scaled = a * 2.0
        local negated = -a
        return sum.x == 5 and scaled.y == 4 and negated.z == -3 and (a - b).x == -3
    end)
    check("Vector3:perpendicular/normalized/crossProduct", function()
        local east = Vector3(1, 0, 0):perpendicular():normalized()
        local zenith = east:crossProduct(Vector3(1, 0, 0)):normalized()
        return east:length() > 0.99 and zenith ~= nil
    end)
    check("Vector3:normalized() of a zero vector", function()
        -- Normalize-then-test-length is the guard DayNightCycle uses, so the zero vector must come
        -- back as a zero vector rather than raise or return garbage.
        return Vector3(0, 0, 0):normalized():length() == 0
    end)
    check("Vector3:lerp/distance/dot", function()
        local a, b = Vector3(0, 0, 0), Vector3(3, 4, 0)
        return a:lerp(b, 0.5).x == 1.5 and a:distance(b) == 5
            and Vector3(1, 2, 3):dotProduct(Vector3(0, 1, 0)) == 2
    end)
    check("Vector3 scalar multiply both orders", function()
        -- addConstVectorMul registers 2 * v as well as v * 2, which the colour ramps rely on.
        local v = Vector3(1, 2, 3)
        return (2.0 * v).z == 6 and (v * 2.0).z == 6
    end)
    check("Vector2 arithmetic", function()
        local a, b = Vector2(1, 2), Vector2(3, 4)
        -- the engine stores float32, so an irrational length must be compared with a tolerance
        return (a + b).x == 4 and (a * 2.0).y == 4 and math.abs(a:length() - math.sqrt(5)) < 1e-5
    end)
    check("Vector4 arithmetic", function()
        -- use halves and quarters: 0.2f is not 0.2, so a decimal literal comparison tests the float
        -- round trip rather than the operator
        local c = Vector4(0.25, 0.5, 0.75, 1.0) * 0.5
        local d = Vector4(0.125, 0.25, 0.375, 0.5) + c
        return c.x == 0.125 and c.w == 0.5 and d.y == 0.5
    end)
    check("vector fields read from userdata", function()
        local v = Vector3(1, 2, 3)
        return v.x == 1 and v.y == 2 and v.z == 3
    end)
    trap("Vector3:index like a table", function()
        -- Property defaults arrive as { r, g, b } tables, but a value read back from the engine is
        -- userdata, so a reader cannot fall back to [1]/[2]/[3].
        return Vector3(1, 2, 3)[1] ~= nil
    end)

    -- ---- queries and post-process ----------------------------------------
    check("Ray(Vector3,Vector3)", function() return Ray(Vector3(0, 4, 0), Vector3(0, -1, 0)) ~= nil end)
    check("Ray:intersects(Scene,RayFilter,Entity)", function()
        local ray = Ray(Vector3(0, 4, 0), Vector3(0, -1, 0))
        return ray:intersects(scene, RayFilter.BODY_3D, crateId).hit ~= nil
    end)
    check("Scene:findEntity", function() return scene:findEntity("Crate") == crateId end)
    check("Scene:setPostProcessUniform(int,string,float)", function() scene:setPostProcessUniform(0, "fade", 0.5) end)
    check("Scene:setPostProcessUniform(int,string,Vector4)", function() scene:setPostProcessUniform(0, "tint", Vector4(1, 1, 1, 0.5)) end)
    check("Scene:getPostProcessUniform", function() return scene:getPostProcessUniform(0, "fade") ~= nil end)
    check("Scene:setPostProcessPassEnabled", function() scene:setPostProcessPassEnabled(0, true) end)
    check("Scene:setEnableUIEvents(bool)", function() scene:setEnableUIEvents(false) end)
    check("Engine.systemTime", function() return type(Engine.systemTime) == "number" end)
    check("Input.getMousePosition", function()
        local m = Input.getMousePosition()
        return type(m.x) == "number" and type(m.y) == "number"
    end)

    local failures, surprises = 0, 0
    for _, entry in ipairs(checks) do
        local ok, result = pcall(entry.body)
        local passed = ok and result ~= false
        if entry.bad then
            if passed then surprises = surprises + 1 end
            print(string.format("[api] %-9s %s  %s", passed and "PASS?!" or "refused", entry.name,
                passed and "the bindings now accept this shape" or tostring(result)))
        elseif passed then
            print(string.format("[api] ok        %s", entry.name))
        else
            failures = failures + 1
            print(string.format("[api] FAIL      %s  %s", entry.name, ok and "returned false" or tostring(result)))
        end
    end
    print(string.format("[api] %d calls probed against the real bindings, %d failures, %d traps now valid",
        #checks, failures, surprises))

    System.quit()
end

local driver
driver = { done = false, onViewLoaded = function()
    if driver.done then return end
    driver.done = true
    main()
end }
RegisterEngineEvent(driver, "onViewLoaded")
keep[#keep + 1] = driver
