-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- FASE 1 probe of WIND_SYSTEM_PLAN.md: the wind is a scene setting, so its five fields have to
-- answer on a fresh scene with the SceneSettings default, survive being set, clamp what cannot be
-- authored, and stay per-scene. Nothing here touches water - the sea state that reads these numbers
-- is FASE 3, and until it exists the only promise this file makes is about storage.
--
-- Run from the repository root:
--   build/Debug/quantum-engine.exe --project tests/wind/probe --window 640x360
--
-- The runtime never reads a .scene file (the editor's Stream is the only decoder, and a shipped
-- build gets its scene from generated C++), so the serializer halves are proven from the shell and
-- documented at the end of this file's output rather than faked here.

local passed = 0
local failed = 0

local function check(name, ok, detail)
    if ok then
        passed = passed + 1
        print("  [PASS] " .. name)
    else
        failed = failed + 1
        print("  [FAIL] " .. name .. (detail ~= nil and (" (got " .. tostring(detail) .. ")") or ""))
    end
end

local function near(a, b)
    return math.abs(a - b) < 1e-5
end

-- The SceneSettings defaults, spelled out here instead of read from the engine: if a default moves,
-- this probe is supposed to fail, because every scene authored before that change would start
-- blowing wind.
local DEFAULT = {
    enabled = false,
    speed = 6.0,
    direction = 0.0,
    gustiness = 0.35,
    change = 0.2,
}

local keep = {}

-- Forward declaration: a `local` name is not in scope above its own statement, and the FASE 2
-- helpers below need the same scene this function builds.
local runFieldChecks

