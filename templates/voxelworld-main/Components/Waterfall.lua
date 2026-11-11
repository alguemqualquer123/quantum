-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- A waterfall that is both things at once: the water you see falling, and the volume that keeps
-- the fall running. The sheet and the mist are Points sprites driven by Particles actions (the
-- only particle path Lua can build), and the impact punches real rings into the shallow-field sim
-- of the pool below.
--
-- The heightfield cannot fall: it is a grid at one altitude, so water crossing a cliff lip has to
-- leave the upper field and arrive in the lower one. `recirculate` does that jump - it takes
-- `flowRate` cubic metres per step out of the source field at the lip and puts exactly that much
-- into the sink field at the plunge pool, which is what makes the fall run forever without the
-- scene either drying out or flooding. Without those two calls the component still draws the
-- fall, it just cannot feed the pool, and says so once.
-- category: Water & Shore
local Waterfall = {
    properties = {
        { name = "top", displayName = "Lip (world position)", type = "vector3", default = { 0, 10.4, -10.2 } },
        { name = "bottom", displayName = "Impact (world position)", type = "vector3", default = { 0, 1.3, -8.6 } },
        { name = "source", displayName = "Upper water entity", type = "entity" },
        { name = "fallbackSourceName", displayName = "Upper water name if slot is empty", type = "string", default = "River" },
        { name = "sink", displayName = "Lower water entity", type = "entity" },
        { name = "fallbackSinkName", displayName = "Lower water name if slot is empty", type = "string", default = "Pool" },
        { name = "recirculate", displayName = "Move volume from the upper field to the pool", type = "bool", default = true },
        { name = "flowRate", displayName = "Flow (cubic metres per second)", type = "float", default = 2.5 },
        { name = "lipRadius", displayName = "Drain radius at the lip (m)", type = "float", default = 1.2 },
        { name = "impactRadius", displayName = "Spread radius in the pool (m)", type = "float", default = 2.6 },
        { name = "width", displayName = "Sheet width across the fall (m)", type = "float", default = 4.2 },
        { name = "thickness", displayName = "Sheet depth into the cliff (m)", type = "float", default = 1.1 },
        -- A heightfield cliff is a slope, not a wall: a drop that only falls goes into the rock.
        -- This is the speed the sheet leaves the lip with, aimed at the impact point, and it is
        -- the difference between a column of water and a wet stain inside the cliff.
        { name = "plunge", displayName = "Outward speed at the lip (m/s)", type = "float", default = 3.0 },
        { name = "drops", displayName = "Drops per second", type = "float", default = 1400.0 },
        { name = "dropSize", displayName = "Drop size (m)", type = "float", default = 0.3 },
        { name = "streak", displayName = "Streak length (x the size, 0 = round)", type = "float", default = 5.0 },
        { name = "opacity", displayName = "Sheet opacity (0..1)", type = "float", default = 0.5 },
        { name = "color", displayName = "Sheet tint (r, g, b)", type = "color3", default = { 0.93, 0.97, 1.0 } },
        { name = "spray", displayName = "Mist particles per second", type = "float", default = 220.0 },
        { name = "mistRadius", displayName = "Mist cloud radius (m)", type = "float", default = 4.5 },
        { name = "mistSize", displayName = "Mist puff size (m)", type = "float", default = 2.6 },
        { name = "mistOpacity", displayName = "Mist opacity (0..1)", type = "float", default = 0.22 },
        { name = "mistColor", displayName = "Mist tint (r, g, b)", type = "color3", default = { 0.86, 0.92, 0.98 } },
        -- A fall reads as a column only if each drop covers ground: the rain streak sprite is 48x192
        -- with 4.5% of its pixels above alpha 200, so a 0.4 m quad of it is a few sub-pixel threads
        -- against the cliff. water_sheet.png is a rope with a dense core for the same footprint.
        { name = "texture", displayName = "Drop sprite ('' = engine default)", type = "string", default = "textures/water_sheet.png" },
        { name = "mistTexture", displayName = "Mist sprite ('' = engine default)", type = "string", default = "textures/fire_puff.png" },
        { name = "windDirection", displayName = "Wind blows toward (degrees)", type = "float", default = 215.0 },
        { name = "windSpeed", displayName = "Wind speed (m/s, bends the fall)", type = "float", default = 1.2 },
        { name = "rippleInterval", displayName = "Impact ring interval (seconds)", type = "float", default = 0.1 },
        { name = "rippleStrength", displayName = "Impact ring strength (0..1)", type = "float", default = 0.55 },
        { name = "foam", displayName = "Keep the pool churned", type = "bool", default = true },
        { name = "startOnLoad", displayName = "Start as soon as the scene loads", type = "bool", default = true }
    }
}

