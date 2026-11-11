-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Probe: does the fire system run end to end without an editor? Everything a Fire is made of is
-- checked against one scene - sprite pools, the flickering light, fuel that turns into char, spread
-- that stops at the radius, rain that wins, burn-away - so a regression fails a named line here
-- instead of showing up as "the demo looked odd".
--   quantum-engine.exe --project <project> --scene checks/probe_fire --window 1280x720
local Kit = require("demos.kit_demo")

local function entityId(reference)
    if type(reference) == "number" then return reference end
    if reference and type(reference.entity) == "number" then return reference.entity end
    return reference
end

local expected = {
    "flame pool fills",
    "smoke pool fills",
    "ember pool fills",
    "the light flickers",
    "the fire really added a Light entity",
    "heat reaches the neighbour",
    "distance is measured from the flame",
    "the neighbour lights its own flame",
    "fuel turns into char",
    "rain puts a fire out",
    "fuel runs out",
    "burn-away hides the fuel",
    "burnt fuel cannot be relit",
    "out of radius stays unlit",
    "unflammable refuses the fire",
    "heat hurts the subject",
    "soaked fuel refuses a match",
    "char darkens the Mesh",
    "tint turns itself off with no Mesh"
}
local passed, reported = {}, {}

local function expect(name, condition, detail)
    if passed[name] then return end
    if not condition then return end
    passed[name] = true
    if not reported[name] then
        reported[name] = true
        print(string.format("[probe] PASS %s%s", name, detail and (" - " .. detail) or ""))
    end
end

local function score(name, condition, detail)
    expect(name, condition, detail)
    if not passed[name] then
        reported[name] = true
        print(string.format("[probe] FAIL %s%s", name, detail and (" - " .. detail) or ""))
    end
end

