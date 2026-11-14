-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- What it costs to put objects into a level, and how that cost moves as the level fills up.
-- The user's complaint is a freeze while adding, so the run reports a cost per 1000-object batch
-- instead of one average: a flat column is a constant-price add, a growing column is an O(n) or
-- O(n^2) walk being repeated per object.
--   SPAWN_N=8000 quantum-engine.exe --project tests/scale/spawnprobe --window 1280x720 \
--       --frames 200 --warmup 20 --stats-out <abs>.json
pcall(function() io.stdout:setvbuf("no") end)

local function envNumber(name, fallback)
    local ok, raw = pcall(function() return os.getenv(name) end)
    local v = ok and tonumber(raw) or nil
    return (v and v > 0) and v or fallback
end

local function envNumber0(name, fallback)
    local ok, raw = pcall(function() return os.getenv(name) end)
    local v = ok and tonumber(raw) or nil
    return v or fallback
end

local function envText(name, fallback)
    local ok, raw = pcall(function() return os.getenv(name) end)
    if ok and type(raw) == "string" and raw ~= "" then return raw end
    return fallback
end

local TOTAL = envNumber0("SPAWN_N", 8000)
local BATCH = envNumber("SPAWN_BATCH", 1000)
local MESH = envText("SPAWN_MESH", "cube.obj")
-- The same object built twice in a row costs a pool hit the second time, so the three columns
-- separate the price of an entity from the price of a mesh.
local WARM_LOADS = envNumber0("SPAWN_WARM_LOADS", 1)