local function run()
    print("========== WIND FASE 1 PROBE: wind as a scene setting ==========")

    local scene = Scene()
    keep[#keep + 1] = scene

    print("-- a fresh scene answers the defaults --")
    check("wind is off", scene.windEnabled == false, scene.windEnabled)
    check("the mean wind is 6 m/s", near(scene.windSpeed, DEFAULT.speed), scene.windSpeed)
    check("it blows along +X", near(scene.windDirection, DEFAULT.direction), scene.windDirection)
    check("gustiness is 0.35", near(scene.windGustiness, DEFAULT.gustiness), scene.windGustiness)
    check("the pattern renews at 0.2 cycles/s", near(scene.windChange, DEFAULT.change), scene.windChange)

    print("-- every field survives being set --")
    scene.windEnabled = true
    scene.windSpeed = 14.5
    scene.windDirection = 210.0
    scene.windGustiness = 0.8
    scene.windChange = 1.25

    check("enabled round-trips", scene.windEnabled == true, scene.windEnabled)
    check("speed round-trips", near(scene.windSpeed, 14.5), scene.windSpeed)
    check("direction round-trips", near(scene.windDirection, 210.0), scene.windDirection)
    check("gustiness round-trips", near(scene.windGustiness, 0.8), scene.windGustiness)
    check("change round-trips", near(scene.windChange, 1.25), scene.windChange)

    print("-- what cannot be authored is clamped, not stored --")
    scene.windSpeed = -5.0
    check("a negative wind speed is calm air", near(scene.windSpeed, 0.0), scene.windSpeed)

    scene.windGustiness = 3.0
    check("gustiness over 1 is capped at 1", near(scene.windGustiness, 1.0), scene.windGustiness)

    scene.windGustiness = -2.0
    check("negative gustiness is steady air", near(scene.windGustiness, 0.0), scene.windGustiness)

    scene.windChange = -1.0
    check("a negative renewal rate never renews", near(scene.windChange, 0.0), scene.windChange)

    -- A heading is an angle, not a magnitude: 720 is the same wind as 0, and folding it here would
    -- make the value the editor shows disagree with the value the scene stores.
    scene.windDirection = 720.0
    check("a direction is stored as authored", near(scene.windDirection, 720.0), scene.windDirection)

    print("-- wind belongs to a scene, not to the process --")
    local other = Scene()
    keep[#keep + 1] = other
    check("a second scene is still calm", other.windEnabled == false, other.windEnabled)
    check("and still at its default speed", near(other.windSpeed, DEFAULT.speed), other.windSpeed)

    -- Switching the wind off has to keep the authored numbers, or a scene whose wind is disabled
    -- and re-enabled would come back calm.
    scene.windSpeed = 8.0
    scene.windEnabled = false
    check("disabling keeps the authored speed", near(scene.windSpeed, 8.0), scene.windSpeed)
    scene.windEnabled = true
    check("and re-enabling finds it again", near(scene.windSpeed, 8.0), scene.windSpeed)

    print(string.format("WIND_FASE1 speed=%.6f direction=%.6f gustiness=%.6f change=%.6f enabled=%s",
        scene.windSpeed, scene.windDirection, scene.windGustiness, scene.windChange,
        tostring(scene.windEnabled)))

    runFieldChecks()

    -- The sea state is stepped by the renderer, so FASE 3 has to run across frames. It owns the
    -- result line and the exit.
    startSeaProbe()
end

-- FASE 2: the field the settings turn into a wind. Everything here is a pure function of
-- (settings, point, seconds), so the printed hash is comparable across two runs of the process.
local function fieldSequence(scene, seconds, count)
    local sum, text, min, max = 0.0, {}, math.huge, -math.huge
    for i = 0, count - 1 do
        local t = seconds * i / count
        local v = Wind.sample(scene, Vector2(37.0, -11.0), t)
        local s = math.sqrt(v.x * v.x + v.y * v.y)
        sum = sum + s
        if s < min then min = s end
        if s > max then max = s end
        text[#text + 1] = string.format("%.4f", s)
    end
    return min, max, sum / count, table.concat(text, ",")
end

local function fnv1a(text)
    local h = 2166136261
    for i = 1, #text do
        h = h ~ text:byte(i)
        h = (h * 16777619) & 0xFFFFFFFF
    end
    return h
end

runFieldChecks = function()
    print("-- FASE 2: the wind field --")
    check("the Wind namespace is bound", type(Wind) == "table" and type(Wind.sample) == "function")

    local scene = Scene()
    keep[#keep + 1] = scene

    scene.windEnabled = false
    local off = Wind.sample(scene, Vector2(0.0, 0.0), 3.0)
    check("no wind, no air movement", near(off.x, 0.0) and near(off.y, 0.0), off.x)

    scene.windEnabled = true
    scene.windGustiness = 0.0
    scene.windSpeed = 9.0
    local steadyMin, steadyMax, steadyMean = fieldSequence(scene, 60.0, 1000)
    check("steady air is the mean wind at every sample",
        near(steadyMin, 9.0) and near(steadyMax, 9.0), steadyMin .. ".." .. steadyMax)
    check("and its mean is the authored speed", near(steadyMean, 9.0), steadyMean)

    scene.windGustiness = 1.0
    scene.windChange = 0.2
    local min, max, mean, seq = fieldSequence(scene, 60.0, 1000)
    check("a gusty day goes under the mean", min < 9.0, min)
    check("and over it", max > 9.0, max)
    check("it never goes negative", min >= 0.0, min)
    check("gusting does not change the mean wind the scene authored", math.abs(mean - 9.0) < 0.6, mean)

    -- The gust has to travel: the pattern is sampled at point + dir * t * speed, so a point pushed
    -- downwind by exactly how far the air moved in the same time reads the same gust. With the
    -- renewal off the two agree to the digit, which is what separates a squall crossing a lake from
    -- every cell blinking on and off together.
    scene.windChange = 0.0
    local dt = 4.0
    local here = Wind.speed(scene, Vector2(12.0, -8.0), 11.0)
    local rad = Wind.direction(scene)
    local ahead = Vector2(12.0 + math.cos(rad) * scene.windSpeed * dt,
                          -8.0 - math.sin(rad) * scene.windSpeed * dt)
    local there = Wind.speed(scene, ahead, 11.0 + dt)
    check("the same gust arrives downwind instead of appearing there", near(here, there),
        here .. " vs " .. there)

    local a = Wind.speed(scene, Vector2(100.0, 40.0), 7.0)
    local b = Wind.speed(scene, Vector2(100.5, 40.0), 7.0)
    check("half a metre does not jump the wind", math.abs(a - b) < 0.1 * scene.windSpeed,
        a .. " vs " .. b)

    -- The veer is bounded by WIND_VEER_MAX in Wind.cpp: a squall line turns the air, it does not
    -- come from behind. atan2(-z, x) is the same reading Wind.direction answers in.
    local gusty = Wind.sample(scene, Vector2(-5.0, 25.0), 5.0)
    local veer = math.atan(-gusty.y, gusty.x) - rad
    if veer > math.pi then veer = veer - 2.0 * math.pi end
    if veer < -math.pi then veer = veer + 2.0 * math.pi end
    check("a gust swings the heading, but not by more than it should", math.abs(veer) <= 0.36,
        math.deg(veer))

    print(string.format("WIND_FASE2 min=%.6f max=%.6f mean=%.6f hash=%d", min, max, mean, fnv1a(seq)))
end

-- FASE 3: the sea the wind raises. The renderer steps it, so this runs as a state machine across
-- frames and Engine.scene has to be the scene holding the water.
--
-- Every promise is about a *settled* number - the fixed point the integrator walks toward - or about
-- one duration compared against another. That is deliberate: a headless run's frame time is whatever
-- the machine made, so an absolute "after 18 seconds" is not a fact two runs can share, while the
-- value the sea converges to, and the ratio between how fast it rises and how fast it falls, are.
local G = 9.81
local TWO_PI = 6.283185307179586
local MIN_FETCH = 18000.0
local SETTLE = 0.005

-- The law as RenderSystem.cpp writes it, spelled out here instead of trusted. If the engine's
-- constants move and these do not, the settled numbers stop agreeing and the probe names it.
local function seaTarget(speed, fetch)
    fetch = math.max(fetch, MIN_FETCH)
    local height = math.min(0.0016 * speed * math.sqrt(fetch / G), 0.0246 * speed * speed)
    local xi = G * fetch / (speed * speed)
    local period = math.min(0.72 * xi ^ 0.34 * speed / G, 0.83 * speed)
    return height, G * period * period / TWO_PI
end

local function within(value, target, share)
    return math.abs(value - target) <= target * share
end

local f3 = {
    started = false, done = false, stage = "idle",
    frames = 0, stageFrames = 0, cap = 60000,
    scene = nil, water = nil,
    target = 0, targetLength = 0, lastHeight = 0, monotone = true, rise10 = 0,
    pond6 = 0, strong = 0, ocean = 0, oceanLength = 0, fall10 = 0, rise10Ref = 0,
}

startSeaProbe = function()
    print("-- FASE 3: the sea the wind raises --")

    local scene = Scene()
    keep[#keep + 1] = scene
    -- only the scene being drawn gets its waters advanced
    Engine.scene = scene
    f3.scene = scene

    local camera = Camera(scene)
    keep[#keep + 1] = camera
    camera.position = Vector3(0, 14.0, 70.0)
    camera.target = Vector3(0, 0.0, 0.0)
    camera:setPerspective(50.0, 640.0 / 360.0, 0.1, 4000.0)
    scene:setCamera(camera)

    -- 400 m of open water: a fetch the engine floors at MIN_FETCH, so the numbers below are the
    -- law's and not a coincidence of this pond's size.
    local water = Water(scene)
    keep[#keep + 1] = water
    water.size = Vector2(400.0, 400.0)
    water.waveHeight = 0.2
    water.waveLength = 6.0
    water:load()
    f3.water = water

    scene.windEnabled = false
    scene.windSpeed = 6.0
    scene.windDirection = 0.0
    scene.windGustiness = 0.0
    scene.windChange = 0.0
    scene.backgroundColor = Vector4(0.28, 0.42, 0.58, 1.0)
    water.windInfluence = 1.0
    water.fetch = 0.0

    f3.started = true
    f3.stage = "settle_in"
end

local function enterStage(name)
    f3.stage = name
    f3.stageFrames = 0
    f3.lastHeight = f3.water:getSeaHeight()
    f3.monotone = true
    f3.rise10 = 0
end

local function report(text)
    print(string.format("  [sea] %-9s %8.4f m high, %8.2f m long, wind %6.3f m/s",
        text, f3.water:getSeaHeight(), f3.water:getSeaLength(), f3.water:getSeaWind()))
end

finish = function()
    if f3.done then return end
    f3.done = true
    print(string.format("WIND_FASE3 pond6=%.6f strong=%.6f ocean=%.6f oceanLength=%.6f rise10=%d fall10=%d hash=%d",
        f3.pond6, f3.strong, f3.ocean, f3.oceanLength, f3.rise10Ref, f3.fall10,
        fnv1a(string.format("%.4f,%.4f,%.4f,%.4f", f3.pond6, f3.strong, f3.ocean, f3.oceanLength))))
    print("========== RESULT: " .. passed .. " passed, " .. failed .. " failed ==========")
    System.quit()
end

local function stall(what)
    check(what, false, "stalled at " .. tostring(f3.water:getSeaHeight()) .. " of " .. tostring(f3.target))
    finish()
end

-- one frame of the machine; every stage either waits for the sea to stop moving or gives up
function seaStep()
    if not f3.started or f3.done then return end

    f3.frames = f3.frames + 1
    f3.stageFrames = f3.stageFrames + 1

    local s = f3.scene
    local w = f3.water
    local h = w:getSeaHeight()

    -- a couple of frames for the water to be loaded and drawn before anything is measured
    if f3.stage == "settle_in" then
        if f3.stageFrames >= 4 then enterStage("calm") end
        return
    end

    if f3.stage == "calm" then
        if f3.stageFrames < 90 then return end
        check("with the wind off the sea never wakes", h == 0.0 and w:getSeaLength() == 0.0, h)
        s.windEnabled = true
        w.windInfluence = 0.0
        enterStage("influence")
        return
    end

    if f3.stage == "influence" then
        if f3.stageFrames < 900 then return end
        check("influence 0 leaves the authored waves exactly alone", h == 0.0 and w:getSeaWind() == 0.0, h)
        w.windInfluence = 1.0
        f3.target, f3.targetLength = seaTarget(6.0, 0.0)
        enterStage("rise")
        return
    end

    if f3.stage == "rise" or f3.stage == "strong" or f3.stage == "ocean" then
        if h < f3.lastHeight - 1e-6 then f3.monotone = false end
        f3.lastHeight = h
        if f3.rise10 == 0 and h >= 0.1 * f3.target then f3.rise10 = f3.stageFrames end

        if not within(h, f3.target, SETTLE) then
            if f3.stageFrames <= f3.cap then return end
            stall("the wind builds the sea the law says")
            return
        end

        if f3.stage == "rise" then
            check("a 6 m/s wind settles on the sea the law says", within(h, f3.target, SETTLE),
                h .. " vs " .. f3.target)
            check("with the wavelength the law says", within(w:getSeaLength(), f3.targetLength, 0.01),
                w:getSeaLength() .. " vs " .. f3.targetLength)
            check("it grew without ever going back", f3.monotone)
            check("the wind the water feels is the wind the scene authored", within(w:getSeaWind(), 6.0, 0.01),
                w:getSeaWind())
            f3.pond6 = h
            f3.rise10Ref = f3.rise10
            s.windSpeed = 14.0
            f3.target, f3.targetLength = seaTarget(14.0, 0.0)
            enterStage("strong")
            report("6 m/s")
            return
        end

        if f3.stage == "strong" then
            check("a stiffer wind raises a bigger sea on the same water", h > f3.pond6 * 1.5,
                h .. " vs " .. f3.pond6)
            check("and settles on the stiffer law", within(h, f3.target, SETTLE), h .. " vs " .. f3.target)
            f3.strong = h
            s.windSpeed = 6.0
            w.fetch = 200000.0
            f3.target, f3.targetLength = seaTarget(6.0, 200000.0)
            enterStage("ocean")
            report("14 m/s")
            return
        end

        -- 6 m/s over 200 km: the growth law is past its fully developed ceiling here, so this is
        -- the number that says fetch was read at all - a pond and an ocean cannot settle the same.
        check("the same wind over an ocean raises a different sea than over a pond",
            within(h, f3.target, SETTLE) and h > f3.pond6 * 1.5, h .. " vs " .. f3.target)
        check("and no sea grows past the fully developed one", within(h, 0.0246 * 36.0, SETTLE), h)
        f3.ocean = h
        f3.oceanLength = w:getSeaLength()
        s.windEnabled = false
        f3.fall10 = 0
        enterStage("decay")
        report("ocean")
        return
    end

    if f3.stage == "decay" then
        if h < f3.lastHeight - 1e-6 then f3.monotone = false end
        if h > 0.9 * f3.ocean then
            if f3.stageFrames <= f3.cap then return end
            stall("a sea flattens when the wind drops")
            return
        end
        f3.fall10 = f3.stageFrames
        -- 24 s of falling against 4 s of rising: six, and a fifth of that is what this can promise
        -- across whatever frame times the machine served.
        check("the swell outlives the wind that raised it by minutes, not frames",
            f3.rise10Ref > 0 and f3.fall10 > 3 * f3.rise10Ref,
            "rose 10% in " .. f3.rise10Ref .. " frames, fell 10% in " .. f3.fall10)
        check("the wind the water feels lets go too", w:getSeaWind() < 6.0, w:getSeaWind())
        report("lull")
        finish()
        return
    end
end

-- `local driver = { ... }` would not work: the name is not in scope inside its own initialiser, so
-- the closure would read a global and the guard would never fire.
local driver
driver = { name = "WindFase1Probe", done = false, onViewLoaded = function()
    if driver.done then return end
    driver.done = true
    local ok, err = pcall(run)
    if not ok then
        print("[wind] the probe stopped: " .. tostring(err))
        System.quit()
    end
end, onUpdate = function()
    if not f3.started or f3.done then return end
    local ok, err = pcall(seaStep)
    if not ok then
        print("[wind] the sea state machine stopped: " .. tostring(err))
        finish()
    end
end }
RegisterEngineEvent(driver, "onViewLoaded")
RegisterEngineEvent(driver, "onUpdate")
keep[#keep + 1] = driver