local GRAVITY = 9.81
local START_SPEED = 1.4   -- the lip pushes the sheet out, it does not just let it go

local function clamp(v, lo, hi) if v < lo then return lo end; if v > hi then return hi end; return v end
local function number(v, fallback)
    if type(v) ~= "number" or v ~= v then return fallback end
    return v
end
local function entityId(reference)
    if type(reference) == "number" then return reference ~= 0 and reference or nil end
    if reference and type(reference.entity) == "number" then
        return reference.entity ~= 0 and reference.entity or nil
    end
    return nil
end
local function vec3(value, fallback)
    local ok, out = pcall(function()
        if type(value) ~= "table" and type(value) ~= "userdata" then return nil end
        if type(value.x) == "number" then
            return Vector3(value.x, number(value.y, value.x), number(value.z, value.x))
        end
        if type(value[1]) == "number" then
            return Vector3(value[1], number(value[2], value[1]), number(value[3], value[1]))
        end
        return nil
    end)
    return (ok and out) or Vector3(fallback[1], fallback[2], fallback[3])
end

function Waterfall:init()
    RegisterEngineEvent(self, "onUpdate")
    RegisterEngineEvent(self, "onFixedUpdate")
    self.moved = 0.0
    self.sinceRing = 0.0
    self.lastWarn = 0.0
    self.active = self.startOnLoad ~= false
    self:build()
    self:apply()
end

function Waterfall:warn(message)
    if Engine.systemTime - self.lastWarn > 2.0 then
        self.lastWarn = Engine.systemTime
        print("[Waterfall] " .. message)
    end
end

function Waterfall:vector(name, fallback)
    return vec3(self[name], fallback)
end

