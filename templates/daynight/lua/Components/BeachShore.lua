-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- The one-knob beach: attach to the entity holding the Water component; a preset applies a
-- category: Water & Shore
-- coherent shore and clarity/wavePower/shorelineFoam live-retune it.
local BeachShore = {
    properties = {
        { name = "water", displayName = "Water", type = "entity" },
        { name = "fallbackName", displayName = "Water name if slot is empty", type = "string", default = "Water" },
        { name = "preset", displayName = "Preset (0 tropical, 1 mediterranean, 2 cold grey, 3 lagoon)", type = "int", default = 0 },
        { name = "clarity", displayName = "Water clarity (0..1)", type = "float", default = 0.75 },
        { name = "wavePower", displayName = "Wave power (0..1)", type = "float", default = 0.5 },
        { name = "shorelineFoam", displayName = "Shoreline foam (0..1)", type = "float", default = 0.6 },
        { name = "waves", displayName = "Waves driver", type = "WaterWaves" },
        { name = "terrain", displayName = "Terrain", type = "entity" },
        { name = "terrainName", displayName = "Terrain name if slot is empty", type = "string", default = "Terrain" },
        { name = "sandTexture", displayName = "Sand detail texture (path, empty = keep)", type = "string", default = "" },
        { name = "sky", displayName = "Sky", type = "entity" },
        { name = "fog", displayName = "Fog", type = "entity" }
    }
}

local function clamp(v, lo, hi) if v < lo then return lo end if v > hi then return hi end return v end

local function entityId(ref)
    if type(ref) == "number" then return ref ~= 0 and ref or nil end
    if ref and type(ref.entity) == "number" then return ref.entity ~= 0 and ref.entity or nil end
    return nil
end

-- Water members applied per preset; preset.v holds the same fields positionally.
local keys = { "depthFade", "shoreFoam", "crestFoam", "caustics", "causticsScale", "refraction",
    "refractionDistortion", "reflectionDistortion", "roughness", "specularIntensity", "reflectivity",
    "normalScale", "normalStrength", "rippleSpeed", "underwater", "buoyancy", "buoyancyDepth", "subdivisions" }
-- s/d/f = shallow/deep/foam RGB, area = plane metres, sky RGBA, fog RGB, fr = linearStart/End.
local presets = {
    { s = {0.16, 0.72, 0.66}, d = {0.01, 0.15, 0.28}, f = {1, 1, 1}, area = {160, 160}, sky = {0.45, 0.68, 0.92, 1}, fog = {0.75, 0.87, 0.95}, fr = {40, 420},
      v = {0.55, 0.55, 0.28, 0.9, 1.4, true, 0.06, 0.12, 0.08, 1.3, 0.55, 1.0, 0.6, 1.0, true, true, 1.6, 64} },
    { s = {0.22, 0.58, 0.72}, d = {0.02, 0.10, 0.26}, f = {0.96, 0.98, 1}, area = {140, 140}, sky = {0.38, 0.60, 0.90, 1}, fog = {0.72, 0.82, 0.92}, fr = {50, 380},
      v = {0.70, 0.40, 0.18, 0.7, 1.1, true, 0.05, 0.10, 0.06, 1.5, 0.65, 0.9, 0.5, 0.8, true, true, 2.0, 64} },
    { s = {0.30, 0.42, 0.46}, d = {0.06, 0.10, 0.14}, f = {0.90, 0.92, 0.94}, area = {200, 200}, sky = {0.42, 0.47, 0.53, 1}, fog = {0.55, 0.60, 0.65}, fr = {20, 260},
      v = {0.35, 0.65, 0.35, 0.25, 0.8, false, 0.09, 0.22, 0.20, 0.8, 0.45, 1.3, 0.9, 1.3, true, true, 2.4, 72} },
    { s = {0.45, 0.80, 0.62}, d = {0.05, 0.30, 0.32}, f = {1, 1, 0.96}, area = {90, 90}, sky = {0.52, 0.74, 0.88, 1}, fog = {0.80, 0.90, 0.90}, fr = {30, 300},
      v = {0.90, 0.30, 0.12, 1.0, 1.8, true, 0.03, 0.06, 0.04, 1.1, 0.40, 0.7, 0.35, 0.6, true, true, 0.9, 48} }
}

function BeachShore:init()
    RegisterEngineEvent(self, "onUpdate")
    self.applied = -1
    self.lastWarn = 0.0
end

function BeachShore:warn(message)
    if Engine.systemTime - self.lastWarn > 1.0 then
        self.lastWarn = Engine.systemTime
        print("[BeachShore] " .. message)
    end
end

function BeachShore:handle(factory, slot, fallbackName)
    local id = entityId(slot)
    if not id and fallbackName and fallbackName ~= "" then id = entityId(self.scene:findEntity(fallbackName)) end
    if not id then return nil end
    local ok, instance = pcall(function() return factory(self.scene, id) end)
    return (ok and instance) or nil
end

