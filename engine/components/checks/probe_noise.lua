-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- The contract engine/core/util/Noise.h promises a script: pure functions of (coordinates, seed), so
-- every base stays in [-1,1], a field is continuous across the lattice, the same seed returns the same
-- number on every call, and the fractal sums keep the range instead of growing with the octave count.
-- It also proves the Lua surface is wired: the NoiseType rows, the dispatch helpers, the seed guards
-- and the octave clamp.
--   quantum-engine.exe --project <dir> --scene checks/probe_noise --window 320x240
-- Nothing is drawn, so no scene content is built: Kit.start only waits until the bindings exist.
local Kit = require("demos.kit_demo")

local checks = 0
local failed = 0
local function check(label, ok, detail)
    checks = checks + 1
    if not ok then failed = failed + 1 end
    print(string.format("[noise] %-38s %s   %s", label, ok and "PASS" or "FAIL", detail or ""))
end

local SEED = 1337
local OTHER = 4242
local SAMPLES = 3000

-- Off-lattice coordinates: a sample exactly on a lattice corner is zero for every seed, which would
-- make the seed test pass a field that ignores its seed.
local function coordinates(i)
    return i * 0.137 + 0.019, i * 0.071 + 0.031
end

local function rangeOf(call, count)
    local lo, hi = math.huge, -math.huge
    for i = 1, count do
        local x, y = coordinates(i)
        local v = call(x, y)
        if v < lo then lo = v end
        if v > hi then hi = v end
    end
    return lo, hi
end

local function inRange(label, call, count)
    local lo, hi = rangeOf(call, count)
    check(label, lo >= -1.0 and hi <= 1.0, string.format("range [%.4f, %.4f]", lo, hi))
    check(label .. " fills it", lo < -0.4 and hi > 0.4, string.format("min %.4f max %.4f", lo, hi))
end

local function continuous(label, call, limit)
    local worst = 0.0
    for i = 1, 600 do
        local x, y = coordinates(i)
        local d = math.abs(call(x + 0.001, y) - call(x, y))
        if d > worst then worst = d end
    end
    check(label, worst < limit, string.format("worst step %.5f", worst))
end

local function seedMoves(label, call)
    local identical = 0
    for i = 1, SAMPLES do
        local x, y = coordinates(i)
        if call(x, y, SEED) == call(x, y, OTHER) then identical = identical + 1 end
    end
    check(label, identical * 10 < SAMPLES, string.format("%d of %d identical", identical, SAMPLES))
end

local function deterministic(label, call)
    local same = true
    for i = 1, 400 do
        local x, y = coordinates(i)
        if call(x, y) ~= call(x, y) then same = false end
    end
    check(label, same, "two calls, one coordinate, one value")
end

