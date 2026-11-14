-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- One object type per process, so a row costs exactly what its own components cost. The earlier
-- all-in-one probe timed sprite, then model, then light in the same growing scene, so every row
-- carried the previous rows' entities as well and the cheaper type looked dearer than the dearer
-- one. Here the process starts empty, COST_PREFILL entities are added outside the clock, and only
-- the COST_N creations after that are measured - so the same row at two different prefill sizes
-- says how much of the price is scene size and how much is the component itself.
--
--   for row in entity object mesh sprite model light point; do
--     COST_ROW=$row COST_N=2000 COST_PREFILL=0 quantum-engine.exe --project tests/scale/spawnprobe \
--       --scene costrow --window 1280x720 --frames 20
--   done
pcall(function() io.stdout:setvbuf("no") end)

local function envText(name, fallback)
    local ok, raw = pcall(function() return os.getenv(name) end)
    return (ok and raw ~= nil and raw ~= "") and raw or fallback
end

local function envInt(name, fallback)
    local v = tonumber(envText(name, ""))
    return v or fallback
end

local ROW = envText("COST_ROW", "mesh")
local N = envInt("COST_N", 2000)
local PREFILL = envInt("COST_PREFILL", 0)

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

local makers = {
    entity = function() return scene:createEntity() end,
    object = function() return Object(scene) end,
    mesh = function() return Mesh(scene) end,
    sprite = function() return Sprite(scene) end,
    model = function() return Model(scene) end,
    light = function() return Light(scene) end,
    point = function() return Points(scene) end,
}

local make = makers[ROW]
if make == nil then
    print(string.format("[row] unknown COST_ROW=%s (want entity|object|mesh|sprite|model|light|point)", ROW))
    System.quit()
    return
end

-- outside the clock, and rooted the same way so the prefill does not get collected mid-run
for _ = 1, PREFILL do hold(make()) end

local t = os.clock()
for _ = 1, N do hold(make()) end
local ms = (os.clock() - t) * 1000.0

print(string.format("[row] %-7s prefill=%-6d n=%-6d %9.2f ms  %.4f ms each", ROW, PREFILL, N, ms, ms / N))

local driver = { frames = 0, keep = keep }
driver.onUpdate = function()
    driver.frames = driver.frames + 1
    if driver.frames == 12 then
        print(string.format("[row] %-7s entities=%d", ROW, #scene:getEntityList()))
        System.quit()
    end
end
RegisterEngineEvent(driver, "onUpdate")
hold(driver)
