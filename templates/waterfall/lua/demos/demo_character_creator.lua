-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Headless proof that an avatar can be assembled from the shipped pack and that its parts move with
-- it: hair, eyebrows and a second hairstyle are hung off the body's "Head" bone, one of them is
-- driven by a world-space spring so it lags a turn and settles again, one hangs off a bone in
-- "follow" mode, and one is detached mid-run to show the difference. Run it with:
--   quantum-engine.exe --project <project> --scene demos/demo_character_creator --window 640x360
-- With QUANTUM_PACK_DIR set to the engine's assets folder it reads the pack where it lives:
--   set QUANTUM_PACK_DIR=C:/quantum2/engine/assets
-- The report goes to CC_PROBE_OUT (default "character_probe.txt" in the working directory) one line
-- at a time, so a run that has to be killed still says how far it got.
local Kit = require("demos.kit_demo")

local outPath = os.getenv("CC_PROBE_OUT") or "character_probe.txt"

local models = {
    body = "characters/human_male.glb",
    hair = "characters/Hair_Long.glb",
    brows = "characters/Eyebrows_Regular.glb",
    alias = "characters/Hair_Buns.glb",
    follow = "characters/Hair_Buzzed.glb",
    loose = "characters/Hair_SimpleParted.glb",
}

-- Same root rule as the pack probe: the variable wins, otherwise the first root that really holds
-- the body model (`assets copy` mirrors the engine tree into the project).
local function packRoot()
    local candidates = { os.getenv("QUANTUM_PACK_DIR"), "assets/engine/assets", "engine/assets" }
    for _, root in ipairs(candidates) do
        if root then
            local path = root:gsub("\\", "/"):gsub("/+$", "") .. "/" .. models.body
            local handle = io.open(path, "rb")
            if handle then
                handle:close()
                return root:gsub("\\", "/"):gsub("/+$", "")
            end
        end
    end
    return (candidates[2] or "engine/assets"):gsub("\\", "/"):gsub("/+$", "")
end

local root = packRoot()

local failures = 0
local lines = {}

