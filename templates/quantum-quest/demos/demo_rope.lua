-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Real physics, five ways: a swinging rope, a slingshot, a falling crate that hurts, a gravity zone
-- and a grabber that throws. Run it from a project that has the library installed:
--   quantum-engine.exe --project <project> --scene demos/demo_rope --window 1280x720
local Kit = require("demos.kit_demo")

Kit.start(function()
    local scene = Kit.createScene()
    scene.backgroundColor = Vector4(0.34, 0.38, 0.46, 1.0)
    scene.gravity3D = Vector3(0, -9.81, 0)

    local sun = Kit.hold(Light(scene))
    sun.name = "Sun"
    sun:setType(LightType.DIRECTIONAL)
    sun.direction = Vector3(-0.4, -0.8, -0.4)
    sun:setIntensity(1.1)

    local camera = Kit.hold(Camera(scene))
    camera.name = "Camera"
    camera.position = Vector3(0, 8, 30)
    camera.target = Vector3(0, 3, 0)
    scene:setCamera(camera)
    Kit.camera3D(camera, 60, 0.1, 200)
    local shaker = Kit.attach(Kit.module("CameraShake"), scene, camera.entity, { maxTrauma = 0.7 })

    -- A static box is the floor the falling objects land on: Body3D with a shape, no script needed.
    local floorBody = Kit.addBoxBody(scene, Kit.spawn(scene, "Floor", 0, -1, 0), 60, 2, 60, false)
    floorBody.friction = 0.9

    -- 1. Rope. The anchor needs a body of its own, because Jolt only builds a joint between bodies,
    -- and the rope itself lives on the entity that carries the Joint3D component.
    local anchor = Kit.spawn(scene, "Anchor", -12, 9, 0)
    Kit.addBoxBody(scene, anchor, 0.6, 0.6, 0.6, false)
    local ball = Kit.spawn(scene, "Ball", -8, 5, 0)
    local ballBody = Kit.addBoxBody(scene, ball, 1.2, 1.2, 1.2)
    local rigId = select(1, Kit.joint3D(scene, "RopeRig"))
    Kit.attach(Kit.module("Rope"), scene, rigId, {
        anchor = anchor, subject = ball, length = 6.5, damping = 0.02, bounce = 0.25, breakLength = 0
    })
    -- One shove is enough: the joint plus the length clamp keep it swinging.
    ballBody.linearVelocity = Vector3(0, 0, 6.0)

    -- 2. Slingshot. Pull with the mouse and the shot leaves along the camera ray with power times
    -- stretch, scaled by the projectile's own mass.
    local stone = Kit.spawn(scene, "Stone", 10, 2, 0)
    Kit.addBoxBody(scene, stone, 0.5, 0.5, 0.5)
    Kit.attach(Kit.module("Slingshot"), scene, Kit.spawn(scene, "Launcher", 10, 0.6, 0), {
        subject = stone, power = 26, maxStretch = 1.6, useMouse = true,
        camera = camera.entity, autoReturn = true
    })

    -- 3. Impact effect: the preset wires the burst, the sound and the shake in one line, and any
    -- component that holds this instance can fire it with a strength.
    local poof = Kit.hold(Particles(scene))
    poof.name = "ImpactParticles"
    Kit.attach(Kit.module("ParticleBurst"), scene, poof.entity, {
        count = 26, life = 0.7, radius = 0.4, speed = 5.0, size = 0.35, color = { 0.8, 0.7, 0.55 }
    })
    local impact = Kit.attach(Kit.module("ImpactEffect"), scene, Kit.spawn(scene, "ImpactFx", 0, 0, 0), {
        preset = 0, particles = poof.entity, shake = shaker, burstScale = 1.2
    })

    -- 4. Falling object. It sleeps until something comes near or its timer expires, then turns the
    -- landing speed into damage, a camera shake and the impact effect above.
    local rock = Kit.spawn(scene, "Rock", -2, 14, -4)
    Kit.addBoxBody(scene, rock, 1.4, 1.4, 1.4)
    Kit.attach(Kit.module("FallingObject"), scene, rock, {
        wakeDistance = 12, wakeAfter = 3.0, gravityFactor = 1.4, impactDelta = 0.35,
        damagePerSpeed = 4, maxDamage = 60, shake = shaker, impact = impact, impactRadius = 6
    })

    -- 5. Gravity zone: its own pull on top of the scene gravity. Lua cannot enumerate the scene, so
    -- the bodies a zone affects are named in its slots.
    Kit.attach(Kit.module("GravityZone"), scene, Kit.spawn(scene, "Updraft", 6, 3, -8), {
        shape = 0, size = Vector3(8, 10, 8), direction = Vector3(0, 1, 0), strength = 6.0,
        factor = 0.0, targetA = stone, targetB = ball
    })

    -- 6. Grabber: pick from a screen ray, hold with damped impulses, throw on release.
    Kit.attach(Kit.module("PhysicsGrabber"), scene, Kit.spawn(scene, "Grabber", 0, 0, 0), {
        camera = camera.entity, maxDistance = 40, strength = 22, throwOnRelease = true,
        allowUiEvents = false
    })

    Kit.report("rope")
    print("[demo] rope: the mouse drags, throws and stretches the slingshot; ESC closes it")
    Kit.watch("rope", 8.0)
end)