local keep = {}
local function hold(object)
    keep[#keep + 1] = object
    return object
end

local checks = 0
local failed = 0
local function check(label, ok, detail)
    checks = checks + 1
    if not ok then failed = failed + 1 end
    print(string.format("[spawn] %-38s %s   %s", label, ok and "PASS" or "FAIL", detail or ""))
end

local scene = hold(Scene())
Engine.scene = scene

local sun = hold(Light(scene))
sun:setType(LightType.DIRECTIONAL)
sun.direction = Vector3(-0.30, -0.85, -0.40)
sun:setIntensity(1.5)

local camera = hold(Camera(scene))
camera:setPerspective(60.0, 16.0 / 9.0, 0.1, 2000.0)
scene:setCamera(camera)
camera.position = Vector3(0, 60, 160)
camera.target = Vector3(0, 0, 0)

-- One mesh parse up front: the pool is cold on the first load of a file and every later entity
-- shares it, so the first object is a different measurement from the other 7999.
local t0 = os.clock()
for _ = 1, WARM_LOADS do
    local warm = hold(Model(scene))
    warm:loadModel(MESH)
end
local warmMs = (os.clock() - t0) * 1000.0

-- Column 1: entities only. This is Scene::createEntity plus the component set a Model asks for.
-- Measured on one batch so the number is the price of an object, not the price of a full scene:
-- the ramp below is what answers how that price moves with the scene.
local UNIT = math.min(TOTAL, BATCH)
local models = {}
local t1 = os.clock()
for i = 1, UNIT do
    models[i] = hold(Model(scene))
end
local entityMs = (os.clock() - t1) * 1000.0

-- Column 2: the mesh on top of the entity. Every one of these hits a warm pool.
local t2 = os.clock()
for i = 1, UNIT do
    models[i]:loadModel(MESH)
end
local loadMs = (os.clock() - t2) * 1000.0

-- Column 3: a place in the world, i.e. the Transform write the editor does when you drop one.
local t3 = os.clock()
for i = 1, UNIT do
    models[i].position = Vector3((i % 100) * 2.0, 1.0, math.floor(i / 100) * 2.0)
end
local placeMs = (os.clock() - t3) * 1000.0

print(string.format("[spawn] warm first loads=%d: %.2f ms (%.2f ms each)", WARM_LOADS, warmMs, warmMs / WARM_LOADS))
print(string.format("[spawn] %d entities      %.2f ms (%.4f ms each)", UNIT, entityMs, entityMs / UNIT))
print(string.format("[spawn] %d mesh attaches %.2f ms (%.4f ms each)", UNIT, loadMs, loadMs / UNIT))
print(string.format("[spawn] %d placements    %.2f ms (%.4f ms each)", UNIT, placeMs, placeMs / UNIT))

check("an entity costs under 0.05 ms", entityMs / UNIT <= 0.05, string.format("%.4f ms", entityMs / UNIT))
check("a warm mesh attach costs under 0.05 ms", loadMs / UNIT <= 0.05, string.format("%.4f ms", loadMs / UNIT))
check("a placement costs under 0.02 ms", placeMs / UNIT <= 0.02, string.format("%.4f ms", placeMs / UNIT))

-- The real scenario: objects created BATCH at a time, one batch per frame, in a scene that keeps
-- growing. A flat column is a constant-price add; a growing one is a walk over the scene that
-- repeats per object, which is the freeze the user sees while placing.
local driver = {
    name = "SpawnDriver", frames = 0, phase = "ramp", done = UNIT, made = 0,
    batch = 0, best = 0, worst = 0, window = 0, ratios = {}, keep = keep, models = models,
}

local function ramp()
    local start = driver.done
    local stop = math.min(start + BATCH, TOTAL)
    local t = os.clock()
    for i = start + 1, stop do
        local m = Model(scene)
        keep[#keep + 1] = m
        m:loadModel(MESH)
        m.position = Vector3((i % 90) * 2.0, 2.0, math.floor(i / 90) * 2.0)
        driver.made = driver.made + 1
    end
    local ms = (os.clock() - t) * 1000.0
    driver.done = stop
    driver.batch = driver.batch + 1
    if driver.batch == 1 then driver.best = ms end
    if ms > driver.worst then driver.worst = ms end
    driver.ratios[driver.batch] = ms
    print(string.format("[spawn] created %d-%d: %.2f ms (%.4f ms each) entities now=%d",
        start + 1, stop, ms, ms / math.max(stop - start, 1), #scene:getEntityList()))
end

-- Steady-state sample once the field is full: the price of *having* them, not of adding them.
local samples = {}
local function collect(dt)
    samples[#samples + 1] = dt
    if #samples < 60 then return end
    local sorted, sum = {}, 0.0
    for i = 1, #samples do sorted[i] = samples[i]; sum = sum + samples[i] end
    table.sort(sorted)
    local avg = sum / #sorted
    print(string.format("[spawn] steady entities=%d avg=%.2f ms p95=%.2f max=%.2f (%.1f fps)",
        #scene:getEntityList(), avg * 1000.0,
        sorted[math.floor(#sorted * 0.95)] * 1000.0, sorted[#sorted] * 1000.0, 1000.0 / math.max(avg, 1e-6)))
    driver.phase = "done"
end

driver.onUpdate = function()
    driver.frames = driver.frames + 1
    local dt = tonumber(Engine.deltatime) or 0.0
    local a = driver.frames * 0.01
    camera.position = Vector3(math.sin(a) * 160, 60, math.cos(a) * 160)
    camera.target = Vector3(0, 0, 0)

    if driver.phase == "ramp" then
        ramp()
        if driver.done >= TOTAL then
            driver.phase = "settle"
            driver.window = 30
            check("adding does not get more expensive as the scene fills",
                driver.worst <= driver.best * 4.0,
                string.format("first batch %.2f ms, worst later %.2f ms over %d batches",
                    driver.best, driver.worst, driver.batch))
        end
    elseif driver.phase == "settle" then
        driver.window = driver.window - 1
        if driver.window <= 0 then driver.phase = "steady" end
    elseif driver.phase == "steady" then
        collect(dt)
    end
end
RegisterEngineEvent(driver, "onUpdate")
hold(driver)