local function runChecks()
    if type(Noise) ~= "table" then
        check("Noise is exposed to Lua", false, "global Noise absent - rebuild the engine")
    else
        check("Noise is exposed to Lua", true, "Noise and NoiseType")

        -- The determinism a saved world depends on: same seed, same coordinate, same bits.
        local a = Noise.perlin2(1.25, -3.5, SEED)
        local b = Noise.perlin2(1.25, -3.5, SEED)
        check("same seed returns the same value", a == b, string.format("%.6f == %.6f", a, b))
        deterministic("perlin2 is a pure function", function(x, y) return Noise.perlin2(x, y, SEED) end)
        deterministic("simplex3 is a pure function", function(x, y) return Noise.simplex3(x, y, x * 0.5, SEED) end)

        inRange("perlin2 in [-1,1]", function(x, y) return Noise.perlin2(x, y, SEED) end, SAMPLES)
        inRange("simplex2 in [-1,1]", function(x, y) return Noise.simplex2(x, y, SEED) end, SAMPLES)
        inRange("value2 in [-1,1]", function(x, y) return Noise.value2(x, y, SEED) end, SAMPLES)
        inRange("perlin3 in [-1,1]", function(x, y) return Noise.perlin3(x, y, x * 0.5, SEED) end, 1200)
        inRange("simplex3 in [-1,1]", function(x, y) return Noise.simplex3(x, y, x * 0.5, SEED) end, 1200)
        inRange("value3 in [-1,1]", function(x, y) return Noise.value3(x, y, x * 0.5, SEED) end, 1200)

        continuous("perlin2 is continuous", function(x, y) return Noise.perlin2(x, y, SEED) end, 0.05)
        continuous("simplex2 is continuous", function(x, y) return Noise.simplex2(x, y, SEED) end, 0.05)
        continuous("value2 is continuous", function(x, y) return Noise.value2(x, y, SEED) end, 0.05)

        -- A different seed must give a different field, or seeded generation is a lie.
        seedMoves("a new seed moves perlin2", function(x, y, s) return Noise.perlin2(x, y, s) end)
        seedMoves("a new seed moves simplex2", function(x, y, s) return Noise.simplex2(x, y, s) end)
        seedMoves("a new seed moves value2", function(x, y, s) return Noise.value2(x, y, s) end)

        -- The dispatch helpers have to be the same functions the named ones are.
        local dispatchOk = true
        local namedOk = true
        for i = 1, 200 do
            local x, y = coordinates(i)
            if Noise.sample(NoiseType.PERLIN, x, y, SEED) ~= Noise.perlin2(x, y, SEED) then dispatchOk = false end
            if Noise.sample(NoiseType.SIMPLEX, x, y, SEED) ~= Noise.simplex2(x, y, SEED) then dispatchOk = false end
            if Noise.sample(NoiseType.VALUE, x, y, SEED) ~= Noise.value2(x, y, SEED) then dispatchOk = false end
            if Noise.sample3(NoiseType.SIMPLEX, x, y, x * 0.5, SEED) ~= Noise.simplex3(x, y, x * 0.5, SEED) then namedOk = false end
        end
        check("NoiseType rows select the base", dispatchOk, "sample(2D) matches perlin2/simplex2/value2")
        check("sample3 matches simplex3", namedOk, "3D dispatch")

        -- One octave is the base sample exactly, and rectified one octave is its absolute value: the
        -- proof that the octave loop neither rescales nor re-seeds the first term.
        for _, pair in ipairs({
            { NoiseType.PERLIN, Noise.perlin2 },
            { NoiseType.SIMPLEX, Noise.simplex2 },
            { NoiseType.VALUE, Noise.value2 },
        }) do
            local kind = pair[1]
            local base = pair[2]
            local fbmOk, turbOk = true, true
            for i = 1, 200 do
                local x, y = coordinates(i)
                local one = Noise.fbm(kind, x, y, SEED, 1, 2.0, 0.5)
                if one ~= base(x, y, SEED) then fbmOk = false end
                if Noise.turbulence(kind, x, y, SEED, 1, 2.0, 0.5) ~= math.abs(one) then turbOk = false end
            end
            check("fbm of one octave is the base", fbmOk, "octaves=1, lacunarity=2, gain=0.5")
            check("turbulence is the rectified sum", turbOk, "abs() of the same octave")
        end

        local fbmLo, fbmHi = math.huge, -math.huge
        local turbLo, turbHi = math.huge, -math.huge
        local differs = 0
        for i = 1, 1200 do
            local x, y = coordinates(i)
            local f = Noise.fbm(NoiseType.PERLIN, x, y, SEED, 5, 2.0, 0.5)
            local t = Noise.turbulence(NoiseType.SIMPLEX, x, y, SEED, 5, 2.0, 0.5)
            if f < fbmLo then fbmLo = f end
            if f > fbmHi then fbmHi = f end
            if t < turbLo then turbLo = t end
            if t > turbHi then turbHi = t end
            if f ~= Noise.perlin2(x, y, SEED) then differs = differs + 1 end
        end
        check("fbm keeps the base range", fbmLo >= -1.0 and fbmHi <= 1.0,
            string.format("5 octaves: [%.4f, %.4f]", fbmLo, fbmHi))
        check("turbulence is [0,1]", turbLo >= 0.0 and turbHi <= 1.0,
            string.format("5 octaves: [%.4f, %.4f]", turbLo, turbHi))
        check("octaves add detail", differs > 1000,
            string.format("%d of 1200 differ from the base sample", differs))
        check("fbm3 sums octaves", Noise.fbm3(NoiseType.SIMPLEX, 1.5, 2.25, 3.75, SEED, 1, 2.0, 0.5)
            == Noise.simplex3(1.5, 2.25, 3.75, SEED), "fbm3 of one octave is simplex3")

        -- White noise: bounded, deterministic, and not smooth.
        local hashLo, hashHi = 1.0, 0.0
        local hashSteps = 0.0
        for i = 1, SAMPLES do
            local h = Noise.hash2(i, i * 3 + 7, SEED)
            if h < hashLo then hashLo = h end
            if h > hashHi then hashHi = h end
            local d = math.abs(Noise.hash2(i, 5, SEED) - Noise.hash2(i + 1, 5, SEED))
            if d > hashSteps then hashSteps = d end
        end
        check("hash2 is [0,1] and spread", hashLo >= 0.0 and hashHi <= 1.0 and hashLo < 0.02 and hashHi > 0.98,
            string.format("[%.6f, %.6f]", hashLo, hashHi))
        check("hash2 jumps between cells", hashSteps > 0.5, string.format("largest step %.4f", hashSteps))
        check("hash3 is [0,1]", Noise.hash3(11, 22, 33, SEED) >= 0.0 and Noise.hash3(11, 22, 33, SEED) <= 1.0,
            string.format("%.6f", Noise.hash3(11, 22, 33, SEED)))

        -- The guards: a runaway octave count and a coordinate off the int lattice must cost detail, not
        -- the process, and an unknown base must read flat instead of throwing.
        local clamped = Noise.fbm(NoiseType.PERLIN, 3.25, 1.75, SEED, 100000, 2.0, 0.5)
        check("octaves are clamped", clamped == Noise.fbm(NoiseType.PERLIN, 3.25, 1.75, SEED, Noise.MAX_OCTAVES, 2.0, 0.5),
            string.format("MAX_OCTAVES=%d, value %.6f", Noise.MAX_OCTAVES, clamped))
        check("zero octaves degrade to one", Noise.fbm(NoiseType.PERLIN, 3.25, 1.75, SEED, 0, 2.0, 0.5)
            == Noise.perlin2(3.25, 1.75, SEED), "octaves=0")
        check("an out-of-range coordinate reads flat", Noise.perlin2(1e30, 0.5, SEED) == 0.0,
            string.format("%.6f", Noise.perlin2(1e30, 0.5, SEED)))
        check("an unknown noise type reads flat", Noise.sample(99, 1.5, 1.5, SEED) == 0.0,
            string.format("%.6f", Noise.sample(99, 1.5, 1.5, SEED)))
    end

end

-- Quit from the frame loop, not from here: a quit before the loop starts leaves the runner alive,
-- holding quantum.dll open for whoever builds next. The frame budget is the second lock on that same
-- door, so a scene that never reaches onViewLoaded still ends on its own.
Kit.start(function()
    local driver = { name = "NoiseDriver", frames = 0, done = false, finished = false }
    driver.onUpdate = function()
        driver.frames = driver.frames + 1
        if driver.done then return end
        if not driver.finished and driver.frames < 240 then return end
        driver.done = true
        if not driver.finished then
            check("the probe reached its end", false, string.format("still unfinished after %d frames", driver.frames))
        end
        print(string.format("[noise] ===== %d checks, %d failed =====", checks, failed))
        Kit.report("noise")
        System.quit()
    end
    RegisterEngineEvent(driver, "onUpdate")
    Kit.keep[#Kit.keep + 1] = driver

    -- A raise in the middle would otherwise leave the report half-printed and the runner alive, so the
    -- cut is named here rather than left to the engine's error handler.
    local ok, err = pcall(runChecks)
    if not ok then
        check("the probe reached its end", false, tostring(err))
    end
    driver.finished = true
end)