function BeachShore:applyPreset(index)
    if type(index) ~= "number" then index = 0 end
    index = clamp(math.floor(index + 0.5), 0, #presets - 1)
    self.preset, self.applied = index, index
    local p = presets[index + 1]
    local water = self:handle(Water, self.water, self.fallbackName)
    if not water then
        self:warn("No water: add a Water component or name it " .. tostring(self.fallbackName))
        return
    end
    pcall(function()
        -- The colour setters are bound as (r, g, b) scalars in Lua, so the vector forms go through
        -- the properties, which are bound to the Vector3/Vector2 overloads.
        water.shallowColor = Vector3(p.s[1], p.s[2], p.s[3])
        water.deepColor = Vector3(p.d[1], p.d[2], p.d[3])
        water.foamColor = Vector3(p.f[1], p.f[2], p.f[3])
        water.size = Vector2(p.area[1], p.area[2])
        for i, name in ipairs(keys) do water[name] = p.v[i] end
    end)
    self:applyKnobs(water, p)
    local terrain = self:handle(Terrain, self.terrain, self.terrainName)
    if terrain then
        pcall(function()
            terrain.textureBaseTiles = 6
            -- Never call the blend-map writer with an empty path.
            if self.sandTexture and self.sandTexture ~= "" then terrain:setTextureDetailBlue(self.sandTexture) end
        end)
    end
    local sky = self:handle(SkyBox, self.sky, nil)
    if sky then pcall(function() sky.color = Vector4(p.sky[1], p.sky[2], p.sky[3], p.sky[4]) end) end
    local fog = self:handle(Fog, self.fog, nil)
    if fog then
        pcall(function()
            fog.color = Vector3(p.fog[1], p.fog[2], p.fog[3])
            fog:setLinearStartEnd(p.fr[1], p.fr[2])
        end)
    end
end

-- The live knobs re-scale the preset instead of replacing it; p.v follows the `keys` order.
function BeachShore:applyKnobs(water, p)
    local clarity, foam = clamp(self.clarity or 0.75, 0, 1), clamp(self.shorelineFoam or 0.6, 0, 1)
    pcall(function()
        water.depthFade = p.v[1] * (0.4 + 1.2 * clarity)
        water.caustics = p.v[4] * (0.3 + 1.4 * clarity)
        water.refraction = (p.v[6] == true) and clarity > 0.15
        water.refractionDistortion = p.v[7] * (1.6 - 1.2 * clarity)
        water.shoreFoam = clamp(p.v[2] * (0.3 + 1.4 * foam), 0.0, 1.0)
        water.crestFoam = clamp(p.v[3] * (0.3 + 1.4 * foam), 0.0, 1.0)
    end)
    -- A script slot only carries setIntensity when the wired entity really runs that script; with a
    -- bare entity handle behind it, waves.setIntensity is nil and calling it would be an error.
    local waves = self.waves
    if type(waves) == "table" and type(waves.setIntensity) == "function" then
        pcall(function() waves:setIntensity(clamp(self.wavePower or 0.5, 0.0, 1.0)) end)
    elseif type(waves) == "userdata" then
        self:warn("wire this: a WaterWaves component on the entity in the Waves driver slot")
    end
end

-- 0 calm morning, 1 stormy dusk: sky and fog slide toward grey and the water dulls.
function BeachShore:setMood(zeroToOne)
    local t = clamp(tonumber(zeroToOne) or 0.0, 0.0, 1.0)
    local p = presets[clamp(self.applied or 0, 0, #presets - 1) + 1]
    local sky = self:handle(SkyBox, self.sky, nil)
    if sky then
        pcall(function() sky.color = Vector4(p.sky[1] * (1 - 0.6 * t), p.sky[2] * (1 - 0.55 * t), p.sky[3] * (1 - 0.45 * t), p.sky[4]) end)
    end
    local fog = self:handle(Fog, self.fog, nil)
    if fog then
        pcall(function()
            fog.color = Vector3(p.fog[1] * (1 - 0.55 * t), p.fog[2] * (1 - 0.5 * t), p.fog[3] * (1 - 0.4 * t))
            fog:setLinearStartEnd(p.fr[1] * (1 - 0.5 * t), p.fr[2] * (1 - 0.6 * t))
        end)
    end
    local water = self:handle(Water, self.water, self.fallbackName)
    if water then
        pcall(function() water.specularIntensity = p.v[10] * (1 - 0.7 * t) water.shoreFoam = clamp(p.v[2] + 0.4 * t, 0.0, 1.0) end)
    end
end

-- Widens the white wet-sand band by pushing shore foam out; width is in metres.
function BeachShore:wetSand(width)
    local water = self:handle(Water, self.water, self.fallbackName)
    if not water or type(width) ~= "number" then return end
    local p = presets[clamp(self.applied or 0, 0, #presets - 1) + 1]
    pcall(function()
        water.shoreFoam = clamp(p.v[2] + width * 0.05, 0.0, 1.0) water.depthFade = clamp(p.v[1] - width * 0.02, 0.05, 2.0)
    end)
end

function BeachShore:onUpdate()
    if self.preset ~= self.applied then
        self.lastClarity, self.lastWavePower, self.lastFoam = self.clarity, self.wavePower, self.shorelineFoam
        self:applyPreset(self.preset or 0)
    elseif self.clarity ~= self.lastClarity or self.wavePower ~= self.lastWavePower or self.shorelineFoam ~= self.lastFoam then
        self.lastClarity, self.lastWavePower, self.lastFoam = self.clarity, self.wavePower, self.shorelineFoam
        local water = self:handle(Water, self.water, self.fallbackName)
        if water then self:applyKnobs(water, presets[clamp(self.applied, 0, #presets - 1) + 1]) end
    end
end

return BeachShore
