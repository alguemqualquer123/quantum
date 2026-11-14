-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Real geometry for an entity whose mesh component has none. A hand-written scene file cannot
-- carry a vertex buffer, and the engine reads only mesh/submeshes/buffer/indices from YAML -
-- there is no "meshType" key - so a primitive described in the file loads as an invisible mesh.
-- This builds the same geometry the editor's Create > Box produces, at start, on this entity.
-- Attach it to the entity that already has a Mesh component.
-- category: Scene & HUD
local PrimitiveShape = {
    properties = {
        { name = "kind", displayName = "Shape (plane/wall/box/sphere/cylinder/capsule/torus)", type = "string", default = "box" },
        { name = "width", displayName = "Width (m)", type = "float", default = 1.0 },
        { name = "height", displayName = "Height (m)", type = "float", default = 1.0 },
        { name = "depth", displayName = "Depth (m)", type = "float", default = 1.0 },
        { name = "radius", displayName = "Radius (m)", type = "float", default = 0.5 },
        { name = "topRadius", displayName = "Top radius (0 = same as radius)", type = "float", default = 0.0 },
        { name = "ringRadius", displayName = "Torus ring radius (m)", type = "float", default = 0.2 },
        { name = "slices", displayName = "Slices / rings", type = "int", default = 24 },
        { name = "stacks", displayName = "Stacks / sides", type = "int", default = 12 },
        { name = "tiles", displayName = "Subdivisions (plane/box/wall)", type = "int", default = 1 },
        { name = "red", displayName = "Colour R", type = "float", default = 1.0 },
        { name = "green", displayName = "Colour G", type = "float", default = 1.0 },
        { name = "blue", displayName = "Colour B", type = "float", default = 1.0 },
        { name = "alpha", displayName = "Colour A", type = "float", default = 1.0 },
        { name = "rebuild", displayName = "Rebuild when the size properties change", type = "bool", default = false }
    }
}

local function text(value, fallback)
    return type(value) == "string" and value ~= "" and value or fallback
end

local function num(value, fallback)
    return type(value) == "number" and value or fallback
end

local function whole(value, fallback)
    if type(value) ~= "number" then
        return fallback
    end
    return math.max(3, math.floor(value))
end

function PrimitiveShape:init()
    self.warningShown = false
    self:build()
    if self.rebuild then
        RegisterEngineEvent(self, "onUpdate")
    end
end

function PrimitiveShape:build()
    local shape = self.kind or "box"
    local w = num(self.width, 1.0)
    local h = num(self.height, 1.0)
    local d = num(self.depth, 1.0)
    local r = num(self.radius, 0.5)
    local top = num(self.topRadius, 0.0)
    if top <= 0.0 then
        top = r
    end
    local slices = whole(self.slices, 24)
    local stacks = whole(self.stacks, 12)
    local tiles = whole(self.tiles, 1)

    local ok, err = pcall(function()
        local mesh = Shape(self.scene, self.entity)
        if shape == "plane" then
            mesh:createPlane(w, d, tiles)
        elseif shape == "wall" then
            mesh:createWall(w, h, tiles)
        elseif shape == "box" then
            mesh:createBox(w, h, d, tiles)
        elseif shape == "sphere" then
            mesh:createSphere(r, slices, stacks)
        elseif shape == "cylinder" then
            mesh:createCylinder(r, top, h, slices, stacks)
        elseif shape == "capsule" then
            mesh:createCapsule(r, top, h, slices, stacks)
        elseif shape == "torus" then
            mesh:createTorus(r, num(self.ringRadius, 0.2), slices, stacks)
        else
            error("unknown shape '" .. tostring(shape) .. "'")
        end
        -- The tint is the only look a primitive can get without project textures: the stock PBR
        -- tile set lives in the engine's assets folder and is not part of a new project.
        mesh.color = Vector4(num(self.red, 1.0), num(self.green, 1.0), num(self.blue, 1.0),
                             num(self.alpha, 1.0))
    end)

    if not ok and not self.warningShown then
        self.warningShown = true
        print("[PrimitiveShape] " .. tostring(err) ..
              " - the entity needs a Mesh component.")
    end
end

function PrimitiveShape:onUpdate()
    if not self.rebuild then
        return
    end
    -- Comparing the authored numbers costs one table read per frame and keeps the Inspector's
    -- size fields live without a reload.
    local signature = table.concat({ text(self.kind, "box"), num(self.width, 1.0), num(self.height, 1.0),
                                     num(self.depth, 1.0), num(self.radius, 0.5), num(self.topRadius, 0.0),
                                     num(self.ringRadius, 0.2), whole(self.slices, 24), whole(self.stacks, 12),
                                     whole(self.tiles, 1) }, "|")
    if signature ~= self.lastSignature then
        self.lastSignature = signature
        self:build()
    end
end

return PrimitiveShape
