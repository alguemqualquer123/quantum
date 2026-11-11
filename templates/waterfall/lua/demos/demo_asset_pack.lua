-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Headless proof that the asset pack the engine ships really loads. One model per category and the
-- four maps of one ground material go through the engine's own loaders, and every submesh material
-- is then checked for a base-colour texture, so "installed" is not taken on faith from a folder
-- listing. Run it with:
--   quantum-engine.exe --project <project> --scene demos/demo_asset_pack --window 640x360
-- With QUANTUM_PACK_DIR set to the engine's assets folder it reads the pack where it lives:
--   set QUANTUM_PACK_DIR=C:/quantum2/engine/assets
-- Without it, it looks for the copy that `quantum-editor-cmd assets copy` leaves in the project
-- (assets/engine/assets) and falls back to engine/assets.
--
-- The report goes to PACK_PROBE_OUT (default "pack_probe.txt" in the working directory) as well as
-- to stdout, and one line at a time: a redirected stdout is block-buffered, so a run that has to be
-- killed leaves nothing behind - and the line it got to is exactly the answer.
local Kit = require("demos.kit_demo")

local outPath = os.getenv("PACK_PROBE_OUT") or "pack_probe.txt"

-- One asset per category, chosen so that a failure names the category that broke.
local models = {
    { category = "foliage", file = "foliage/grass_medium_01.glb" },
    { category = "trees", file = "trees/island_tree_02.glb" },
    { category = "weapons", file = "weapons/service_pistol.glb" },
    { category = "characters", file = "characters/human_male.glb" },
}

-- QUANTUM_PACK_DIR wins, otherwise the first root that actually holds a pack file: `assets copy`
-- mirrors the engine tree into the project, so a project sees the pack under assets/engine.
local function packRoot()
    local candidates = { os.getenv("QUANTUM_PACK_DIR"), "assets/engine/assets", "engine/assets" }
    local probe = models[1].file
    for _, root in ipairs(candidates) do
        if root then
            local path = root:gsub("\\", "/"):gsub("/+$", "") .. "/" .. probe
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

local maps = {
    "materials/ground/ground-grass-color.jpg",
    "materials/ground/ground-grass-normal-gl.png",
    "materials/ground/ground-grass-roughness.jpg",
    "materials/ground/ground-grass-height.png",
}

local loaded = {}
local textures = {}
local failures = 0
local built = false

local function log(line)
    print(line)
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

-- A material counts as textured when its base-colour slot holds pixels, not just a name.
local function isTextured(material)
    local ok, textured = pcall(function()
        local map = material.baseColorTexture
        return map ~= nil and not map:empty()
    end)
    return ok and textured == true
end

Kit.start(function()
    local scene = Kit.createScene()
    scene.backgroundColor = Vector4(0.12, 0.14, 0.18, 1.0)

    local camera = Kit.hold(Camera(scene))
    camera.name = "Camera"
    camera.position = Vector3(0, 4, 12)
    camera.target = Vector3(0, 0, 0)
    scene:setCamera(camera)
    Kit.camera3D(camera, 60, 0.1, 500)

    local sun = Kit.hold(Light(scene))
    sun.name = "Sun"
    sun:setType(LightType.DIRECTIONAL)
    sun:setIntensity(2.0)

    log("[pack] reading the pack from " .. root)

    for _, entry in ipairs(models) do
        local path = root .. "/" .. entry.file
        log("[pack] loading " .. entry.category .. " " .. path)
        local model = Kit.hold(Model(scene))
        local ok, err = pcall(function() model:loadModel(path) end)
        if not ok then
            failures = failures + 1
            log(string.format("[pack] FAIL %-10s %s: %s", entry.category, entry.file, tostring(err)))
        else
            local submeshes = model:getNumSubmeshes() or 0
            loaded[#loaded + 1] = { entry = entry, model = model, submeshes = submeshes }
            if submeshes == 0 then
                failures = failures + 1
                log(string.format("[pack] FAIL %-10s %s loaded no submesh", entry.category, entry.file))
            end
        end
    end

    -- Texture(path) is file-backed, so the load is queued and the frame counter below decides when
    -- the answer is real; asking one frame later would report every map as empty.
    for _, file in ipairs(maps) do
        local map = Kit.hold(Texture(root .. "/" .. file))
        pcall(function() map:load() end)
        textures[#textures + 1] = { file = file, map = map }
    end

    built = true
    log(string.format("[pack] %d models and %d ground maps queued, waiting for the loader",
        #loaded, #textures))
end)

local driver = { name = "PackProbe", frames = 0 }
driver.onUpdate = function()
    driver.frames = driver.frames + 1
    if driver.frames < 120 then
        return
    end

    if not built then
        log("[pack] FAIL the scene builder never ran (no onViewLoaded, no model was loaded)")
        System.quit()
        return
    end

    for _, entry in ipairs(loaded) do
        local textured = 0
        for index = 0, (entry.submeshes - 1) do
            local ok, material = pcall(function() return entry.model:getMaterial(index) end)
            if ok and material and isTextured(material) then
                textured = textured + 1
            end
        end
        local verdict = textured > 0 and "ok  " or "FAIL"
        if textured == 0 then
            failures = failures + 1
        end
        log(string.format("[pack] %s %-10s %-28s submeshes=%2d textured=%d/%d",
            verdict, entry.entry.category, entry.entry.file, entry.submeshes, textured, entry.submeshes))
    end

    for _, entry in ipairs(textures) do
        local width, height = 0, 0
        pcall(function() width, height = entry.map:getWidth(), entry.map:getHeight() end)
        local verdict = width > 0 and "ok  " or "FAIL"
        if width <= 0 then
            failures = failures + 1
        end
        log(string.format("[pack] %s %-10s %-44s %dx%d",
            verdict, "ground", entry.file, width, height))
    end

    log(string.format("[pack] %d models and %d ground maps checked, %d failures",
        #loaded, #textures, failures))
    System.quit()
end
RegisterEngineEvent(driver, "onUpdate")
Kit.keep[#Kit.keep + 1] = driver