Kit.start(function()
    local scene = Kit.createScene()
    scene.backgroundColor = Vector4(0.05, 0.05, 0.07, 1.0)

    local camera = Kit.hold(Camera(scene))
    camera.name = "Camera"
    camera.position = Vector3(10, 9, 26)
    camera.target = Vector3(2, 1, 0)
    scene:setCamera(camera)
    Kit.camera3D(camera, 60, 0.1, 400)

    Kit.spawn(scene, "Ground", 0, -0.05, 0)

    -- Something standing in the heat with a Health on it, so the damage path is exercised instead of
    -- only configured: deathAction 0 keeps it in the scene for the later checks.
    local dummy = Kit.spawn(scene, "Dummy", 0.6, 0, 0.4)
    local victim = Kit.attach(Kit.module("Health"), scene, dummy, {
        maxHealth = 100.0, deathAction = 0, regenRate = 0.0
    })

    -- The reference fire: full intensity, no fuel limit, and a spread radius that reaches the crate
    -- at 3 m and deliberately not the one at 24 m.
    local fire = Kit.attach(Kit.module("Fire"), scene, Kit.spawn(scene, "Bonfire", 0, 0, 0), {
        intensity = 1.0, flameSize = 1.1, flameHeight = 1.8, spreadRadius = 6.0, spreadInterval = 0.2,
        lightIntensity = 8.0, flicker = 0.5,
        heatDamage = 40.0, damageRadius = 2.2, subject = dummy, health = victim
    })

    local weather = Kit.attach(Kit.module("WeatherSystem"), scene, Kit.spawn(scene, "Weather", 0, 0, 0), {
        type = 0, intensity = 1.0, targetIntensity = 1.0, changeSpeed = 10.0, toggleKey = 0, cycleKey = 0
    })

    local near = Kit.attach(Kit.module("Burnable"), scene, Kit.spawn(scene, "CrateNear", 3, 0, 0), {
        fuel = 30.0, burnRate = 1.0, spreadRadius = 0.0
    })
    local far = Kit.attach(Kit.module("Burnable"), scene, Kit.spawn(scene, "CrateFar", 24, 0, 0), { fuel = 6.0 })
    local stone = Kit.attach(Kit.module("Burnable"), scene, Kit.spawn(scene, "Stone", 1.5, 0, 0), {
        flammability = 0.0, fuel = 6.0
    })
    local away = Kit.attach(Kit.module("Burnable"), scene, Kit.spawn(scene, "CrateAway", 4.5, 0, 0), {
        fuel = 1.2, burnRate = 1.0, burnAway = true, crackle = false
    })

    -- A fire with a fuel clock has to put itself out; a fire under rain has to be put out by it.
    local short = Kit.attach(Kit.module("Fire"), scene, Kit.spawn(scene, "Torch", 0, 0, -14), {
        fuel = 1.0, burnRate = 1.0, flameRate = 60, smoke = false, embers = false, light = false,
        soundPath = ""
    })
    local wet = Kit.attach(Kit.module("Fire"), scene, Kit.spawn(scene, "RainTest", 0, 0, -22), {
        weather = weather, rainExtinguishes = true, flameRate = 60, smoke = false, embers = false,
        light = false, soundPath = ""
    })

    -- Char works through Mesh.color, so the fuel has to sit on an entity that really has the
    -- component; Mesh(scene) is the cheapest one the library can make without an asset. Both of
    -- these are out of the bonfire's spread radius, so they light from their own `ignited` flag.
    local crate = Kit.hold(Mesh(scene))
    crate.name = "CrateMesh"
    crate.position = Vector3(0, 0, 9)
    crate.color = Vector4(0.6, 0.5, 0.4, 1.0)
    local tinct = Kit.attach(Kit.module("Burnable"), scene, crate.entity, {
        ignited = true, fuel = 12.0, burnRate = 1.0, tint = true, spreadRadius = 0.0, crackle = false
    })

    -- The authoring mistake this guards: tint on a Transform-only entity. The first failed read has
    -- to turn the property off, or the ramp would log an engine error on every step of the char.
    local bare = Kit.attach(Kit.module("Burnable"), scene, Kit.spawn(scene, "NoMesh", 0, 0, 12), {
        ignited = true, fuel = 12.0, tint = true, spreadRadius = 0.0, crackle = false
    })

    local levels = {}
    local driver = { name = "FireProbe", frame = 0 }

    function driver:onUpdate()
        self.frame = self.frame + 1

        -- The pool only fills while the action runs, so the first count worth trusting is a few
        -- frames in, and the rain and fuel clocks need seconds, not frames.
        if self.frame == 90 then
            expect("flame pool fills", fire:getParticleCount() > 0,
                string.format("%d sprites live", fire:getParticleCount()))
            expect("smoke pool fills", fire:layerCount(fire.smokeLayer) > 0,
                string.format("%d smoke sprites", fire:layerCount(fire.smokeLayer)))
            expect("ember pool fills", fire:layerCount(fire.emberLayer) > 0,
                string.format("%d embers", fire:layerCount(fire.emberLayer)))
            -- A pcall that stopped halfway through build() would leave the light nil and the flicker
            -- would still print numbers, so the entity itself is what this asserts.
            expect("the fire really added a Light entity",
                fire.lamp ~= nil and Kit.find(scene, "BonfireLight") ~= nil,
                Kit.find(scene, "BonfireLight") and "BonfireLight is in the scene" or "no light entity")
        end
        if self.frame % 20 == 0 and #levels < 12 then levels[#levels + 1] = fire.lightLevel end

        local varied = false
        for i = 2, #levels do
            if levels[i] > 0.0 and math.abs(levels[i] - levels[1]) > 0.05 then varied = true end
        end
        expect("the light flickers", varied,
            string.format("%.2f .. %.2f", levels[1] or 0.0, levels[#levels] or 0.0))

        expect("heat reaches the neighbour", near.lit == true)
        local gap = near:distanceTo(fire:position())
        expect("distance is measured from the flame", math.abs(gap - 3.0) < 0.05, string.format("%.3f m", gap))
        expect("the neighbour lights its own flame",
            near.fire ~= nil and near.fire:getParticleCount() > 0,
            near.fire and string.format("%d sprites", near.fire:getParticleCount()) or nil)
        expect("fuel turns into char", near:getChar() > 0.02, string.format("char %.2f", near:getChar()))
        expect("heat hurts the subject", victim.percent < 0.9,
            string.format("%.1f health left", victim.current))

        -- The read is inside the guard: a passed check must not keep touching the material every
        -- frame, and this probe runs at several times real time.
        if not passed["char darkens the Mesh"] then
            local tinted = nil
            pcall(function() tinted = Mesh(scene, crate.entity).color end)
            expect("char darkens the Mesh",
                tinted ~= nil and tinct:getChar() > 0.3 and tinted.y < 0.45,
                tinted and string.format("green %.2f at char %.2f", tinted.y, tinct:getChar()))
        end
        expect("tint turns itself off with no Mesh", bare.tint == false,
            string.format("char reached %.2f", bare:getChar()))

        if self.frame == 1100 then
            score("out of radius stays unlit", far.lit == false, "checked at frame 1100")
            score("unflammable refuses the fire", stone.lit == false, "checked at frame 1100")
            far:soak(1.0)
            score("soaked fuel refuses a match", far:ignite() == false and far.lit == false,
                string.format("wet %.2f", far:getWet()))
            score("rain puts a fire out", wet:isBurning() == false, "checked at frame 1100")
            score("fuel runs out", short:isBurning() == false, "checked at frame 1100")
            local visible = true
            pcall(function() visible = Object(scene, entityId(away.entity)).visible end)
            score("burn-away hides the fuel", away.spent == true and visible == false,
                string.format("spent=%s visible=%s", tostring(away.spent), tostring(visible)))
            away:igniteFrom(fire)
            score("burnt fuel cannot be relit", away.lit == false and away.spent == true)

            local tally = 0
            for _, name in ipairs(expected) do
                if passed[name] then
                    tally = tally + 1
                elseif not reported[name] then
                    reported[name] = true
                    print("[probe] FAIL " .. name .. " - never happened")
                end
            end
            print(string.format("[probe] fire: %d/%d checks passed - %d sprites on the bonfire, char %.2f",
                tally, #expected, fire:getParticleCount(), near:getChar()))
            print(tally == #expected and "[probe] ALL PASS" or "[probe] FAILED")
            System.quit()
        end
    end

    RegisterEngineEvent(driver, "onUpdate")
    Kit.keep[#Kit.keep + 1] = driver
    print(string.format("[probe] fire: %d library components wired", Kit.count))
    Kit.report("fire")
end)
