-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Scale probe: what a frame and a spawn cost once the scene holds N entities. Everything about the
-- shape of the run comes from the environment, so one file draws the whole curve:
--   SCALE_N=5000 quantum-engine.exe --project tests/scale/probe --window 1280x720 \
--       --frames 240 --warmup 60 --name n5000 --stats-out /tmp/n5000.json
-- Three numbers, three different failures:
--   spawnMs   the hitch felt while adding objects to a level,
--   steady ms the fps that are then kept (the camera orbits, so a fully frustum-rejected static
--             view cannot flatter the result),
--   destroyMs what it costs to take them away again.
--
-- SCALE_KIND=model    N Model entities sharing one mesh file (the ordinary case),
-- SCALE_KIND=object   N Transform-only entities (isolates the per-entity loops that run whatever
--                     the entity draws),
-- SCALE_KIND=spread   N models spread over a volume the size of a big terrain, so the cull path is
--                     exercised with most of them outside the frustum.
local function envNumber(name, fallback)
    local ok, raw = pcall(function() return os.getenv(name) end)
    local v = ok and tonumber(raw) or nil
    return (v and v > 0) and v or fallback
end

local function envText(name, fallback)
    local ok, raw = pcall(function() return os.getenv(name) end)
    if ok and type(raw) == "string" and raw ~= "" then return raw end
    return fallback
end

local N = envNumber("SCALE_N", 1000)
local KIND = envText("SCALE_KIND", "model")
local SPACING = envNumber("SCALE_SPACING", 6.0)
local MODEL = envText("SCALE_MODEL", "cube.obj")
local WARMUP = envNumber("SCALE_WARMUP", 45)
local SAMPLES = envNumber("SCALE_SAMPLES", 120)
local DO_DESTROY = envNumber("SCALE_DESTROY", 1) > 0

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
    print(string.format("[scale] %-30s %s   %s", label, ok and "PASS" or "FAIL", detail or ""))
end

local scene = Scene()
Engine.scene = scene
scene.backgroundColor = Vector4(0.08, 0.10, 0.14, 1.0)

local sun = hold(Light(scene))
sun.name = "Sun"
sun:setType(LightType.DIRECTIONAL)
sun.direction = Vector3(-0.30, -0.85, -0.40)
sun:setIntensity(1.5)

local camera = hold(Camera(scene))
camera.name = "ScaleCam"
local width, height = 0, 0
pcall(function()
    width = Engine.getScreenWidth()
    height = Engine.getScreenHeight()
end)
local aspect = (width > 0 and height > 0) and (width / height) or 1.7778
-- A script-built camera defaults to the UI projection with a ten-metre far clip, which would draw
-- almost nothing and measure the wrong cost.
camera:setPerspective(60.0, aspect, 0.1, 6000.0)
camera.autoResize = true
scene:setCamera(camera)

local side = math.floor(math.sqrt(N) + 0.5)
if side < 1 then side = 1 end
local radius = side * SPACING * 1.6
local heightY = side * SPACING * 0.8

local models = {}
local spawned = 0
local loadFailed = 0
local spawnMs = 0.0

local function place(index, object)
    local gx = index % side
    local gz = math.floor(index / side)
    if KIND == "spread" then
        -- A volume instead of a slab: most entities end up behind or above the orbiting camera, so
        -- the cull decision is taken per frame for all of them and only a fraction draws.
        local layer = math.floor(index / (side * side))
        object.position = Vector3((gx - side * 0.5) * SPACING,
            1.0 + layer * SPACING, (gz - side * 0.5) * SPACING)
    else
        object.position = Vector3((gx - side * 0.5) * SPACING, 1.0, (gz - side * 0.5) * SPACING)
    end
end

