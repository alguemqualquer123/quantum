-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- The other half of the insertion question: what a scene pays to *lose* an entity.
-- ComponentArray::remove erases the slot and copy-assigns every later element down over the hole,
-- so for a 48 KB MeshComponent the cost of deleting one entity is the number of entities still
-- behind it in the array. This times a create pass and a destroy pass in the same process, which is
-- what the editor does when an object is added and then undone.
--
--   COST_TYPE=mesh COST_N=2000 quantum-engine.exe --project tests/scale/spawnprobe \
--     --scene costdel --window 1280x720 --frames 20
pcall(function() io.stdout:setvbuf("no") end)

local function envText(name, fallback)
    local ok, raw = pcall(function() return os.getenv(name) end)
    return (ok and raw ~= nil and raw ~= "") and raw or fallback
end

local function envInt(name, fallback)
    return tonumber(envText(name, "")) or fallback
end

local TYPE = envText("COST_TYPE", "mesh")
local N = envInt("COST_N", 2000)

local scene = nil
local keep = {}
local function hold(value)
    keep[#keep + 1] = value
    return value
end

local makers = {
    entity = function() return scene:createEntity() end,
    object = function() return Object(scene) end,
    mesh = function() return Mesh(scene) end,
    model = function() return Model(scene) end,
    light = function() return Light(scene) end,
}

scene = hold(Scene())
Engine.scene = scene
local camera = hold(Camera(scene))
camera:setPerspective(60.0, 16.0 / 9.0, 0.1, 2000.0)
scene:setCamera(camera)

local make = makers[TYPE]
if make == nil then
    print(string.format("[del] unknown COST_TYPE=%s (want entity|object|mesh|model|light)", TYPE))
    System.quit()
    return
end

local ids = {}
local t = os.clock()
for _ = 1, N do
    local item = hold(make())
    -- the raw id, so the destroy pass can go through the scene the way the editor does
    ids[#ids + 1] = (type(item) == "number") and item or item.entity
end
local createMs = (os.clock() - t) * 1000.0

-- Backwards removes from the tail, which is the cheap end of the shift; forwards always takes the
-- first slot and shifts the whole array, so the two together bound what a real deletion costs.
local backward = envText("COST_DEL_BACKWARD", "1") ~= "0"
t = os.clock()
if backward then
    for i = #ids, 1, -1 do
        local id = ids[i]
        if id ~= nil and id ~= 0 then
            scene:destroyEntity(id)
        end
    end
else
    for i = 1, #ids do
        local id = ids[i]
        if id ~= nil and id ~= 0 then
            scene:destroyEntity(id)
        end
    end
end
local destroyMs = (os.clock() - t) * 1000.0

print(string.format("[del] %-7s n=%-6d %s create %8.2f ms (%.4f each)  destroy %8.2f ms (%.4f each)  left=%d",
    TYPE, N, backward and "backward" or "forward", createMs, createMs / N, destroyMs, destroyMs / N,
    #scene:getEntityList()))
local driver = { frames = 0 }
driver.onUpdate = function()
    driver.frames = driver.frames + 1
    if driver.frames == 12 then
        System.quit()
    end
end
RegisterEngineEvent(driver, "onUpdate")
hold(driver)