local function log(line)
    print(line)
    lines[#lines + 1] = line
    local handle = io.open(outPath, "a")
    if handle then
        handle:write(line, "\n")
        handle:close()
    end
end

do
    local fresh = io.open(outPath, "w")
    if fresh then fresh:close() end
end

local function verdict(ok, label, detail)
    if ok then
        log(string.format("[avatar] ok   %-10s %s", label, detail))
    else
        failures = failures + 1
        log(string.format("[avatar] FAIL %-10s %s", label, detail))
    end
    return ok
end

local function angle(degrees)
    if Engine.useDegrees then
        return degrees
    end
    return math.rad(degrees)
end

local function distance(a, b)
    return math.sqrt((a.x - b.x) ^ 2 + (a.y - b.y) ^ 2 + (a.z - b.z) ^ 2)
end

local scene, body, headBone
local parts = {}

-- findEntityInParent hands back a wrapper for a hit and NULL_ENTITY for a miss, and every read below
-- wants the plain id, so both shapes collapse here.
local function idOf(reference)
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

Kit.start(function()
    scene = Kit.createScene()
    scene.backgroundColor = Vector4(0.13, 0.15, 0.19, 1.0)

    local camera = Kit.hold(Camera(scene))
    camera.name = "Camera"
    camera.position = Vector3(0, 1.4, 2.6)
    camera.target = Vector3(0, 1.2, 0)
    scene:setCamera(camera)
    Kit.camera3D(camera, 55, 0.1, 100)

    local sun = Kit.hold(Light(scene))
    sun.name = "Sun"
    sun:setType(LightType.DIRECTIONAL)
    sun:setIntensity(2.2)

    body = Kit.hold(Model(scene))
    body.name = "Player"
    pcall(function() body:loadModel(root .. "/" .. models.body) end)

    local function part(key, name, file)
        local model = Kit.hold(Model(scene))
        model.name = name
        pcall(function() model:loadModel(root .. "/" .. file) end)
        parts[key] = model
        return model
    end

    part("hair", "Hair", models.hair)
    part("brows", "Eyebrows", models.brows)
    part("alias", "AliasHair", models.alias)
    part("follow", "FollowHair", models.follow)
    part("loose", "LooseHair", models.loose)

    log("[avatar] reading the pack from " .. root)

    local AvatarPart = Kit.module("AvatarPart")
    local HairSwing = Kit.module("HairSwing")

    -- `hair` carries no offset, so its world position must equal the bone's every frame: that is
    -- the attachment proof. `brows` hangs at an offset and is proved by the distance staying fixed.
    parts.hairDriver = Kit.attach(AvatarPart, scene, parts.hair.entity, {
        body = body.entity, bodyName = "Player", bone = "Head", retryAfter = 0.25
    })
    parts.browsDriver = Kit.attach(AvatarPart, scene, parts.brows.entity, {
        body = body.entity, bodyName = "Player", bone = "Head",
        offset = { 0.0, 0.04, 0.03 }, rotationOffset = { 0.0, 0.0, 3.0 }, retryAfter = 0.25
    })
    -- The wrong name has to fall through to the alias list, which is what makes one component work
    -- on every rig regardless of how its head bone happens to be spelled.
    parts.aliasDriver = Kit.attach(AvatarPart, scene, parts.alias.entity, {
        body = body.entity, bodyName = "Player", bone = "NotABone",
        aliases = "stillWrong, Head", retryAfter = 0.25
    })
    parts.followDriver = Kit.attach(AvatarPart, scene, parts.follow.entity, {
        body = body.entity, bodyName = "Player", bone = "Head", mode = "follow",
        offset = { 0.0, 0.05, 0.0 }, retryAfter = 0.25
    })
    parts.looseDriver = Kit.attach(AvatarPart, scene, parts.loose.entity, {
        body = body.entity, bodyName = "Player", bone = "Head", retryAfter = 0.25
    })

    -- droop and wind off for the swing that is measured: a pure spring has to come back to the rest
    -- pose, and only that verdict can be read as "it lagged and then settled".
    parts.hairSwing = Kit.attach(HairSwing, scene, parts.hair.entity, {
        stiffness = 42.0, damping = 5.0, droop = 0.0, windStrength = 0.0,
        maxAngle = 40.0, startAfter = 0.3
    })
    parts.browsSwing = Kit.attach(HairSwing, scene, parts.brows.entity, {
        stiffness = 10.0, damping = 3.0, droop = 6.0, maxAngle = 20.0, startAfter = 0.3
    })

    headBone = idOf(scene:findEntityInParent("Head", body.entity))
    log(string.format("[avatar] head bone resolved to %s", tostring(headBone or "none")))
end)

-- Phase boundaries read as a clock range. An earlier cut of this probe swapped elements of the phase
-- table while looking for the active one, which silently rewrote the timeline it was measuring.
local ENDS = { 0.6, 1.35, 2.7, 4.2, 4.9 }
local NAMES = { "settle", "spin", "hold", "dance", "sweep", "report" }

local ZERO = { x = 0.0, y = 0.0, z = 0.0 }

local function phaseAt(clock)
    for index, limit in ipairs(ENDS) do
        if clock < limit then return NAMES[index] end
    end
    return NAMES[#NAMES]
end

local function positionOf(id)
    if not id then return nil end
    local found = false
    local position
    pcall(function()
        position = Object(scene, id):getWorldPosition()
        found = true
    end)
    return found and { x = position.x, y = position.y, z = position.z } or nil
end

local driver = {
    name = "CharacterProbe",
    phase = "waiting",
    clock = 0.0,
    yaw = 0.0,
    frames = 0,
    attachedFrames = 0,
    trackError = 0.0,
    followLag = 0.0,
    followRigid = 0.0,
    headTravel = 0.0,
    assembledKeys = {},
    followArm = nil,
    offsetError = 0.0,
    offsetBase = nil,
    peak = 0.0,
    tail = nil,
    bounded = true,
    prev = nil,
    detached = false,
    assembled = false,
    looseGap = 0.0,
    animated = false
}

-- The body's nodes only exist once the asynchronous glTF load has finished, so the bone every
-- verdict is measured against has to be re-resolved in the loop instead of once in the builder.
local function resolveHead()
    if headBone or not body then return headBone end
    headBone = idOf(scene:findEntityInParent("Head", body.entity))
    return headBone
end

-- Head, hair, follow part, brows and the loose part are sampled every frame, and each verdict below
-- is a difference between two samples of the same pair: a part that is not really hung off the bone
-- drifts, and the drift is exactly what gets reported.
local function sample()
    local head = positionOf(headBone)
    local hair = parts.hair and positionOf(parts.hair.entity)
    local follow = parts.follow and positionOf(parts.follow.entity)
    local brows = parts.brows and positionOf(parts.brows.entity)
    local loose = parts.loose and positionOf(parts.loose.entity)

    -- The first comparison is only valid once the attach itself has stopped moving things.
    local headTravel = 0.0
    local headStep = nil
    if head and driver.prev and driver.prev.head then
        headStep = {
            x = head.x - driver.prev.head.x,
            y = head.y - driver.prev.head.y,
            z = head.z - driver.prev.head.z
        }
        headTravel = distance(headStep, ZERO)
        if headTravel > driver.headTravel then driver.headTravel = headTravel end
    end

    if driver.attachedFrames > 2 and head and hair and driver.prev and driver.prev.head and driver.prev.hair then
        local hairStep = {
            x = hair.x - driver.prev.hair.x,
            y = hair.y - driver.prev.hair.y,
            z = hair.z - driver.prev.hair.z
        }
        -- "attach" reparents under the bone with no offset, so the two steps have to be one vector.
        local track = distance(headStep, hairStep)
        if track > driver.trackError then driver.trackError = track end

        if driver.prev.follow and follow then
            local followStep = {
                x = follow.x - driver.prev.follow.x,
                y = follow.y - driver.prev.follow.y,
                z = follow.z - driver.prev.follow.z
            }
            -- "follow" writes a world position carried on a rotating arm, so the step vectors differ
            -- by construction; what has to match is how far each of them travelled this frame.
            local lag = math.abs(distance(followStep, ZERO) - distance(headStep, ZERO))
            if lag > driver.followLag then driver.followLag = lag end
        end
    end

    -- Both offset paths are proved the same way: a rigid attachment keeps a constant distance to the
    -- bone however the body turns, animates or translates.
    if driver.attachedFrames > 2 and head and brows then
        local separation = distance(brows, head)
        if not driver.offsetBase then
            driver.offsetBase = separation
        else
            local error = math.abs(separation - driver.offsetBase)
            if error > driver.offsetError then driver.offsetError = error end
        end
    end

    -- The follow arm is proved against the bone's *previous* frame position, which is exactly what the
    -- copy reads: "follow" copies the bone's world transform during the script phase, before the
    -- render system has refreshed it, so on a moving body the part sits one step behind by
    -- construction. Testing the arm against that stale source keeps the claim honest and still fails
    -- if the offset is lost, doubled or applied in the wrong space.
    if driver.attachedFrames > 2 and follow then
        local arm = nil
        if driver.prev and driver.prev.head then
            arm = distance(follow, driver.prev.head)
        end
        if arm then
            if not driver.followArm then
                driver.followArm = arm
            else
                local error = math.abs(arm - driver.followArm)
                if error > driver.followRigid then driver.followRigid = error end
            end
        end
    end

    -- Before the detach this sits at zero (the part is on the bone); after it, the body keeps moving
    -- and the abandoned part cannot.
    if head and loose then
        local gap = distance(loose, head)
        if gap > driver.looseGap then driver.looseGap = gap end
    end

    driver.prev = { head = head, hair = hair, follow = follow }
end

local function report()
    local keys = { "hair", "brows", "alias", "follow", "loose" }
    local attachedCount = 0
    for _, key in ipairs(keys) do
        local instance = parts[key .. "Driver"]
        if instance and instance.attached then attachedCount = attachedCount + 1 end
    end

    -- Counted over the whole run, not at report time: the sweep phase detaches one part on purpose,
    -- and that is proved by its own verdict below, not by making this one fail.
    local hung = 0
    for _, key in ipairs(keys) do
        if driver.assembledKeys[key] then hung = hung + 1 end
    end
    verdict(hung == #keys and headBone ~= nil, "attach",
        string.format("%d of %d parts hung off the body's head bone (%s)",
            hung, #keys, tostring(parts.hairDriver and parts.hairDriver.boneName)))

    verdict(parts.aliasDriver and parts.aliasDriver.attached
            and parts.aliasDriver.boneName == "Head",
        "alias",
        string.format("\"NotABone\" fell through to %q",
            tostring(parts.aliasDriver and parts.aliasDriver.boneName)))

    verdict(driver.trackError < 0.01, "track",
        string.format("hair/head per-frame step differed by at most %.5f m while the body turned, "
            .. "animated and translated", driver.trackError))

    verdict(driver.offsetBase ~= nil and driver.offsetError < 0.01, "offset",
        string.format("eyebrows held %.4f m from the head, drift at most %.5f m",
            driver.offsetBase or 0.0, driver.offsetError))

    -- "follow" reads the bone's cached world transform during the script phase, which the render
    -- system has not refreshed yet, so it can only ever be one frame behind; the arm length is what
    -- has to stay exact.
    verdict(driver.followArm ~= nil and driver.followRigid < 0.01
            and driver.followLag <= driver.headTravel + 0.01, "follow",
        string.format("mode=follow held %.4f m off the bone's previous frame position (drift %.5f m) "
            .. "and trailed it by at most %.5f m per frame, one step of the %.5f m the head travelled",
            driver.followArm or 0.0, driver.followRigid, driver.followLag, driver.headTravel))

    verdict(driver.peak > 4.0 and (driver.tail or 99.0) < driver.peak * 0.35, "swing",
        string.format("peak deviation %.1f degrees, settled to %.1f degrees",
            driver.peak, driver.tail or -1.0))

    verdict(driver.bounded, "droop",
        string.format("the 20 degree cap held: eyebrow spring peaked at %.1f degrees",
            parts.browsSwing and parts.browsSwing:deviation() or -1.0))

    verdict(driver.looseGap > 0.05, "detach",
        string.format("after detach the abandoned part fell %.3f m behind the head",
            driver.looseGap))

    log(string.format("[avatar] %d of %d parts still hung on the bone at report time, %d failures",
        attachedCount, #keys, failures))
    log(string.format("[avatar] %s", failures == 0 and "PASS" or "FAIL"))
    Kit.report("character creator")
    System.quit()
end

driver.onUpdate = function()
    driver.frames = driver.frames + 1
    if not scene or not body or not parts.hair then return end

    if not resolveHead() then
        if driver.frames > 900 then
            verdict(false, "attach", "the loaded body never produced a Head bone to hang parts off")
            log("[avatar] FAIL")
            System.quit()
        end
        return
    end

    local attached = true
    for _, key in ipairs({ "hair", "brows", "alias", "follow", "loose" }) do
        local instance = parts[key .. "Driver"]
        if not instance or not instance.attached then attached = false end
    end
    for _, key in ipairs({ "hair", "brows", "alias", "follow", "loose" }) do
        local instance = parts[key .. "Driver"]
        if instance and instance.attached then driver.assembledKeys[key] = true end
    end
    if attached then
        -- Latched, because the sweep phase deliberately detaches the loose part: re-running this
        -- gate afterwards would send the loop back to the waiting branch and the report would never
        -- be reached.
        driver.assembled = true
    elseif not driver.assembled then
        if driver.frames > 900 then
            local missing = {}
            for _, key in ipairs({ "hair", "brows", "alias", "follow", "loose" }) do
                local instance = parts[key .. "Driver"]
                if not instance or not instance.attached then
                    missing[#missing + 1] = string.format("%s(bone=%s,tried=%s,released=%s)", key,
                        tostring(instance and instance.boneName), tostring(instance and instance.triedFrames),
                        tostring(instance and instance.released))
                end
            end
            verdict(false, "attach", "parts never hung off the head bone: " .. table.concat(missing, " "))
            log("[avatar] FAIL")
            System.quit()
        end
        return
    end

    local dt = math.min(tonumber(Engine.deltatime) or (1.0 / 60.0), 0.05)
    driver.clock = driver.clock + dt
    local name = phaseAt(driver.clock)
    driver.phase = name

    -- A yaw about the body's own axis is what excites the spring: HairSwing reads the bone's world
    -- rotation, not its position, so the spin phase has to be rotation and the sweep translation.
    if name == "spin" then
        driver.yaw = driver.yaw + 420.0 * dt
        body.rotation = Quaternion(0.0, angle(driver.yaw), 0.0)
    elseif name == "dance" and not driver.animated then
        driver.animated = true
        pcall(function() body:playAnimation("Dance_Loop", 1.0) end)
    elseif name == "sweep" then
        if not driver.detached then
            driver.detached = true
            pcall(function() parts.looseDriver:detach() end)
        end
        -- Continuous and starting at rest: a teleport would measure the one-frame stale read of
        -- follow mode as if it were a tracking failure.
        local elapsed = driver.clock - ENDS[4]
        body.position = Vector3(math.sin(elapsed * 2.5) * 0.8, 0.0, elapsed * elapsed * 1.0)
    end

    sample()

    local swingAngle = parts.hairSwing and parts.hairSwing:deviation() or 0.0
    if swingAngle > driver.peak then driver.peak = swingAngle end
    if name == "hold" and driver.clock > 2.2 then
        driver.tail = swingAngle
    end
    if parts.browsSwing and (parts.browsSwing:deviation() or 0.0) > 24.0 then
        driver.bounded = false
    end

    driver.attachedFrames = driver.attachedFrames + 1
    if name == "report" then
        report()
    end
end

RegisterEngineEvent(driver, "onUpdate")
Kit.keep[#Kit.keep + 1] = driver