-- The fall is a vertical box, so the emitter sits at the lip and every offset below is local to
-- it. The action entity has no Transform of its own that matters: the sprites entity is the one
-- that is moved, exactly like Precipitation.
function Waterfall:build()
    if self.sheet and self.sheetAction then return true end
    local scene = self.scene

    local made = {}
    local function pair(spritesName, actionName)
        local ok, sprites = pcall(function()
            local handle = Points(scene)
            handle.name = spritesName
            return handle
        end)
        if not ok then return nil end
        local ok2, action = pcall(function()
            local handle = Particles(scene)
            handle.name = actionName
            return handle
        end)
        if not ok2 then return nil end
        made[#made + 1] = sprites
        made[#made + 1] = action
        return sprites, action
    end

    local sprites, action = pair("WaterfallSprites", "Waterfall")
    if not sprites then
        self:warn("could not build the Points/Particles pair for the sheet")
        return false
    end
    self.sheet, self.sheetAction = sprites, action

    local mistSprites, mistAction = pair("WaterfallMistSprites", "WaterfallMist")
    self.mist, self.mistAction = mistSprites, mistAction

    pcall(function()
        self.sheet.transparent = true
        self.sheet.autoTransparency = true
        if mistSprites then
            mistSprites.transparent = true
            mistSprites.autoTransparency = true
        end
    end)
    self.hold = made
    return true
end

-- A sprite sheet is optional and gets its own pcall: a path that does not resolve must not also
-- lose the target link below, which is the only thing that makes the drops move at all.
function Waterfall:sprite(points, action, path)
    if not points or not action then return end
    if type(path) ~= "string" or path == "" then return end
    pcall(function()
        points:setTexture(path)
        points:addSpriteFrame("fall", 0, 0, 1, 1)
        action:setSpriteIntializer(0, 0)
    end)
end

-- Time to cross `height` metres starting at START_SPEED downward. The life has to be this, or the
-- drops outlive the fall and pile up under the pool.
function Waterfall:fallTime(height)
    local h = math.max(number(height, 8.0), 0.5)
    return (-START_SPEED + math.sqrt(START_SPEED * START_SPEED + 2.0 * GRAVITY * h)) / GRAVITY
end

function Waterfall:apply()
    if not self.sheetAction then
        if not self:build() then return end
    end
    local top = self:vector("top", { 0, 10.4, -10.2 })
    local bottom = self:vector("bottom", { 0, 1.3, -8.6 })
    local drop = math.max(top.y - bottom.y, 0.5)
    local life = self:fallTime(drop)
    local width = math.max(number(self.width, 4.2), 0.2)
    local thickness = math.max(number(self.thickness, 1.1), 0.1)
    local size = math.max(number(self.dropSize, 0.3), 0.02)
    local streak = math.max(number(self.streak, 5.0), 0.0)
    local tint = self:vector("color", { 0.93, 0.97, 1.0 })
    local radians = math.rad(number(self.windDirection, 215.0))
    local wind = number(self.windSpeed, 1.2)
    -- the sheet leaves the lip sideways and only then accelerates down, which is what makes the
    -- plume lean; without the initial horizontal term the column is a straight rod
    local vx = math.cos(radians) * wind
    local vz = math.sin(radians) * wind
    -- Aimed at the impact point rather than at an angle the author has to keep in step with the
    -- relief: the fall has to clear the face, and the face is defined by the same two points.
    local plunge = math.max(number(self.plunge, 3.0), 0.0)
    local spanX, spanZ = bottom.x - top.x, bottom.z - top.z
    local span = math.sqrt(spanX * spanX + spanZ * spanZ)
    if span < 0.5 then spanX, spanZ, span = 0.0, 1.0, 1.0 end
    local ox = spanX / span * plunge
    local oz = spanZ / span * plunge

    pcall(function()
        -- started 0.4 m in front of the crest, or the first frames of every drop are behind the
        -- rock it is supposed to be falling past
        self.sheet.position = Vector3(top.x + spanX / span * 0.4, top.y, top.z + spanZ / span * 0.4)
        local particles = self.sheetAction
        particles.maxParticles = math.floor(clamp(number(self.drops, 1400.0) * life * 1.6, 64, 8192))
        particles.target = self.sheet.entity
        particles.localSpace = true
        particles.loop = true
        particles:setLifeInitializer(life * 0.9, life)
        particles:setPositionInitializer(Vector3(-width * 0.5, 0.0, -thickness * 0.5),
            Vector3(width * 0.5, 0.35, thickness * 0.5))
        particles:setVelocityInitializer(Vector3(ox * 0.8 + vx * 0.6, -START_SPEED, oz * 0.8 + vz * 0.6),
            Vector3(ox * 1.2 + vx * 1.4 + 0.4, -START_SPEED * 1.6, oz * 1.2 + vz * 1.4 + 0.4))
        particles:setAccelerationInitializer(Vector3(vx, -GRAVITY, vz))
        particles:setSizeInitializer(size, size * 1.4)
        if streak > 0.0 then
            particles:setScaleInitializer(Vector3(1.0, 1.0 + streak, 1.0))
        else
            particles:setScaleInitializer(Vector3(1.0, 1.0, 1.0))
        end
        particles:setColorInitializer(tint)
        particles:setAlphaInitializer(number(self.opacity, 0.5))
        -- the sheet thickens as it stretches and clears out just before the impact, where the
        -- mist takes over: a hard cut at the pool plane reads as a wall, not as water landing
        particles:setAlphaModifier(life * 0.72, life, number(self.opacity, 0.5), 0.0)
        self:sprite(self.sheet, particles, self.texture)
    end)

    if self.mistAction then
        local mistSize = math.max(number(self.mistSize, 2.6), 0.1)
        local mistTint = self:vector("mistColor", { 0.86, 0.92, 0.98 })
        local mistLife = 2.6
        pcall(function()
            self.mist.position = bottom
            local particles = self.mistAction
            particles.maxParticles = math.floor(clamp(number(self.spray, 220.0) * mistLife, 32, 4096))
            particles.target = self.mist.entity
            particles.localSpace = true
            particles.loop = true
            particles:setLifeInitializer(mistLife * 0.6, mistLife)
            particles:setCirclePositionInitializer(math.max(number(self.mistRadius, 4.5), 0.5) * 0.4)
            particles:setVelocityInitializer(Vector3(-1.6, 1.4, -1.6), Vector3(1.6, 4.2, 1.6))
            particles:setVelocityModifier(0.0, mistLife, Vector3(vx * 2.0, 0.4, vz * 2.0),
                Vector3(vx * 3.0 + 1.2, -0.6, vz * 3.0 + 1.2))
            particles:setSizeInitializer(mistSize * 0.5, mistSize)
            particles:setColorInitializer(mistTint)
            particles:setAlphaInitializer(0.0, number(self.mistOpacity, 0.22))
            particles:setAlphaModifier(mistLife * 0.35, mistLife, number(self.mistOpacity, 0.22), 0.0)
            self:sprite(self.mist, particles, self.mistTexture)
        end)
    end

    self:restart()
end

-- An action only allocates its pool when it starts, so every rebuild of the look has to go back
-- through restart().
function Waterfall:restart()
    for _, action in ipairs({ self.sheetAction, self.mistAction }) do
        if action then pcall(function() action:restart() end) end
    end
    self:updateRate()
end

function Waterfall:updateRate()
    local scale = self.active and 1.0 or 0.0
    pcall(function()
        if self.sheetAction then
            self.sheetAction.rate = math.floor(number(self.drops, 1400.0) * scale)
            self.sheetAction.emitter = scale > 0.0
            if scale > 0.0 and not self.sheetAction:isRunning() then self.sheetAction:restart() end
        end
        if self.mistAction then
            self.mistAction.rate = math.floor(number(self.spray, 220.0) * scale)
            self.mistAction.emitter = scale > 0.0
            if scale > 0.0 and not self.mistAction:isRunning() then self.mistAction:restart() end
        end
    end)
end

function Waterfall:handle(class, slot, fallbackName)
    local id = entityId(slot)
    if not id and type(fallbackName) == "string" and fallbackName ~= "" then
        id = entityId(self.scene:findEntity(fallbackName))
    end
    if not id then return nil end
    local ok, handle = pcall(class, self.scene, id)
    return ok and handle or nil
end

function Waterfall:sourceWater()
    if self._source == nil then
        self._source = self:handle(Water, self.source, self.fallbackSourceName) or false
    end
    return self._source or nil
end

function Waterfall:sinkWater()
    if self._sink == nil then
        self._sink = self:handle(Water, self.sink, self.fallbackSinkName) or false
    end
    return self._sink or nil
end

-- The jump the heightfield cannot make: the same volume that leaves the upper field at the lip is
-- what arrives in the pool. Drain first, then add only what was actually taken, so a starving
-- upper field shrinks the fall instead of inventing water.
function Waterfall:onFixedUpdate()
    if self.active == false or self.recirculate == false then return end
    local source, sink = self:sourceWater(), self:sinkWater()
    if not source or not sink then return end
    if type(source.drainWater) ~= "function" or type(sink.addWater) ~= "function" then
        if not self.warnedNoSource then
            self.warnedNoSource = true
            self:warn("this build has no Water:drainWater/addWater, so the pool is fed by rain only")
        end
        return
    end
    local top = self:vector("top", { 0, 10.4, -10.2 })
    local bottom = self:vector("bottom", { 0, 1.3, -8.6 })
    local want = math.max(number(self.flowRate, 2.5), 0.0) * number(Engine.updateTime, 1.0 / 60.0)
    if want <= 0.0 then return end

    local ok, moved = pcall(function()
        return source:drainWater(Vector3(top.x, top.y, top.z),
            math.max(number(self.lipRadius, 3.0), 0.2), want)
    end)
    if not ok or type(moved) ~= "number" or moved <= 0.0 then
        self.starved = (self.starved or 0) + 1
        if self.starved == 60 then
            self:warn("the upper field has nothing left to give at the lip")
        end
        return
    end
    self.starved = 0
    pcall(function()
        sink:addWater(Vector3(bottom.x, bottom.y, bottom.z),
            math.max(number(self.impactRadius, 2.6), 0.2), moved)
    end)
    self.moved = self.moved + moved
end

-- The impact is the loudest thing the sim can hear: a ring every few frames keeps the pool
-- visibly churned under the fall even when the volume itself is not moving.
function Waterfall:onUpdate()
    if self.active == false then return end
    local step = number(Engine.deltatime, 0.0)
    self.sinceRing = self.sinceRing + step
    local every = math.max(number(self.rippleInterval, 0.1), 0.02)
    if self.sinceRing < every then return end
    self.sinceRing = 0.0
    if self.foam == false then return end
    local sink = self:sinkWater()
    if not sink then return end
    local bottom = self:vector("bottom", { 0, 1.3, -8.6 })
    pcall(function()
        sink:addDisturbance(Vector3(bottom.x, bottom.y, bottom.z),
            math.max(number(self.mistRadius, 4.5), 1.0) * 0.55,
            clamp(number(self.rippleStrength, 0.55), 0.0, 1.0))
    end)
end

function Waterfall:setFlow(value)
    self.flowRate = math.max(number(value, self.flowRate), 0.0)
    self:apply()
end
function Waterfall:start() self.active = true; self:updateRate() end
function Waterfall:stop() self.active = false; self:updateRate() end
function Waterfall:getCount()
    local out = 0
    if self.sheet then pcall(function() out = self.sheet:getNumPoints() end) end
    return out
end
-- cubic metres moved over the lip since the scene started: the number a headless run asserts on
function Waterfall:getMovedVolume() return self.moved or 0.0 end

return Waterfall
