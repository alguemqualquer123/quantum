-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- FASE 2 and FASE 4 probe of the World Creator plan: a procedurally generated 16-bit heightmap
-- becomes a real CDLOD terrain, the map the Lua API writes is byte-for-byte the map the CLI wrote,
-- and that terrain arrives covered - the procedural material set baked into the three blend maps the
-- shader already reads, with no paint stroke and no downloaded byte.
--
-- The scene is built here rather than authored in a .scene file, for the same reason the waterfall
-- template builds its own: every number in it (map size, elevation range, sea plane, band thresholds)
-- has to agree with the generator's parameters, and one file that holds both cannot drift.
--
-- Reproduce a run from the repository root:
--   quantum-editor-cmd terrain generate --out tests/worldcreator/probe/assets/textures/heightmap_generated.png \
--       --preset island --size 512 --seed 7313 --erosion-seed 99
--   quantum-engine.exe --project tests/worldcreator/probe --window 1280x720 \
--       --screenshot <TEMP>/fase4_terrain.png --capture-frame 140
--
-- Two path spellings appear on purpose. WorldGen.generate and io.open are filesystem calls resolved
-- against the running project directory, so they say "assets/textures/..."; a component texture path
-- is relative to the asset root, so the same file is "textures/...".

local keep = {}
local function hold(object)
    keep[#keep + 1] = object
    return object
end

local passed = 0
local failed = 0

local function check(name, ok, detail)
    if ok then
        passed = passed + 1
        Log.print("  [PASS] " .. name)
    else
        failed = failed + 1
        Log.print("  [FAIL] " .. name .. (detail ~= nil and (" (" .. tostring(detail) .. ")") or ""))
    end
end

local function readBytes(path)
    local file = io.open(path, "rb")
    if not file then return nil end
    local bytes = file:read("*a")
    file:close()
    return bytes
end

-- IHDR starts at byte 17 (8 signature + 4 length + 4 type), so the width is 17..20, the height 21..24,
-- the bit depth is byte 25 and the colour type 26. The colour type is the whole claim here: 0 is
-- grayscale, 2 is RGB, and a map that lands as the wrong one is a map the renderer will misread.
local function pngHeader(bytes)
    if bytes == nil or bytes:sub(1, 4) ~= "\137PNG" then return nil end
    local function be(base)
        return bytes:byte(base) * 16777216 + bytes:byte(base + 1) * 65536
            + bytes:byte(base + 2) * 256 + bytes:byte(base + 3)
    end
    return { width = be(17), height = be(21), depth = bytes:byte(25), colour = bytes:byte(26) }
end

local SEED = 7313
local SIZE = 512
local TERRAIN_SIZE = 400.0
local MAX_HEIGHT = 60.0
-- Two path spellings per map on purpose. WorldGen.generate and io.open are filesystem calls resolved
-- against the running project directory, so they say "assets/textures/..."; a component texture path
-- is relative to the asset root, so the same file is "textures/...".
local CLI_MAP = "assets/textures/heightmap_generated.png"
local LUA_MAP = "assets/textures/heightmap_probe.png"
local REROLL_MAP = "assets/textures/heightmap_reroll.png"
local OTHER_SEED_MAP = "assets/textures/heightmap_seed2.png"
local CLI_MAP_IN_SCENE = "textures/heightmap_generated.png"
local LUA_MAP_IN_SCENE = "textures/heightmap_probe.png"

-- The FASE 4 surface: the material set the plan asks for, and the blend maps baked from the map the
-- terrain is wearing. The directory spellings are filesystem paths, the bases are what a component
-- stores, and the two differ by the project's assets directory.
local MATERIAL_DIR = "assets/textures/materials"
local MATERIAL_BASE = "textures/materials"
local REROLL_DIR = "assets/textures/materials_reroll"
local REROLL_BASE = "textures/materials_reroll"
local BLEND_DIR = "assets/textures/blend"
local BLEND_BASE = "textures/blend"
local OTHER_SEED_DIR = "assets/textures/materials_seed2"
local OTHER_SEED_BASE = "textures/materials_seed2"
local MATERIAL_SIZE = 256
local BLEND_RESOLUTION = 512

local surface = { entries = {}, paths = {}, rerollEntries = {} }

local function optionsFor(out, seed)
    return { out = out, preset = "island", seed = seed, size = SIZE, erosionSeed = 99 }
end

local scene
local camera
local terrain
local sea

-- What the bare heightmap implies as a surface: the nine materials the plan asks for, and the three
-- blend maps baked from the map the terrain is wearing. It runs at view load because the layer list
-- has to exist before the first frame asks the shader for the wide terrain variant.
local function setupSurface()
    local island = WorldGen.params("island")
    local plan = WorldGen.splatPlan()

    local ok, err, count, entries = WorldGen.writeMaterials({
        out = MATERIAL_DIR, kinds = plan, size = MATERIAL_SIZE, seed = SEED, pathBase = MATERIAL_BASE })
    surface.materialsOk, surface.materialsErr, surface.count = ok, err, count
    surface.entries = entries or {}

    if ok then
        for index, entry in ipairs(entries) do
            local layer = TerrainSurfaceLayer()
            -- Without pbr a layer is the historical colour-only path, where the texture's own alpha is
            -- the blending height. These are full glTF maps, so they ask for the wide variant.
            layer.pbr = true
            layer.colorFactor = Vector4(1, 1, 1, 1)
            layer.colorTexture = Texture(entry.baseColor)
            layer.normalTexture = Texture(entry.normal)
            layer.roughnessTexture = Texture(entry.metallicRoughness)
            layer.metallicTexture = Texture(entry.metallicRoughness)
            layer.occlusionTexture = Texture(entry.occlusion)
            layer.heightTexture = Texture(entry.height)
            terrain:setSurfaceLayer(index - 1, layer)
        end
    end

    -- The water line the generator damped the bed below, and the component's own numbers: a slope is
    -- nothing without the world scale it was measured against.
    local blendOk, blendErr, paths = WorldGen.bakeSplat({
        heightmap = LUA_MAP, out = BLEND_DIR, pathBase = BLEND_BASE,
        resolution = BLEND_RESOLUTION, maxHeight = MAX_HEIGHT, terrainSize = TERRAIN_SIZE,
        seaLevel = island.seaLevel, seed = SEED })
    surface.blendOk, surface.blendErr, surface.paths = blendOk, blendErr, paths

    if blendOk then
        for index = 1, #paths do
            terrain:setBlendMapIndex(index - 1, paths[index])
        end
        terrain:setBlendMapIndex(1, "textures/blend/black_all.png")
        terrain:setBlendMapIndex(2, "textures/blend/black_all.png")
    end

    -- The same two materials again, asked for in a different order and without the other eight: the
    -- bytes have to agree, or a seed would name a position in a request instead of a material.
    local rerollOk, _, _, rerollEntries = WorldGen.writeMaterials({
        out = REROLL_DIR, kinds = {"rock", "sand"}, size = MATERIAL_SIZE, seed = SEED,
        pathBase = REROLL_BASE })
    surface.rerollOk, surface.rerollEntries = rerollOk, rerollEntries or {}
end

local builder = {}
builder.name = "WorldCreatorProbe"

function builder.onViewLoaded()
    scene = hold(Scene())
    Engine.scene = scene
    scene.backgroundColor = Vector4(0.05, 0.08, 0.14, 1.0)
    -- Water mirrors the sky through envColor = skyColour x globalIlluminationIntensity, and the
    -- setting's 0.1 default leaves the sea ink-black under a bright dome.
    scene.globalIlluminationIntensity = 0.55

    local sun = hold(Light(scene))
    sun.name = "Sun"
    sun:setType(LightType.DIRECTIONAL)
    sun.direction = Vector3(-0.35, -0.85, 0.39)
    sun:setIntensity(1.35)

    local sky = hold(SkyBox(scene))
    sky.name = "Sky"
    sky.color = Vector4(0.42, 0.60, 0.85, 1.0)

    -- The camera goes in before the terrain: a Lua-built terrain with no scene camera goes through
    -- MeshSystem::resetTerrainToUnbuilt, which clears needUpdateTerrain for good.
    camera = hold(Camera(scene))
    camera.name = "Camera"
    camera.position = Vector3(0.0, 120.0, -330.0)
    camera.target = Vector3(0.0, 25.0, 0.0)
    scene:setCamera(camera)
    -- A Camera built from script is a UI camera with a 10-unit far clip, so a 3D scene needs an
    -- explicit perspective projection.
    camera:setPerspective(55.0, 1280.0 / 720.0, 0.5, 3000.0)
    camera.autoResize = true

    terrain = hold(Terrain(scene))
    terrain.name = "Terrain"
    terrain.size = TERRAIN_SIZE
    terrain.maxHeight = MAX_HEIGHT
    terrain.resolution = 64
    terrain.levels = 8
    -- Whatever weight the layers leave unclaimed is painted with the mesh's own colour, so this is the
    -- seabed and any texel the rules did not reach - not the whole surface, which is FASE 4's job.
    terrain.color = Vector4(1.0, 0.0, 0.0, 1.0)

    -- The Lua half of the FASE 2 determinism claim needs the map on disk before the splat can read
    -- it, and the checks below compare it against the CLI's bytes.
    WorldGen.generate(optionsFor(LUA_MAP, SEED))
    terrain:setHeightMap(LUA_MAP_IN_SCENE)

    setupSurface()

    sea = hold(Water(scene))
    sea.name = "Sea"
    sea.position = Vector3(0.0, 0.34 * MAX_HEIGHT, 0.0)
    sea.size = Vector2(1400.0, 1400.0)
    sea.subdivisions = 96
    sea.shallowColor = Vector3(0.25, 0.50, 0.55)
    sea.deepColor = Vector3(0.02, 0.10, 0.18)
    sea.depthFade = 3.0
    sea.shoreFoam = 0.05
end

-- The checks run a few frames after the build, once the async heightmap load has landed and the
-- terrain has actually been built.
local frames = 0
local ran = false

function builder.onUpdate(dt)
    if ran or scene == nil then return end
    frames = frames + 1
    if frames < 4 then return end
    ran = true
    runChecks()
end

RegisterEngineEvent(builder, "onViewLoaded")
RegisterEngineEvent(builder, "onUpdate")

function runChecks()
    Log.print("========== WORLD CREATOR FASE 2 AND 4 PROBE ==========")

    check("WorldGen namespace exists", type(WorldGen) == "table")
    check("WorldGen.generate is a function", type(WorldGen.generate) == "function")
    check("WorldGen.presets is a function", type(WorldGen.presets) == "function")
    check("WorldGen.params is a function", type(WorldGen.params) == "function")
    check("WorldGen.waterHeight is a function", type(WorldGen.waterHeight) == "function")

    local presets = WorldGen.presets()
    check("five presets are named", #presets == 5, #presets)
    local hasIsland = false
    for _, name in ipairs(presets) do
        if name == "island" then hasIsland = true end
    end
    check("the island preset is one of them", hasIsland)

    local island = WorldGen.params("island")
    check("params reads a preset as a table", type(island) == "table")
    check("the island preset uses simplex", island.noise == "simplex", island.noise)
    check("the island preset has sea level 0.34", math.abs(island.seaLevel - 0.34) < 1e-6, island.seaLevel)
    check("the island preset erodes", island.droplets == 6000, island.droplets)
    check("the island preset is an island", island.island == 1.0, island.island)
    check("an unknown preset name still answers", WorldGen.params("nope").noise == "simplex")

    -- waterHeight is the one number a scene needs and cannot guess: where the sea plane sits.
    local byName = WorldGen.waterHeight("island", MAX_HEIGHT)
    local byTable = WorldGen.waterHeight(island, MAX_HEIGHT)
    check("waterHeight of a preset name is seaLevel x maxHeight",
        math.abs(byName - 0.34 * MAX_HEIGHT) < 1e-3, byName)
    check("waterHeight of a params table agrees", math.abs(byTable - byName) < 1e-6, byTable)

    -- The binding must refuse to guess a destination rather than silently write nowhere.
    local refused, reason = WorldGen.generate({ preset = "island" })
    check("generate without an out path is refused", refused == false)
    check("generate says why it was refused", type(reason) == "string" and #reason > 0, reason)

    local badArg, badReason = WorldGen.generate(7)
    check("generate of a non-table is refused", badArg == false)
    check("generate of a non-table explains itself", type(badReason) == "string" and #badReason > 0)

    local ok, err, size = WorldGen.generate(optionsFor(LUA_MAP, SEED))
    check("generate wrote the map", ok == true, err)
    check("generate reports the size it actually wrote", size == SIZE, size)

    -- The CLI wrote CLI_MAP with the same seed before this run started. Two entry points, one
    -- algorithm, so the determinism claim of quantum::Noise has to hold across the boundary.
    local cliBytes = readBytes(CLI_MAP)
    local luaBytes = readBytes(LUA_MAP)
    check("the CLI map is on disk", cliBytes ~= nil, CLI_MAP)
    check("the Lua map is on disk", luaBytes ~= nil, LUA_MAP)
    if cliBytes and luaBytes then
        check("both maps have the same length", #cliBytes == #luaBytes, #cliBytes .. " vs " .. #luaBytes)
        check("the same seed gives the same bytes from Lua and from the CLI", cliBytes == luaBytes)
        check("the map is a PNG", luaBytes:sub(1, 4) == "\137PNG")
        -- IHDR starts at byte 17 (8 signature + 4 length + 4 type), so the width is 17..20, the bit
        -- depth is byte 25 and the colour type is byte 26.
        check("the map is 16-bit", luaBytes:byte(25) == 16, luaBytes:byte(25))
        check("the map is grayscale", luaBytes:byte(26) == 0, luaBytes:byte(26))
        check("the map is SIZE texels wide", luaBytes:byte(20) == SIZE % 256
            and luaBytes:byte(19) == math.floor(SIZE / 256), luaBytes:byte(19) * 256 + luaBytes:byte(20))
    end

    -- A second write of the same seed must not drift, which is what makes a seed a save-game field,
    -- and a different seed must actually change the map.
    WorldGen.generate(optionsFor(REROLL_MAP, SEED))
    WorldGen.generate(optionsFor(OTHER_SEED_MAP, SEED + 1))
    local rerollBytes = readBytes(REROLL_MAP)
    local otherBytes = readBytes(OTHER_SEED_MAP)
    check("a re-run of the same seed is identical", luaBytes ~= nil and rerollBytes == luaBytes)
    check("a different seed gives a different map",
        luaBytes ~= nil and otherBytes ~= nil and otherBytes ~= luaBytes)

    Log.print("========== the scene side ==========")

    check("the engine has a scene to draw", Engine.getScene() ~= nil)
    local terrainId = scene:findEntity("Terrain")
    check("a script-built entity is findable by name", terrainId ~= nil and terrainId ~= 0, terrainId)
    check("the terrain wrapper reads its size", terrain.size == TERRAIN_SIZE, terrain.size)
    check("the terrain wrapper reads its maxHeight",
        math.abs(terrain.maxHeight - MAX_HEIGHT) < 1e-3, terrain.maxHeight)
    check("the terrain resolution is the requested value", terrain.resolution == 64, terrain.resolution)
    check("the terrain has LOD levels", terrain.levels >= 6, terrain.levels)

    -- Re-pointing the live terrain at the map Lua just wrote is what a World Creator panel does on
    -- every brush stroke: the CDLOD grid stays, only the height texture is re-uploaded.
    terrain:setHeightMap(LUA_MAP_IN_SCENE)
    check("the terrain still builds after a heightmap swap", terrain:createTerrain() == true)

    sea:setPosition(0.0, byTable, 0.0)
    check("the sea plane sits at the computed water height",
        math.abs(sea:getWorldPosition().y - byTable) < 0.01, sea:getWorldPosition().y)

    Log.print("========== quantum::Noise from Lua ==========")

    local r1 = Noise.ridged(NoiseType.SIMPLEX, 12.5, -4.25, SEED, 4, 2.0, 0.5)
    local r2 = Noise.ridged(NoiseType.SIMPLEX, 12.5, -4.25, SEED, 4, 2.0, 0.5)
    local rOther = Noise.ridged(NoiseType.SIMPLEX, 12.5, -4.25, SEED + 1, 4, 2.0, 0.5)
    check("Noise.ridged answers", type(r1) == "number", r1)
    check("Noise.ridged is in [0, 1]", r1 >= 0.0 and r1 <= 1.0, r1)
    check("Noise.ridged is deterministic", r1 == r2)
    check("Noise.ridged depends on the seed", r1 ~= rOther)
    local ridged3 = Noise.ridged3(NoiseType.PERLIN, 1.5, 2.5, 3.5, SEED, 4, 2.0, 0.5)
    check("Noise.ridged3 answers in range",
        type(ridged3) == "number" and ridged3 >= 0.0 and ridged3 <= 1.0, ridged3)

    local plain = Noise.fbm(NoiseType.SIMPLEX, 12.5, -4.25, SEED, 4, 2.0, 0.5)
    local warped = Noise.warpedFbm(NoiseType.SIMPLEX, 12.5, -4.25, SEED, 4, 2.0, 0.5, 1.2, 0.35)
    local warpedAgain = Noise.warpedFbm(NoiseType.SIMPLEX, 12.5, -4.25, SEED, 4, 2.0, 0.5, 1.2, 0.35)
    check("warpedFbm moves the sample off the plain fbm", plain ~= warped, plain .. " vs " .. warped)
    check("warpedFbm is deterministic", warped == warpedAgain)
    local warped3 = Noise.warpedFbm3(NoiseType.SIMPLEX, 1.5, 2.5, 3.5, SEED, 4, 2.0, 0.5, 1.2, 0.35)
    check("Noise.warpedFbm3 answers", type(warped3) == "number", warped3)
    -- Zero amplitude has to be the identity: the warp must not be a hidden cost when unused.
    check("warpedFbm with no warp equals fbm",
        math.abs(Noise.warpedFbm(NoiseType.SIMPLEX, 12.5, -4.25, SEED, 4, 2.0, 0.5, 0.0, 0.35) - plain) < 1e-5)

    Log.print("========== FASE 4: materials and the automatic splat ==========")

    check("WorldGen.materials is a function", type(WorldGen.materials) == "function")
    check("WorldGen.splatPlan is a function", type(WorldGen.splatPlan) == "function")
    check("WorldGen.writeMaterials is a function", type(WorldGen.writeMaterials) == "function")
    check("WorldGen.splatRules is a function", type(WorldGen.splatRules) == "function")
    check("WorldGen.bakeSplat is a function", type(WorldGen.bakeSplat) == "function")

    local materials = WorldGen.materials()
    check("the library names ten material families", #materials == 10, #materials)

    local named = {}
    for _, name in ipairs(materials) do named[name] = true end
    local plan = WorldGen.splatPlan()
    check("the plan fills the terrain's nine slots", #plan == 9, #plan)
    local planIsLibrary = true
    for _, name in ipairs(plan) do
        if not named[name] then planIsLibrary = false end
    end
    check("every slot of the plan is a material the library can build", planIsLibrary)
    check("slot 0 is the beach the water line asks for", plan[1] == "sand", plan[1])
    check("slot 8 is the lava rock", plan[9] == "volcanic", plan[9])

    local defaults = WorldGen.splatRules()
    check("splatRules answers a table", type(defaults) == "table")
    check("the default water line is 0.28", math.abs(defaults.seaLevel - 0.28) < 1e-6, defaults.seaLevel)
    check("the default cap leaves the base material a share",
        math.abs(defaults.maxWeight - 0.9) < 1e-6, defaults.maxWeight)
    check("lava is off until a map asks for it", defaults.volcanic == 0.0, defaults.volcanic)
    check("the default blend resolution is a texture size", defaults.resolution == 512, defaults.resolution)

    local noTable, noTableWhy = WorldGen.writeMaterials(7)
    check("writeMaterials of a non-table is refused", noTable == false)
    check("and says why", type(noTableWhy) == "string" and #noTableWhy > 0, noTableWhy)
    local noOut, noOutWhy = WorldGen.writeMaterials({ kinds = {"sand"} })
    check("writeMaterials without an out is refused", noOut == false, noOutWhy)
    local badKind, badKindWhy = WorldGen.writeMaterials({ out = MATERIAL_DIR, kinds = {"notamaterial"} })
    check("an unknown material name is refused", badKind == false, badKindWhy)
    check("and names the one it did not recognise",
        type(badKindWhy) == "string" and badKindWhy:find("notamaterial") ~= nil, badKindWhy)
    local noMap, noMapWhy = WorldGen.bakeSplat({ out = BLEND_DIR })
    check("bakeSplat without a heightmap is refused", noMap == false, noMapWhy)
    local noDir, noDirWhy = WorldGen.bakeSplat({ heightmap = LUA_MAP })
    check("bakeSplat without an out is refused", noDir == false, noDirWhy)
    local badMap, badMapWhy = WorldGen.bakeSplat({ heightmap = "assets/textures/nope.png", out = BLEND_DIR })
    check("bakeSplat of a missing map is refused", badMap == false, badMapWhy)

    check("the nine plan materials were written", surface.materialsOk == true, surface.materialsErr)
    check("one entry per material", surface.count == 9, surface.count)
    check("the blend maps were baked", surface.blendOk == true, surface.blendErr)
    check("three blend paths came back", type(surface.paths) == "table" and #surface.paths == 3, #surface.paths)

    local firstEntry = surface.entries[1]
    check("an entry names the material it is", firstEntry ~= nil and firstEntry.kind == "sand",
        firstEntry and firstEntry.kind)
    local allPaths = firstEntry ~= nil
    for _, key in ipairs({ "baseColor", "normal", "metallicRoughness", "occlusion", "height" }) do
        if type(firstEntry[key]) ~= "string" or firstEntry[key]:sub(1, #MATERIAL_BASE) ~= MATERIAL_BASE then
            allPaths = false
        end
    end
    check("an entry carries all five paths under the prefix the caller asked for", allPaths,
        firstEntry and firstEntry.baseColor)

    local colourBytes = readBytes(MATERIAL_DIR .. "/sand_basecolor.png")
    local colourHead = pngHeader(colourBytes)
    check("a base colour map is a PNG", colourHead ~= nil)
    if colourHead then
        check("the base colour map is 8-bit RGB",
            colourHead.depth == 8 and colourHead.colour == 2,
            colourHead.depth .. "/" .. colourHead.colour)
        check("the base colour map is the size asked for",
            colourHead.width == MATERIAL_SIZE and colourHead.height == MATERIAL_SIZE, colourHead.width)
    end
    local occlusionHead = pngHeader(readBytes(MATERIAL_DIR .. "/sand_occlusion.png"))
    check("an occlusion map is 8-bit grayscale",
        occlusionHead ~= nil and occlusionHead.depth == 8 and occlusionHead.colour == 0,
        occlusionHead and (occlusionHead.depth .. "/" .. occlusionHead.colour))
    local heightHead = pngHeader(readBytes(MATERIAL_DIR .. "/sand_height.png"))
    check("the blending height map is there too",
        heightHead ~= nil and heightHead.colour == 0, heightHead and heightHead.colour)
    local normalHead = pngHeader(readBytes(MATERIAL_DIR .. "/sand_normal.png"))
    check("a normal map is 8-bit RGB",
        normalHead ~= nil and normalHead.depth == 8 and normalHead.colour == 2,
        normalHead and (normalHead.depth .. "/" .. normalHead.colour))

    local blendHead = pngHeader(readBytes(BLEND_DIR .. "/blend_0.png"))
    check("a blend map decodes as 8-bit RGB",
        blendHead ~= nil and blendHead.depth == 8 and blendHead.colour == 2,
        blendHead and (blendHead.depth .. "/" .. blendHead.colour))
    check("a blend map is the resolution the rules asked for",
        blendHead ~= nil and blendHead.width == BLEND_RESOLUTION and blendHead.height == BLEND_RESOLUTION,
        blendHead and blendHead.width)
    check("the blend paths point at the files that exist",
        surface.paths[1] == BLEND_BASE .. "/blend_0.png" and readBytes(BLEND_DIR .. "/blend_2.png") ~= nil)

    check("a re-request of two materials out of the plan succeeds", surface.rerollOk == true)
    local planSand = readBytes(MATERIAL_DIR .. "/sand_basecolor.png")
    local rerollSand = readBytes(REROLL_DIR .. "/sand_basecolor.png")
    check("a material's bytes depend on its seed, not on the position in the request",
        planSand ~= nil and rerollSand ~= nil and planSand == rerollSand)
    local rerollRock = readBytes(REROLL_DIR .. "/rock_basecolor.png")
    check("two materials in one request are not the same map",
        rerollRock ~= nil and rerollRock ~= rerollSand)

    WorldGen.writeMaterials({ out = OTHER_SEED_DIR, kinds = {"sand"}, size = MATERIAL_SIZE,
        seed = SEED + 1, pathBase = OTHER_SEED_BASE })
    local otherSand = readBytes(OTHER_SEED_DIR .. "/sand_basecolor.png")
    check("another seed gives another material", otherSand ~= nil and otherSand ~= planSand)

    Log.print("========== the surface on the live terrain ==========")

    check("the terrain wears all nine layers", terrain:getNumLayers() == 9, terrain:getNumLayers())
    local layer0 = terrain:getSurfaceLayer(0)
    check("the beach layer asks for the wide terrain variant", layer0.pbr == true)
    check("the beach layer is the sand the plan put in slot 0",
        layer0.colorTexture:getPath(0) == MATERIAL_BASE .. "/sand_basecolor.png",
        layer0.colorTexture:getPath(0))
    check("a generated material keeps its blending height on a layer",
        layer0.heightTexture:getPath(0) == MATERIAL_BASE .. "/sand_height.png",
        layer0.heightTexture:getPath(0))
    check("and its normal", layer0.normalTexture:getPath(0) == MATERIAL_BASE .. "/sand_normal.png")
    -- A Vector4 travels as x/y/z/w: there is no `.a` in the Lua binding, and the shader reads this
    -- component's alpha as the flag that puts the layer on the PBR path.
    check("the tint is white, so the authored palette is not multiplied twice",
        math.abs(layer0.colorFactor.w - 1.0) < 1e-6, layer0.colorFactor.w)
    check("the terrain rebuilds with layers and blend maps", terrain:createTerrain() == true)

    Log.print("========== RESULT: " .. passed .. " passed, " .. failed .. " failed ==========")
end
