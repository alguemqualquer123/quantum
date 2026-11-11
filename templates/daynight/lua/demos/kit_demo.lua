-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Shared driver for the demo scripts in this folder. It builds scene content with the wrapper
-- constructors and runs library components the way a ScriptComponent would, so every demo is a
-- plain Lua file that quantum-engine can run headless without an editor-authored scene.
local Kit = { keep = {}, count = 0 }

local function number(value, fallback)
    if type(value) == "number" and value == value then return value end
    return fallback
end

-- A reference reaches the driver as a wrapper handle, as a bare id, or as nil.
local function entityId(reference)
    if type(reference) == "number" then
        if reference == 0 then return nil end
        return reference
    end
    if reference and type(reference.entity) == "number" then
        if reference.entity == 0 then return nil end
        return reference.entity
    end
    return nil
end

-- The engine only fills a script's fields after the view exists, and resource loads need the
-- same moment, so a demo passes its builder here instead of running it at load time.
function Kit.start(builder)
    local driver
    driver = {
        name = "KitDemo",
        done = false,
        onViewLoaded = function()
            if driver.done then return end
            driver.done = true
            -- A builder that raises halfway leaves a half-wired scene ticking, and every later error
            -- then points at the wrong file, so the cut is named here instead.
            local ok, err = pcall(builder)
            if not ok then
                print("[demo] the builder stopped: " .. tostring(err))
            end
        end
    }
    RegisterEngineEvent(driver, "onViewLoaded")
    Kit.keep[#Kit.keep + 1] = driver
    return driver
end

-- Engine.getScene() reads scenes[0] without checking it, so a runner start has no scene yet: the
-- demo builds one and keeps its own reference instead of asking the engine for it back.
function Kit.createScene()
    local scene = Scene()
    Engine.scene = scene
    Kit.keep[#Kit.keep + 1] = scene
    return scene
end

-- A Transform is what every component reads a position from, so entities are born as Objects.
function Kit.spawn(scene, name, x, y, z)
    local object = Object(scene)
    object.position = Vector3(number(x, 0.0), number(y, 0.0), number(z, 0.0))
    if name and name ~= "" then
        object.name = name
    end
    Kit.keep[#Kit.keep + 1] = object
    return object.entity, object
end

-- `Water(scene)` and friends create their entity and own it: EntityHandle's destructor destroys it,
-- so a demo that keeps only the id loses the entity (and every component on it) at the next collect.
-- Every constructor result goes through here.
function Kit.hold(object)
    Kit.keep[#Kit.keep + 1] = object
    return object
end

-- A Camera built from script is a UI camera with a 10-unit far clip (`CameraType::CAMERA_UI` and
-- `DEFAULT_ORTHO_FAR` in engine/core/component/CameraComponent.h:37,63), so a 3D scene rendered
-- through it comes out flat and clipped to ten metres. Nothing in the engine fixes that up for a
-- scene assembled in Lua, so every 3D demo has to ask for a perspective projection. The aspect is
-- the current window's and autoResize goes back on right after, because setPerspective turns it off
-- and a resized window would otherwise keep the ratio of the one it started in.
function Kit.camera3D(camera, yfov, nearValue, farValue)
    local width, height = 0, 0
    pcall(function()
        width = Engine.getScreenWidth()
        height = Engine.getScreenHeight()
    end)
    local aspect = (width > 0 and height > 0) and (width / height) or 1.7778
    camera:setPerspective(number(yfov, 60.0), aspect, number(nearValue, 0.1), number(farValue, 400.0))
    camera.autoResize = true
    return camera
end

-- Joint3D is bound as a class of its own, not derived from EntityHandle, so a joint created from a
-- script cannot answer `.entity` — the one id the demo needs to hang the Rope script on it. The
-- registry hands user entities out in order, so the probe spawned straight after the joint names the
-- joint's id exactly one below it. In an editor-authored scene this is a non-issue: the Inspector
-- writes the Joint3D component and the engine fills self.entity.
function Kit.joint3D(scene, name)
    local rig = Kit.hold(Joint3D(scene))
    local probe = Kit.spawn(scene, name, 0.0, -1000.0, 0.0)
    return probe - 1, rig
end

-- Lua cannot add a component to an entity, but the physics system exposes the creator that does,
-- so a body is a Transform entity plus this call plus a shape.
function Kit.addBody(scene, id)
    local physics = scene:getPhysicsSystem()
    physics:createBody3D(id)
    local body = Body3D(scene, id)
    Kit.keep[#Kit.keep + 1] = body
    return body
end

-- The body type must be chosen before loadBody3D: Body3D:setType calls Jolt SetMotionType with
-- Activate on the already-added body, which asserts in a Debug engine and takes the process down.
-- Pass false for a static body, BodyType.KINEMATIC for a moving platform, nothing for dynamic.
function Kit.addBoxBody(scene, id, width, height, depth, bodyType)
    local body = Kit.addBody(scene, id)
    if bodyType == nil then
        body.type = BodyType.DYNAMIC
    elseif type(bodyType) == "boolean" then
        body.type = bodyType and BodyType.DYNAMIC or BodyType.STATIC
    else
        body.type = bodyType
    end
    body:createBoxShape(number(width, 1.0), number(height, 1.0), number(depth, 1.0))
    scene:getPhysicsSystem():loadBody3D(id)
    return body
end

-- Mirrors LuaBinding's instance setup: defaults from the properties table, then the overrides the
-- demo passes, then init, then the opt-in events the module defines.
function Kit.attach(module, scene, entity, overrides)
    local instance = setmetatable({}, { __index = module })
    instance.scene = scene
    instance.entity = entityId(entity) or entity

    for _, property in ipairs(rawget(module, "properties") or {}) do
        if property.default ~= nil then
            instance[property.name] = property.default
        end
    end
    for key, value in pairs(overrides or {}) do
        instance[key] = value
    end

    if rawget(module, "init") then
        instance:init()
    end
    -- A component that subscribes from its own init records it in _registeredEvents. Subscribing
    -- twice would run its handler twice per step, and RegisterEngineEvent cannot report a duplicate.
    local subscribed = instance._registeredEvents or {}
    if rawget(module, "onFixedUpdate") and not subscribed.onFixedUpdate then
        RegisterEngineEvent(instance, "onFixedUpdate")
    end
    if rawget(module, "onUpdate") and not subscribed.onUpdate then
        RegisterEngineEvent(instance, "onUpdate")
    end

    Kit.keep[#Kit.keep + 1] = instance
    Kit.count = Kit.count + 1
    return instance
end

-- The installer puts the library in the project's Components/, and a demo run from the source tree
-- reads it next to this folder, so both layouts resolve to one module.
function Kit.module(name)
    local ok, loaded = pcall(require, "Components." .. name)
    if ok and type(loaded) == "table" then
        return loaded
    end
    return require(name)
end

-- Demo entities are named so a component slot can fall back to Scene:findEntity; this is how the
-- demo asserts that the content it built is really in the scene.
function Kit.find(scene, name)
    return entityId(scene:findEntity(name))
end

function Kit.report(title)
    print(string.format("[demo] %s: %d library components running, %d scene handles kept alive",
        title or "kit", Kit.count, #Kit.keep))
end

-- A demo has to prove it is still ticking without a human watching, so the driver prints the state
-- every few seconds and closes on ESC or after the frame limit an automated run asks for.
-- KIT_DEMO_FRAMES is how a headless run bounds a demo it does not want to edit first.
function Kit.watch(title, seconds, frameLimit)
    local requested = number(frameLimit, 0)
    if requested <= 0 then
        local ok, fromEnvironment = pcall(function() return tonumber(os.getenv("KIT_DEMO_FRAMES")) end)
        requested = (ok and number(fromEnvironment, 0)) or 0
    end
    local driver = {
        name = "KitDemoWatch",
        every = number(seconds, 3.0),
        limit = requested,
        frames = 0,
        since = 0.0
    }
    driver.onUpdate = function()
        driver.frames = driver.frames + 1
        driver.since = driver.since + number(Engine.deltatime, 0.0)
        if driver.since >= driver.every then
            driver.since = 0.0
            print(string.format("[demo] %s still running, frame %d", title or "kit", driver.frames))
        end
        if driver.limit > 0 and driver.frames >= driver.limit then
            Kit.report(title)
            System.quit()
        end
    end
    RegisterEngineEvent(driver, "onUpdate")

    local closer = { name = "KitDemoCloser", onKeyDown = function(_, key)
        if key == Input.KEY_ESCAPE then
            Kit.report(title)
            System.quit()
        end
    end }
    RegisterEngineEvent(closer, "onKeyDown")

    Kit.keep[#Kit.keep + 1] = driver
    Kit.keep[#Kit.keep + 1] = closer
    return driver
end

return Kit
