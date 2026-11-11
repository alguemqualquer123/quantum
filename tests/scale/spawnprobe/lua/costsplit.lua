-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Which part of creating an object costs what. Same scene, five different prices, so the fix
-- lands on the expensive line instead of on a guess:
--   raw      Scene::createEntity, i.e. the entity id bookkeeping alone,
--   sprite   an Object plus one small component,
--   model    an Object plus MeshComponent, which is the fat one,
--   terrain  a component whose struct carries the whole spline field.
-- The point of the raw row is the delta: whatever a MeshComponent costs, it pays on top of the
-- same entity creation a Sprite pays, and the array behind it copies that struct every time it
-- grows.
--   quantum-engine.exe --project tests/scale/spawnprobe --scene costsplit --window 1280x720 --frames 60
pcall(function() io.stdout:setvbuf("no") end)

local function envNumber(name, fallback)
    local ok, raw = pcall(function() return os.getenv(name) end)
    local v = ok and tonumber(raw) or nil
    return (v and v > 0) and v or fallback
end

local N = envNumber("SPLIT_N", 4000)

local keep = {}
local function hold(object)
    keep[#keep + 1] = object
    return object
end

local scene = hold(Scene())
Engine.scene = scene
local camera = hold(Camera(scene))
camera:setPerspective(60.0, 16.0 / 9.0, 0.1, 2000.0)
scene:setCamera(camera)

local function time(label, fn)
    local t = os.clock()
    fn()
    local ms = (os.clock() - t) * 1000.0
    print(string.format("[split] %-10s %6d   %8.2f ms   %.4f ms each", label, N, ms, ms / N))
    return ms
end

time("raw", function()
    for _ = 1, N do hold(scene:createEntity()) end
end)
time("sprite", function()
    for _ = 1, N do hold(Sprite(scene)) end
end)
time("model", function()
    for _ = 1, N do hold(Model(scene)) end
end)
time("light", function()
    for _ = 1, N do hold(Light(scene)) end
end)
time("point", function()
    for _ = 1, N do hold(Points(scene)) end
end)

-- A mesh attached to an already-built model: the pool is warm after the first, so this is the
-- price of the CPU-side geometry, not of the file.
local models = {}
for _ = 1, N do models[#models + 1] = hold(Model(scene)) end
time("attach", function()
    for i = 1, N do models[i]:loadModel("cube.obj") end
end)

local driver = { frames = 0, keep = keep }
driver.onUpdate = function()
    driver.frames = driver.frames + 1
    if driver.frames == 20 then
        print(string.format("[split] entities in scene=%d", #scene:getEntityList()))
        print("[split] done")
    end
end
RegisterEngineEvent(driver, "onUpdate")
hold(driver)