local t0 = os.clock()
for i = 0, N - 1 do
    if KIND == "object" then
        local object = hold(Object(scene))
        object.name = "T" .. i
        place(i, object)
    elseif KIND == "mesh" then
        -- Object + MeshComponent, no ModelComponent: separates which of the two costs the spawn.
        local mesh = hold(Mesh(scene))
        place(i, mesh)
    elseif KIND == "modelonly" then
        -- The wrapper and its components, without any file: separates the entity-side cost from
        -- the parse/upload cost that the steady-state numbers cannot see.
        local model = hold(Model(scene))
        place(i, model)
        models[#models + 1] = model
    else
        local model = hold(Model(scene))
        if model:loadModel(MODEL) == false then loadFailed = loadFailed + 1 end
        place(i, model)
        models[#models + 1] = model
    end
    spawned = spawned + 1
end
spawnMs = (os.clock() - t0) * 1000.0

camera.position = Vector3(0, heightY, radius)
camera.target = Vector3(0, 0, 0)

print(string.format("[scale] n=%d kind=%s spawn=%.0f ms (%.4f ms/entity) rejected_loads=%d",
    spawned, KIND, spawnMs, spawnMs / math.max(spawned, 1), loadFailed))

check("every entity was built", spawned == N, string.format("%d of %d", spawned, N))
check("the model loads", loadFailed == 0, string.format("%d rejected", loadFailed))

local driver = { name = "ScaleDriver", frames = 0, samples = {}, reported = false, destroying = false }

local destroyMs = 0.0
local destroyErrors = 0
local function destroyAll()
    driver.destroying = true
    local beginClock = os.clock()
    -- Scene derives from EntityRegistry (CoreClassesLua.cpp:955), so the id is what gets destroyed;
    -- the wrapper would destroy it again on its own collect, hence entityOwned goes first.
    for i = #keep, 1, -1 do
        local entry = keep[i]
        if entry ~= scene and entry ~= camera and entry ~= sun and entry ~= driver
            and type(entry.entity) == "number" then
            local ok, err = pcall(function()
                entry.entityOwned = false
                scene:destroyEntity(entry.entity)
            end)
            if not ok then destroyErrors = destroyErrors + 1 end
        end
    end
    destroyMs = (os.clock() - beginClock) * 1000.0
    driver.destroyed = true
end

driver.onUpdate = function()
    if driver.destroyed then return end
    driver.frames = driver.frames + 1
    if driver.frames > WARMUP and not driver.reported then
        local dt = tonumber(Engine.deltatime) or 0.0
        driver.samples[#driver.samples + 1] = dt
        local a = driver.frames * 0.012
        camera.position = Vector3(math.sin(a) * radius, heightY, math.cos(a) * radius)
        camera.target = Vector3(0, 0, 0)
        if #driver.samples >= SAMPLES then
            local sorted = {}
            local sum, min, max = 0.0, math.huge, 0.0
            for i = 1, #driver.samples do
                local s = driver.samples[i]
                sorted[i] = s
                sum = sum + s
                if s < min then min = s end
                if s > max then max = s end
            end
            table.sort(sorted)
            local p95 = sorted[math.max(1, math.floor(#sorted * 0.95))]
            local avg = sum / #sorted
            print(string.format(
                "[scale] steady n=%d samples=%d avg=%.2f ms min=%.2f p95=%.2f max=%.2f (%.1f fps)",
                spawned, #sorted, avg * 1000.0, min * 1000.0, p95 * 1000.0, max * 1000.0,
                1.0 / math.max(avg, 1e-6)))
            check("steady frame holds 30 fps", avg * 1000.0 <= 33.4, string.format("avg %.2f ms", avg * 1000.0))
            check("no frame is a hitch", max * 1000.0 <= 100.0, string.format("worst %.2f ms", max * 1000.0))
            check("spawning is not a hitch", spawnMs <= 1000.0, string.format("%.0f ms", spawnMs))
            driver.reported = true
            if DO_DESTROY then destroyAll() end
        end
    end
    if DO_DESTROY and driver.destroyed and not driver.destroyReported then
        driver.destroyReported = true
        print(string.format("[scale] destroy %.0f entities: %.0f ms (%.4f ms/entity) errors=%d",
            spawned, destroyMs, destroyMs / math.max(spawned, 1), destroyErrors))
        check("destroying is not a hitch", destroyMs <= 1000.0, string.format("%.0f ms", destroyMs))
        print(string.format("[scale] ===== %d checks, %d failed =====", checks, failed))
    end
end
RegisterEngineEvent(driver, "onUpdate")
keep[#keep + 1] = driver
