-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Scratch harness: loads every library component in plain Lua 5.4, instantiates it the way
-- LuaBinding does and runs frames. Not part of the build.
--
-- The class stubs are STRICT on purpose: every member name below is copied from the generated
-- Lua surface (build/generated/lua_api.md), so a component that calls a method the engine does
-- not bind fails here instead of failing silently in a scene. When this harness reports
-- "X has no member 'y'", the component is wrong, not the stub: check lua_api.md and use the
-- real API, or report the missing stub. Do not add members to this file to make a component
-- pass.
--
-- usage: lua54 checks/kit_harness.lua <components-dir> [Only,Names]

-- The component list lives in `order` below: a file that is not in it is not part of the
-- library, and a name in it that has no file fails as MISSING.
local dir = arg[1] or "."
local onlyFilter = nil
if arg[2] then
    onlyFilter = {}
    for name in arg[2]:gmatch("[^,]+") do
        onlyFilter[name:gsub("^%s+", ""):gsub("%s+$", "")] = true
    end
end

-- ---------------------------------------------------------------- math

local vector = {}
vector.__index = vector

local function v3(x, y, z)
    return setmetatable({ x = x or 0.0, y = y or 0.0, z = z or 0.0 }, vector)
end

function vector:__add(o) return v3(self.x + o.x, self.y + o.y, self.z + o.z) end
function vector:__sub(o) return v3(self.x - o.x, self.y - o.y, self.z - o.z) end
function vector:__mul(o)
    if type(o) == "number" then return v3(self.x * o, self.y * o, self.z * o) end
    return v3(self.x * o.x, self.y * o.y, self.z * o.z)
end
function vector:__div(o)
    if type(o) == "number" then return v3(self.x / o, self.y / o, self.z / o) end
    return v3(self.x / o.x, self.y / o.y, self.z / o.z)
end
function vector:__unm() return v3(-self.x, -self.y, -self.z) end
function vector:isValid() return true end
function vector:length() return math.sqrt(self.x * self.x + self.y * self.y + self.z * self.z) end
function vector:squaredLength() return self.x * self.x + self.y * self.y + self.z * self.z end
function vector:distance(o)
    local dx, dy, dz = self.x - o.x, self.y - o.y, self.z - o.z
    return math.sqrt(dx * dx + dy * dy + dz * dz)
end
function vector:squaredDistance(o)
    local dx, dy, dz = self.x - o.x, self.y - o.y, self.z - o.z
    return dx * dx + dy * dy + dz * dz
end
function vector:dotProduct(o) return self.x * o.x + self.y * o.y + self.z * o.z end
function vector:absDotProduct(o) return math.abs(self:dotProduct(o)) end
function vector:crossProduct(o)
    return v3(self.y * o.z - self.z * o.y, self.z * o.x - self.x * o.z, self.x * o.y - self.y * o.x)
end
function vector:normalize()
    local n = self:length()
    if n < 0.000001 then return v3(0.0, 0.0, 0.0) end
    self.x, self.y, self.z = self.x / n, self.y / n, self.z / n
    return self
end
function vector:normalized()
    local copy = v3(self.x, self.y, self.z)
    return copy:normalize()
end
function vector:normalizeL() return self:length() end
function vector:midPoint(o) return v3((self.x + o.x) * 0.5, (self.y + o.y) * 0.5, (self.z + o.z) * 0.5) end
function vector:lerp(o, t)
    return v3(self.x + (o.x - self.x) * t, self.y + (o.y - self.y) * t, self.z + (o.z - self.z) * t)
end
function vector:moveTowards(target, maxDistanceDelta)
    local delta = target - self
    local dist = delta:length()
    if dist <= maxDistanceDelta or dist < 0.000001 then return v3(target.x, target.y, target.z) end
    return self + delta * (maxDistanceDelta / dist)
end
function vector:makeFloor(o)
    self.x = math.min(self.x, o.x)
    self.y = math.min(self.y, o.y)
    self.z = math.min(self.z, o.z)
end
function vector:makeCeil(o)
    self.x = math.max(self.x, o.x)
    self.y = math.max(self.y, o.y)
    self.z = math.max(self.z, o.z)
end
function vector:perpendicular()
    if math.abs(self.x) > math.abs(self.z) then return v3(-self.z, 0.0, self.x) end
    return v3(0.0, -self.z, self.y)
end
function vector:reflect(normal)
    local d = 2.0 * self:dotProduct(normal)
    return self - normal * d
end

Vector3 = setmetatable({}, {
    __call = function(_, a, b, c)
        if a and type(a) ~= "number" then
            if b ~= nil then return v3(a.x, a.y, b) end
            return v3(a.x, a.y, a.z)
        end
        return v3(a, b, c)
    end
})
Vector3.ZERO = v3(0.0, 0.0, 0.0)
Vector3.UNIT_X = v3(1.0, 0.0, 0.0)
Vector3.UNIT_Y = v3(0.0, 1.0, 0.0)
Vector3.UNIT_Z = v3(0.0, 0.0, 1.0)
Vector3.UNIT_SCALE = v3(1.0, 1.0, 1.0)

Vector2 = setmetatable({}, { __call = function(_, x, y) return v3(x, y, 0.0) end })
Vector2.ZERO = v3(0.0, 0.0, 0.0)
Vector2.UNIT_X = v3(1.0, 0.0, 0.0)
Vector2.UNIT_Y = v3(0.0, 1.0, 0.0)
Vector2.UNIT_SCALE = v3(1.0, 1.0, 0.0)

Vector4 = setmetatable({}, { __call = function(_, x, y, z, w)
    return { x = x or 0.0, y = y or 0.0, z = z or 0.0, w = w or 0.0 }
end })

local quat = {}
quat.__index = quat
Quaternion = function(x, y, z)
    return setmetatable({ x = x or 0.0, y = y or 0.0, z = z or 0.0, w = 1.0 }, quat)
end
function quat:zAxis() return v3(0.0, 0.0, 1.0) end
function quat:xAxis() return v3(1.0, 0.0, 0.0) end
function quat:yAxis() return v3(0.0, 1.0, 0.0) end
function quat:normalize() return Quaternion(self.x, self.y, self.z) end
function quat:getYaw() return self.y end
function quat:getPitch() return self.x end
function quat:getRoll() return self.z end
function quat:__mul(o)
    if type(o) == "table" and o.x and o.z and o.y and o.length then return v3(o.x, o.y, o.z) end
    return Quaternion(self.x + (o.x or 0.0), self.y + (o.y or 0.0), self.z + (o.z or 0.0))
end
Matrix4 = function() return { identity = true } end
Rect = function(x, y, w, h) return { x = x or 0, y = y or 0, width = w or 0, height = h or 0 } end
-- The binding exposes minimum/maximum and the isNull/merge/setNull/getCorners methods; a stub with
-- min/max only would make every reader of a real AABB look correct here and break in the engine.
AABB = function()
    local box = { minimum = v3(-1, -1, -1), maximum = v3(1, 1, 1) }
    function box:isNull() return self.maximum.x < self.minimum.x end
    function box:setNull()
        self.minimum = v3(math.huge, math.huge, math.huge)
        self.maximum = v3(-math.huge, -math.huge, -math.huge)
        return self
    end
    function box:merge(other)
        if not other then return self end
        local a, b = other.minimum or other.min, other.maximum or other.max
        if a and b then
            self.minimum = v3(math.min(self.minimum.x, a.x), math.min(self.minimum.y, a.y),
                math.min(self.minimum.z, a.z))
            self.maximum = v3(math.max(self.maximum.x, b.x), math.max(self.maximum.y, b.y),
                math.max(self.maximum.z, b.z))
        end
        return self
    end
    function box:getCorners()
        local m, M = self.minimum, self.maximum
        return { v3(m.x, m.y, m.z), v3(M.x, m.y, m.z), v3(M.x, m.y, M.z), v3(m.x, m.y, M.z),
            v3(m.x, M.y, m.z), v3(M.x, M.y, m.z), v3(M.x, M.y, M.z), v3(m.x, M.y, M.z) }
    end
    return box
end
OBB = function() return { center = v3(0, 0, 0) } end
Plane = function() return { normal = v3(0, 1, 0), d = 0.0 } end
Sphere = function() return { center = v3(0, 0, 0), radius = 1.0 } end
Angle = function(degrees) return degrees end
Base64 = { encode = function(value) return value end, decode = function(value) return value end }

-- ---------------------------------------------------------------- enums
-- Members and spelling taken from the `## Enums` section of lua_api.md.

local function enum(members)
    local e = {}
    for i, name in ipairs(members) do e[name] = i - 1 end
    return e
end

BodyType = enum({ "STATIC", "KINEMATIC", "DYNAMIC" })
Body3DMotionQuality = enum({ "DISCRETE", "LINEAR_CAST" })
CameraType = enum({ "CAMERA_UI", "CAMERA_ORTHO", "CAMERA_PERSPECTIVE" })
LightType = enum({ "DIRECTIONAL", "POINT", "SPOT" })
FogType = enum({ "LINEAR", "EXPONENTIAL", "EXPONENTIALSQUARED" })
Joint3DType = enum({ "FIXED", "DISTANCE", "POINT", "HINGE", "CONE", "PRISMATIC", "SWINGTWIST",
    "SIXDOF", "PATH", "GEAR", "RACKANDPINION", "PULLEY" })
Joint2DType = enum({ "DISTANCE", "REVOLUTE", "PRISMATIC", "MOUSE", "WHEEL", "WELD", "MOTOR" })
ParticleEmitterShape = enum({ "Box", "Sphere", "Hemisphere", "Circle", "Cone" })
ShaderType = enum({ "MESH", "UI", "SKYBOX", "POINTS", "LINES", "WATER" })
ContainerType = enum({ "BOX", "WRAP" })
PivotPreset = enum({ "TOP_LEFT", "TOP", "TOP_RIGHT", "LEFT", "CENTER", "RIGHT",
    "BOTTOM_LEFT", "BOTTOM", "BOTTOM_RIGHT" })
PrimitiveType = enum({ "TRIANGLES", "TRIANGLE_FAN", "TRIANGLE_STRIP", "LINES", "LINE_STRIP", "POINTS" })
RayFilter = { BODY_2D = 1, BODY_3D = 2, BOUNDS = 3 }

-- ---------------------------------------------------------------- input

Input = {
    MODIFIER_SHIFT = 0x0001, MODIFIER_CONTROL = 0x0002, MODIFIER_ALT = 0x0004,
    MODIFIER_SUPER = 0x0008, MODIFIER_CAPS_LOCK = 0x0100, MODIFIER_NUM_LOCK = 0x0200,
    KEY_UNKNOWN = -1, KEY_SPACE = 32, KEY_APOSTROPHE = 39, KEY_COMMA = 44, KEY_MINUS = 45,
    KEY_PERIOD = 46, KEY_SLASH = 47, KEY_SEMICOLON = 59, KEY_EQUAL = 61,
    KEY_LEFT_BRACKET = 91, KEY_BACKSLASH = 92, KEY_RIGHT_BRACKET = 93, KEY_GRAVE_ACCENT = 96,
    KEY_WORLD_1 = 97, KEY_WORLD_2 = 98, KEY_ESCAPE = 256, KEY_ENTER = 257, KEY_TAB = 258,
    KEY_BACKSPACE = 259, KEY_INSERT = 260, KEY_DELETE = 261, KEY_RIGHT = 262, KEY_LEFT = 263,
    KEY_DOWN = 264, KEY_UP = 265, KEY_PAGE_UP = 266, KEY_PAGE_DOWN = 267, KEY_HOME = 268,
    KEY_END = 269, KEY_CAPS_LOCK = 280, KEY_SCROLL_LOCK = 281, KEY_NUM_LOCK = 282,
    KEY_PRINT_SCREEN = 283, KEY_PAUSE = 284, KEY_MENU = 348, KEY_LAST = 348,
    KEY_KP_DECIMAL = 330, KEY_KP_DIVIDE = 331, KEY_KP_MULTIPLY = 332, KEY_KP_SUBTRACT = 333,
    KEY_KP_ADD = 334, KEY_KP_ENTER = 335, KEY_KP_EQUAL = 336,
    KEY_LEFT_SHIFT = 340, KEY_LEFT_CONTROL = 341, KEY_LEFT_ALT = 342, KEY_LEFT_SUPER = 343,
    KEY_RIGHT_SHIFT = 344, KEY_RIGHT_CONTROL = 345, KEY_RIGHT_ALT = 346, KEY_RIGHT_SUPER = 347,
    MOUSE_BUTTON_1 = 0, MOUSE_BUTTON_2 = 1, MOUSE_BUTTON_3 = 2, MOUSE_BUTTON_4 = 3,
    MOUSE_BUTTON_5 = 4, MOUSE_BUTTON_6 = 5, MOUSE_BUTTON_7 = 6, MOUSE_BUTTON_8 = 7,
    MOUSE_BUTTON_LAST = 7, MOUSE_BUTTON_LEFT = 0, MOUSE_BUTTON_RIGHT = 1, MOUSE_BUTTON_MIDDLE = 2,
    GAMEPAD_BUTTON_A = 0, GAMEPAD_BUTTON_B = 1, GAMEPAD_BUTTON_X = 2, GAMEPAD_BUTTON_Y = 3,
    GAMEPAD_BUTTON_LEFT_BUMPER = 4, GAMEPAD_BUTTON_RIGHT_BUMPER = 5, GAMEPAD_BUTTON_BACK = 6,
    GAMEPAD_BUTTON_START = 7, GAMEPAD_BUTTON_GUIDE = 8, GAMEPAD_BUTTON_LEFT_THUMB = 9,
    GAMEPAD_BUTTON_RIGHT_THUMB = 10, GAMEPAD_BUTTON_DPAD_UP = 11, GAMEPAD_BUTTON_DPAD_RIGHT = 12,
    GAMEPAD_BUTTON_DPAD_DOWN = 13, GAMEPAD_BUTTON_DPAD_LEFT = 14, GAMEPAD_BUTTON_LAST = 15,
    GAMEPAD_BUTTON_CROSS = 0, GAMEPAD_BUTTON_CIRCLE = 1, GAMEPAD_BUTTON_SQUARE = 2,
    GAMEPAD_BUTTON_TRIANGLE = 3,
    GAMEPAD_AXIS_LEFT_X = 0, GAMEPAD_AXIS_LEFT_Y = 1, GAMEPAD_AXIS_RIGHT_X = 2,
    GAMEPAD_AXIS_RIGHT_Y = 3, GAMEPAD_AXIS_LEFT_TRIGGER = 4, GAMEPAD_AXIS_RIGHT_TRIGGER = 5,
    GAMEPAD_AXIS_LAST = 6
}

-- Letters, digits, function keys and numpad digits are contiguous in GLFW.
local function contiguous(prefix, first, list)
    for i, suffix in ipairs(list) do
        Input[prefix .. suffix] = first + i - 1
    end
end
contiguous("KEY_", 48, { "0", "1", "2", "3", "4", "5", "6", "7", "8", "9" })
contiguous("KEY_", 65, { "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N",
    "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z" })
contiguous("KEY_", 290, (function()
    local names = {}
    for i = 1, 25 do names[i] = "F" .. i end
    return names
end)())
contiguous("KEY_", 320, { "KP_0", "KP_1", "KP_2", "KP_3", "KP_4", "KP_5", "KP_6", "KP_7", "KP_8", "KP_9" })

local pressed = {}
local scrollY = 0.0
local gamepadAxes = {}
local gamepadButtons = {}

function Input.isKeyPressed(code) return pressed[code] == true end
function Input.isMousePressed(button) return pressed["m" .. tostring(button)] == true end
function Input.isTouch() return false end
function Input.isMouseEntered() return true end
function Input.getMousePosition() return Vector2(10.0, 20.0) end
function Input.getMouseScroll() return Vector2(0.0, scrollY) end
function Input.getTouchPosition() return Vector2(10.0, 20.0) end
function Input.getTouches() return {} end
function Input.numTouches() return 0 end
function Input.findTouchIndex() return 0 end
function Input.getModifiers() return 0 end
function Input.isGamepadConnected(id) return id == 0 end
function Input.getGamepadName() return "stub" end
function Input.numGamepads() return 1 end
function Input.getGamepadId() return 0 end
function Input.findGamepadIndex(id) if id == 0 then return 1 end return 0 end
function Input.isGamepadButtonPressed(id, button) return gamepadButtons[button] == true end
function Input.getGamepadAxis(id, axis) return gamepadAxes[axis] or 0.0 end

Log = {}
function Log.print() end
function Log.verbose() end
function Log.debug() end
function Log.warn() end
function Log.error() end

Engine = {
    deltatime = 1.0 / 60.0, systemTime = 0.0, useDegrees = true, maxDeltatime = 0.25,
    updateTime = 0, interpolationAlpha = 0.0, framerate = 60, canvasWidth = 1280,
    canvasHeight = 720, preferredCanvasWidth = 1280, preferredCanvasHeight = 720,
    platform = "win32", graphicBackend = "glcore", mouseCursor = 0, mouseMode = 0,
    asyncLoading = false
}
NULL_ENTITY = 0

-- ---------------------------------------------------------------- entities

local entities = {}
local destroyed = {}
local nextSynthetic = 1000

local function frameFor(id)
    if not id then
        nextSynthetic = nextSynthetic + 1
        id = nextSynthetic
    end
    entities[id] = entities[id] or {
        position = Vector3(0.0, 1.0, 0.0),
        rotation = Quaternion(0.0, 0.0, 0.0),
        scale = Vector3(1.0, 1.0, 1.0),
        visible = true
    }
    return entities[id], id
end

local function allocEntity()
    nextSynthetic = nextSynthetic + 1
    return nextSynthetic
end

-- ---------------------------------------------------------------- strict class factory
-- props is a member -> type-name map (the allowlist) and seed fills plausible defaults so a
-- component can read a property before it has written one, like the real engine would.

local defaultFor
defaultFor = function(kind)
    if kind == "float" or kind == "double" or kind == "int" or kind == "unsigned int"
        or kind == "size_t" or kind == "uint16_t" or kind == "uint32_t" or kind == "int16_t"
        or kind == "uint8_t" then
        return 0.0
    elseif kind == "bool" then
        return false
    elseif kind == "string" then
        return ""
    elseif kind == "vec2" then
        return Vector2(0.0, 0.0)
    elseif kind == "vec3" or kind == "color3" then
        return Vector3(0.0, 0.0, 0.0)
    elseif kind == "vec4" or kind == "color4" then
        return Vector4(0.0, 0.0, 0.0, 1.0)
    elseif kind == "quat" then
        return Quaternion(0.0, 0.0, 0.0)
    elseif kind == "list" then
        return {}
    else
        return nil
    end
end

local function strict(className, def)
    local methods = def.methods or {}
    local props = def.props or {}
    local state = def.state

    return setmetatable({}, {
        __call = function(_, scene, entity)
            if entity == nil then
                entity = allocEntity()
            elseif type(entity) == "table" then
                entity = entity.entity or entity
            end
            local store = state and state(entity) or {}
            for name, kind in pairs(props) do
                if store[name] == nil then
                    local seeded = def.seed and def.seed[name]
                    if seeded ~= nil then
                        store[name] = seeded
                    else
                        local value = defaultFor(kind)
                        if value ~= nil then store[name] = value end
                    end
                end
            end
            local self = { entity = entity, scene = scene, name = "" }
            return setmetatable(self, {
                __index = function(t, key)
                    local handler = methods[key]
                    if handler then
                        return function(_, ...)
                            if type(handler) == "function" then
                                return handler(t, store, ...)
                            end
                            return nil
                        end
                    end
                    if props[key] ~= nil then
                        local held = rawget(t, key)
                        if held ~= nil then return held end
                        return store[key]
                    end
                    local held = rawget(t, key)
                    if held ~= nil then return held end
                    error(string.format("%s has no member '%s' (not in the Lua binding)",
                        className, tostring(key)), 2)
                end,
                __newindex = function(t, key, value)
                    if methods[key] then
                        error(string.format("%s.%s is a method, it cannot be assigned",
                            className, tostring(key)), 2)
                    end
                    if props[key] == nil then
                        error(string.format("%s has no property '%s' (not in the Lua binding)",
                            className, tostring(key)), 2)
                    end
                    store[key] = value
                end
            })
        end
    })
end

-- ---------------------------------------------------------------- scene

local physicsSystem
local postProcessPasses = { { shader = "postprocess.frag", enabled = true } }

local scene = {}
function scene.find(_, key)
    if key == "Player" then return 2 end
    if key == "PlayerCamera" or key == "Camera" then return 1 end
    if key == "Water" or key == "Sea" then return 3 end
    return NULL_ENTITY
end
scene.findEntity = scene.find
-- Both are on the Scene binding (`CoreClassesLua.cpp`), and a script that asks them through pcall
-- would silently get nil if the stub did not answer: Fire's spread loop prunes every registry entry
-- when isEntityCreated is missing, which is the bug these two lines exist to keep visible.
function scene.isEntityCreated(_, id)
    return type(id) == "number" and id ~= NULL_ENTITY and not destroyed[id]
end
function scene.getEntityName(_, id)
    if id == 1 then return "Camera" end
    if id == 2 then return "Player" end
    if id == 3 then return "Water" end
    return "Entity" .. tostring(id)
end
function scene.getScene() return scene end
scene.getMainScene = scene.getScene
scene.getLastScene = scene.getScene
Engine.getScene = function() return scene end
Engine.getMainScene = Engine.getScene
Engine.getLastScene = Engine.getScene
function Engine.setScene() return nil end
function Engine.addSceneLayer() return nil end
function Engine.executeSceneOnce() return nil end
function Engine.removeScene() return nil end
function Engine.removeAllSceneLayers() return nil end
function Engine.removeAllScenes() return nil end
function Engine.isSceneRunning() return true end
function Engine.hasScenesToExecuteOnce() return false end
function Engine.pauseGameEvents() return nil end
function Engine.setCanvasSize() return nil end
function Engine.setCallTouchInMouseEvent() return nil end
function Engine.isUIEventReceived() return false end
function Engine.setUpdateTimeMS() return nil end
function Engine.setMousePosition() return nil end
function Engine.startAsyncThread() return nil end
function Engine.commitThreadQueue() return nil end
function Engine.endAsyncThread() return nil end
function Engine.isAsyncThread() return false end
function Engine.isViewLoaded() return true end
function Engine.setMaxResourceLoadingThreads() return nil end
function Engine.getQueuedResourceCount() return 0 end
function Engine.clearAllSubscriptions() return nil end
function Engine.clearComponentSubscriptions() return nil end
function Engine.removeSubscriptionsByTag() return nil end

-- The scene's own members, from the `## Scene` section of the generated API.
local sceneMembers = {
    findEntity = true, findEntityInParent = true, load = true, destroy = true, draw = true,
    update = true, updateCameraSize = true, setCamera = true, setBackgroundColor = true,
    getGlobalIlluminationColorLinear = true, setGlobalIllumination = true,
    setGlobalIlluminationColor = true, setGlobalIlluminationIntensity = true,
    getAmbientLight2DColorLinear = true, setAmbientLight2D = true, setAmbientLight2DColor = true,
    setAmbientLight2DIntensity = true, setGravity2D = true, setGravity3D = true,
    setFixedResolutionSize = true, setDefaultShaderUniform = true, getDefaultShaderUniform = true,
    removeDefaultShaderUniform = true, setPostProcessUniform = true, getPostProcessUniform = true,
    removePostProcessUniform = true, setPostProcessPassEnabled = true, isPostProcessPassEnabled = true,
    canReceiveUIEvents = true, enableUIEvents = true, setEnableUIEvents = true,
    isEnableUIEvents = true, setDefaultCustomShader = true, getDefaultCustomShader = true,
    getActionSystem = true, getAudioSystem = true, getMeshSystem = true, getPhysicsSystem = true,
    getRenderSystem = true, getUISystem = true, destroyEntity = true, getEntity = true,
    getChildren = true, getParent = true, addEntityChild = true, setEntityName = true,
    getEntityName = true, createEntity = true, createUserEntity = true, createSystemEntity = true
}
local sceneProps = { camera = "int", gravity = "vec3", gravity2D = "vec2", gravity3D = "vec3",
    postProcessPasses = "list", backgroundColor = "vec4", fixedResolutionWidth = "int",
    fixedResolutionHeight = "int", uiEvents = "bool", name = "string" }
local sceneStore = { camera = 1, gravity = Vector3(0.0, -9.81, 0.0),
    gravity2D = Vector2(0.0, -9.81), gravity3D = Vector3(0.0, -9.81, 0.0),
    postProcessPasses = postProcessPasses, backgroundColor = Vector4(0.0, 0.0, 0.0, 1.0),
    name = "Main" }

function scene.destroyEntity(_, id) destroyed[id] = true end
function scene.getPhysicsSystem() return physicsSystem end
function scene.getActionSystem() return {} end
function scene.getAudioSystem() return {} end
function scene.getMeshSystem() return {} end
function scene.getRenderSystem() return {} end
function scene.getUISystem() return {} end
function scene.getDefaultShaderUniform() return Vector4(0, 0, 0, 0) end
function scene.getPostProcessUniform() return Vector4(0, 0, 0, 0) end
function scene.isPostProcessPassEnabled() return true end
function scene.canReceiveUIEvents() return true end
function scene.isEnableUIEvents() return true end
function scene.getDefaultCustomShader() return "" end
function scene.getGlobalIlluminationColorLinear() return Vector3(0.1, 0.1, 0.12) end
function scene.getAmbientLight2DColorLinear() return Vector3(0.1, 0.1, 0.12) end
function scene.getEntities() return {} end
function scene.createUserEntity() return allocEntity() end
function scene.createEntity() return allocEntity() end
function scene.createSystemEntity() return allocEntity() end
function scene.getEntity() return NULL_ENTITY end
function scene.getChildren() return {} end
function scene.getParent() return NULL_ENTITY end

scene = setmetatable(scene, {
    __index = function(_, key)
        if sceneStore[key] ~= nil then return sceneStore[key] end
        if sceneMembers[key] then return nil end
        error(string.format("Scene has no member '%s' (not in the Lua binding)", tostring(key)), 2)
    end,
    __newindex = function(_, key, value)
        if sceneMembers[key] then
            error(string.format("Scene.%s is a method, it cannot be assigned", tostring(key)), 2)
        end
        if sceneProps[key] == nil then
            error(string.format("Scene has no property '%s' (not in the Lua binding)", tostring(key)), 2)
        end
        sceneStore[key] = value
    end
})

physicsSystem = strict("PhysicsSystem", {
    props = {
        gravity = "vec3", gravity2D = "vec2", gravity3D = "vec3", interpolation = "bool",
        pointsToMeterScale2D = "float", lock3DBodies = "bool", steppingWorld3D = "bool",
        beginContact2D = "list", endContact2D = "list", beginSensorContact2D = "list",
        endSensorContact2D = "list", hitContact2D = "list", preSolve2D = "list",
        shouldCollide2D = "list", onBodyActivated3D = "list", onBodyDeactivated3D = "list",
        onContactAdded3D = "list", onContactPersisted3D = "list", onContactRemoved3D = "list",
        shouldCollide3D = "list"
    },
    seed = { gravity = Vector3(0.0, -9.81, 0.0), gravity2D = Vector2(0.0, -9.81),
        gravity3D = Vector3(0.0, -9.81, 0.0), pointsToMeterScale2D = 64.0 },
    methods = {
        setGravity = true, setGravity2D = true, setGravity3D = true,
        createBody2D = true, removeBody2D = true, loadBody2D = true, destroyBody2D = true,
        destroyShape2D = true, destroyJoint2D = true, createBody3D = true, removeBody3D = true,
        loadBody3D = true, destroyBody3D = true, destroyShape3D = true, destroyJoint3D = true,
        addBroadPhaseLayer3D = true, getWorld = function() return physicsSystem end,
        loadDistanceJoint2D = true, loadRevoluteJoint2D = true, loadPrismaticJoint2D = true,
        loadMouseJoint2D = true, loadWheelJoint2D = true, loadWeldJoint2D = true,
        loadMotorJoint2D = true, loadFixedJoint3D = true, loadDistanceJoint3D = true,
        loadPointJoint3D = true, loadHingeJoint3D = true, loadConeJoint3D = true,
        loadPrismaticJoint3D = true, loadSwingTwistJoint3D = true, loadSixDOFJoint3D = true,
        loadPathJoint3D = true, loadGearJoint3D = true, loadRackAndPinionJoint3D = true,
        loadPulleyJoint3D = true
    }
})()

-- ---------------------------------------------------------------- wrappers

Object = strict("Object", {
    state = frameFor,
    props = {
        addChild = "list", removeChild = "list", removeParent = "list", moveToTop = "list",
        moveUp = "list", moveDown = "list", moveToBottom = "list", setPosition = "list",
        getWorldPosition = "list", setRotation = "list", getWorldRotation = "list",
        setScale = "list", getWorldScale = "list", setVisibleOnly = "list", setBillboard = "list",
        setBillboardRotation = "list", updateTransform = "list", getBody2D = "list",
        removeBody2D = "list", getBody3D = "list", removeBody3D = "list", getRay = "list",
        name = "string", scene = "list", entity = "int", entityOwned = "bool",
        position = "vec3", rotation = "quat", scale = "vec3", visible = "bool",
        billboard = "bool", billboardRotation = "quat", fakeBillboard = "bool",
        cylindricalBillboard = "bool", localMatrix = "list", modelMatrix = "list",
        normalMatrix = "list"
    },
    methods = {
        addChild = true, removeChild = true, removeParent = true, moveToTop = true, moveUp = true,
        moveDown = true, moveToBottom = true, updateTransform = true, setVisibleOnly = true,
        setBillboard = true, setBillboardRotation = true,
        setPosition = function(_, store, value)
            if type(value) ~= "table" or type(value.x) ~= "number" then
                error("Invalid object passed to Object:setPosition", 2)
            end
            store.position = value
        end,
        getWorldPosition = function(_, store) return store.position end,
        setRotation = function(_, store, value)
            if type(value) ~= "table" or type(value.x) ~= "number" then
                error("Invalid quaternion passed to Object:setRotation", 2)
            end
            store.rotation = value
        end,
        getWorldRotation = function(_, store) return store.rotation end,
        setScale = function(_, store, value) store.scale = value or store.scale end,
        getWorldScale = function(_, store) return store.scale end,
        getWorldDirection = function() return Vector3(0.0, 0.0, 1.0) end,
        getWorldRight = function() return Vector3(1.0, 0.0, 0.0) end,
        getWorldUp = function() return Vector3(0.0, 1.0, 0.0) end,
        getBody2D = function(_, store, o) return Body2D(o.scene, store == nil and 1 or nil) end,
        getBody3D = function(t) return Body3D(t.scene, t.entity) end,
        removeBody2D = true, removeBody3D = true,
        getRay = function(t) return Ray(t.position, Vector3(0.0, 0.0, 1.0)) end
    }
})

Camera = strict("Camera", {
    state = frameFor,
    props = {
        activate = "list", setOrtho = "list", setPerspective = "list", setType = "list",
        setTarget = "list", disableTarget = "list", isUsingTarget = "list", setUp = "list",
        getUp = "list", getDirection = "list", getRight = "list", getWorldTarget = "list",
        getWorldDirection = "list", getWorldUp = "list", getWorldRight = "list",
        getViewMatrix = "list", getProjectionMatrix = "list", getViewProjectionMatrix = "list",
        rotateView = "list", rotatePosition = "list", elevateView = "list", elevatePosition = "list",
        walkForward = "list", zoom = "list", slide = "list", slideForward = "list", slideUp = "list",
        getFramebuffer = "list", setFramebufferSize = "list", setFramebufferFilter = "list",
        screenToRay = "list", getDistanceFromTarget = "list", updateCamera = "list",
        autoResize = "bool", nearClip = "float", farClip = "float", leftClip = "float",
        rightClip = "float", bottomClip = "float", topClip = "float", aspect = "float",
        yfov = "float", type = "int", target = "vec3", up = "vec3", renderToTexture = "bool",
        transparentSort = "bool", depthTest = "bool", position = "vec3", rotation = "quat",
        scale = "vec3", visible = "bool", entity = "int", name = "string"
    },
    seed = { yfov = 60.0, nearClip = 0.1, farClip = 1000.0, target = Vector3(0, 0, 0),
        up = Vector3(0, 1, 0) },
    methods = {
        activate = true, setOrtho = true, setPerspective = true, setType = true,
        -- The binding registers only setTarget(float, float, float): the Vector3 setter is the
        -- "target" property, so a single Vector3 argument fails to decode inside the engine.
        setTarget = function(_, store, x, y, z)
            if type(x) ~= "number" or type(y) ~= "number" or type(z) ~= "number" then
                error("Camera:setTarget takes three numbers; use camera.target = Vector3", 2)
            end
            store.target = Vector3(x, y, z)
        end,
        disableTarget = true, isUsingTarget = function() return true end, setUp = true,
        getUp = function(_, store) return store.up end,
        getDirection = function() return Vector3(0.0, 0.0, 1.0) end,
        getRight = function() return Vector3(1.0, 0.0, 0.0) end,
        getWorldTarget = function(_, store) return store.target end,
        getWorldDirection = function() return Vector3(0.0, 0.0, 1.0) end,
        getWorldUp = function() return Vector3(0.0, 1.0, 0.0) end,
        getWorldRight = function() return Vector3(1.0, 0.0, 0.0) end,
        getViewMatrix = function() return Matrix4() end,
        getProjectionMatrix = function() return Matrix4() end,
        getViewProjectionMatrix = function() return Matrix4() end,
        rotateView = true, rotatePosition = true, elevateView = true, elevatePosition = true,
        walkForward = true, zoom = true, slide = true, slideForward = true, slideUp = true,
        getFramebuffer = function() return {} end, setFramebufferSize = true,
        setFramebufferFilter = true, updateCamera = true,
        screenToRay = function(t, store, point)
            return Ray(store.position, Vector3(point and 0.1 or 0.0, -0.2, 1.0):normalize())
        end,
        getDistanceFromTarget = function(_, store) return store.position:length() end
    }
})

Body3D = strict("Body3D", {
    state = frameFor,
    props = {
        getAttachedObject = "list", load = "list", createBoxShape = "list",
        createSphereShape = "list", createCapsuleShape = "list", createTaperedCapsuleShape = "list",
        createCylinderShape = "list", createConvexHullShape = "list", createMeshShape = "list",
        createHeightFieldShape = "list", getNumShapes = "list", getShapeType = "list",
        setShapeDensity = "list", getShapeDensity = "list", canBeKinematicOrDynamic = "list",
        activate = "list", deactivate = "list", setAllowedDOFsAll = "list",
        setAllowedDOFs2DPlane = "list", setAllowedDOFs = "list", setOverrideMassAndInertia = "list",
        setBitsFilter = "list", setLinearVelocityClamped = "list", setAngularVelocityClamped = "list",
        getPointVelocityCOM = "list", getPointVelocity = "list", getAccumulatedForce = "list",
        getAccumulatedTorque = "list", getInverseInertia = "list", applyForce = "list",
        applyTorque = "list", applyImpulse = "list", applyAngularImpulse = "list",
        applyBuoyancyImpulse = "list", getCenterOfMassPosition = "list",
        shapeDensity = "float", type = "int", motionQuality = "int", sensor = "bool",
        collideKinematicVsNonDynamic = "bool", mass = "float", gravityFactor = "float",
        buoyancy = "float", waterDrag = "float", waterAngularDrag = "float",
        collisionGroupID = "int", categoryBitsFilter = "int", maskBitsFilter = "int",
        allowSleeping = "bool", friction = "float", restitution = "float", position = "vec3",
        rotation = "quat", scale = "vec3", visible = "bool", linearVelocity = "vec3",
        angularVelocity = "vec3", entity = "int", name = "string"
    },
    seed = { type = BodyType.DYNAMIC, gravityFactor = 1.0, motionQuality = Body3DMotionQuality.DISCRETE,
        linearVelocity = Vector3(0, 0, 0), angularVelocity = Vector3(0, 0, 0) },
    methods = {
        load = function() return true end,
        createBoxShape = function() return 1 end, createSphereShape = function() return 1 end,
        createCapsuleShape = function() return 1 end, createTaperedCapsuleShape = function() return 1 end,
        createCylinderShape = function() return 1 end, createConvexHullShape = function() return 1 end,
        createMeshShape = function() return 1 end, createHeightFieldShape = function() return 1 end,
        getNumShapes = function() return 1 end, getShapeType = function() return 0 end,
        setShapeDensity = true, getShapeDensity = function() return 1.0 end,
        canBeKinematicOrDynamic = function() return true end,
        activate = true, deactivate = true, setAllowedDOFsAll = true, setAllowedDOFs2DPlane = true,
        setAllowedDOFs = true, setOverrideMassAndInertia = true, setBitsFilter = true,
        setLinearVelocityClamped = function(_, store, v) store.linearVelocity = v end,
        setAngularVelocityClamped = function(_, store, v) store.angularVelocity = v end,
        getPointVelocityCOM = function() return Vector3(0, 0, 0) end,
        getPointVelocity = function() return Vector3(0, 0, 0) end,
        getAccumulatedForce = function() return Vector3(0, 0, 0) end,
        getAccumulatedTorque = function() return Vector3(0, 0, 0) end,
        getInverseInertia = function() return Matrix4() end,
        applyForce = true, applyTorque = true, applyImpulse = true, applyAngularImpulse = true,
        applyBuoyancyImpulse = function() return true end,
        getCenterOfMassPosition = function(_, store) return store.position end,
        getAttachedObject = function(t) return Object(t.scene, t.entity) end
    }
})

Body2D = strict("Body2D", {
    state = frameFor,
    props = {
        getAttachedObject = "list", load = "list", createBoxShape = "list",
        createCenteredBoxShape = "list", createRoundedBoxShape = "list", createPolygonShape = "list",
        createCircleShape = "list", createCapsuleShape = "list", createSegmentShape = "list",
        createChainShape = "list", removeAllShapes = "list", getBodyContacts = "list",
        getShapeContacts = "list", getNumShapes = "list", getShapeType = "list",
        setShapeDensity = "list", getShapeDensity = "list", setShapeFriction = "list",
        getShapeFriction = "list", setShapeRestitution = "list", getShapeRestitution = "list",
        setShapeSensor = "list", isShapeSensor = "list", setShapeEnableHitEvents = "list",
        isShapeEnableHitEvents = "list", setShapeContactEvents = "list", isShapeContactEvents = "list",
        setShapePreSolveEvents = "list", isShapePreSolveEvents = "list", setShapeSensorEvents = "list",
        isShapeSensorEvents = "list", setBitsFilter = "list", setCategoryBitsFilter = "list",
        getCategoryBitsFilter = "list", setMaskBitsFilter = "list", getMaskBitsFilter = "list",
        setGroupIndexFilter = "list", getGroupIndexFilter = "list", getMass = "list",
        getRotationalInertia = "list", applyMassFromShapes = "list", applyForce = "list",
        applyForceToCenter = "list", applyTorque = "list", applyLinearImpulse = "list",
        applyLinearImpulseToCenter = "list", applyAngularImpulse = "list",
        pointsToMeterScale = "float", shapeDensity = "float", shapeFriction = "float",
        shapeRestitution = "float", shapeSensor = "bool", shapeEnableHitEvents = "bool",
        shapeContactEvents = "bool", shapePreSolveEvents = "bool", shapeSensorEvents = "bool",
        position = "vec2", angle = "float", linearVelocity = "vec2", angularVelocity = "float",
        linearDamping = "float", angularDamping = "float", enableSleep = "bool", awake = "bool",
        fixedRotation = "bool", bullet = "bool", type = "int", enabled = "bool",
        gravityScale = "float", categoryBitsFilter = "int", maskBitsFilter = "int",
        groupIndexFilter = "int", visible = "bool", scale = "vec3", rotation = "quat",
        entity = "int", name = "string"
    },
    seed = { type = BodyType.DYNAMIC, gravityScale = 1.0, pointsToMeterScale = 64.0,
        linearVelocity = Vector2(0, 0), awake = true },
    methods = {
        load = function() return true end, createBoxShape = function() return 1 end,
        createCenteredBoxShape = function() return 1 end, createRoundedBoxShape = function() return 1 end,
        createPolygonShape = function() return 1 end, createCircleShape = function() return 1 end,
        createCapsuleShape = function() return 1 end, createSegmentShape = function() return 1 end,
        createChainShape = function() return 1 end, removeAllShapes = true,
        getBodyContacts = function() return {} end, getShapeContacts = function() return {} end,
        getNumShapes = function() return 1 end, getShapeType = function() return 0 end,
        setShapeDensity = true, getShapeDensity = function() return 1.0 end,
        setShapeFriction = true, getShapeFriction = function() return 0.6 end,
        setShapeRestitution = true, getShapeRestitution = function() return 0.0 end,
        setShapeSensor = true, isShapeSensor = function() return false end,
        setShapeEnableHitEvents = true, isShapeEnableHitEvents = function() return false end,
        setShapeContactEvents = true, isShapeContactEvents = function() return false end,
        setShapePreSolveEvents = true, isShapePreSolveEvents = function() return false end,
        setShapeSensorEvents = true, isShapeSensorEvents = function() return false end,
        setBitsFilter = true, setCategoryBitsFilter = true, getCategoryBitsFilter = function() return 1 end,
        setMaskBitsFilter = true, getMaskBitsFilter = function() return 65535 end,
        setGroupIndexFilter = true, getGroupIndexFilter = function() return 0 end,
        getMass = function() return 1.0 end, getRotationalInertia = function() return 1.0 end,
        applyMassFromShapes = true, applyForce = true, applyForceToCenter = true, applyTorque = true,
        applyLinearImpulse = true, applyLinearImpulseToCenter = true, applyAngularImpulse = true,
        getAttachedObject = function(t) return Object(t.scene, t.entity) end
    }
})

Water = strict("Water", {
    state = frameFor,
    props = {
        load = "list", setSize = "list", setShallowColor = "list", setDeepColor = "list",
        setNormalTexture = "list", setFoamColor = "list", setShaderUniform = "list",
        getShaderUniform = "list", removeShaderUniform = "list", getHeight = "list",
        getNormal = "list", getSimWaterDepth = "list", getSimVelocity = "list",
        addDisturbance = "list", getSimVolume = "list", size = "vec2", subdivisions = "int",
        shallowColor = "color3", deepColor = "color3", depthFade = "float",
        waveHeight = "float", waveLength = "float", waveSpeed = "float", waveDirection = "float",
        waveSteepness = "float", normalScale = "float", normalStrength = "float",
        rippleSpeed = "float", reflectivity = "float", specularIntensity = "float",
        roughness = "float", foamColor = "color3", shoreFoam = "float", crestFoam = "float",
        receiveShadows = "bool", depthEffects = "bool", planarReflection = "bool",
        reflectionDistortion = "float", refraction = "bool", refractionDistortion = "float",
        caustics = "float", causticsScale = "float", underwater = "bool", buoyancy = "bool",
        buoyancyDepth = "float", simulate = "bool", simResolution = "int", simDepth = "float",
        simFlow = "float", simRain = "float", simEvaporation = "float", simDryDepth = "float",
        simEdgeDrain = "bool", simRipples = "bool", simRippleSpeed = "float",
        simRippleDamping = "float", simRippleStrength = "float", customShader = "string",
        customUnderwaterShader = "string", visible = "bool", position = "vec3", rotation = "quat",
        scale = "vec3", entity = "int", name = "string"
    },
    seed = { size = Vector2(50.0, 50.0), waveHeight = 0.2, waveLength = 6.0, waveSpeed = 1.0,
        waveDirection = 0.0, waveSteepness = 0.6, shoreFoam = 0.4, crestFoam = 0.15,
        buoyancy = true, underwater = true, refraction = true, depthEffects = true,
        receiveShadows = true, caustics = 0.5 },
    methods = {
        load = function() return true end, setSize = true, setShallowColor = true,
        setDeepColor = true, setNormalTexture = true, setFoamColor = true, setShaderUniform = true,
        getShaderUniform = function() return Vector4(0, 0, 0, 0) end,
        removeShaderUniform = function() return true end,
        -- A surface plane at the entity height: matches the world-space wave lookup.
        getHeight = function(_, store, x, z)
            if type(x) ~= "number" or type(z) ~= "number" then
                error("Water:getHeight needs (x, z)", 2)
            end
            local height = (store.position and store.position.y or 0.0)
            return height + math.sin((x or 0.0) * 0.3 + Engine.systemTime) * (store.waveHeight or 0.2) * 0.5
        end,
        getNormal = function() return Vector3(0.0, 1.0, 0.0) end,
        getSimWaterDepth = function(_, store, _x, _z)
            local base = store.simDepth
            if type(base) ~= "number" or base == 0.0 then base = 1.5 end
            return base
        end,
        getSimVelocity = function(_, store)
            local flow = store.simFlow or 0.0
            return Vector3(flow * 0.2, 0.0, 0.0)
        end,
        addDisturbance = true,
        getSimVolume = function() return 100.0 end
    }
})

Terrain = strict("Terrain", {
    state = frameFor,
    props = {
        createTerrain = "list", setHeightMap = "list", setBlendMap = "list", setBlendMapIndex = "list",
        setTextureLayer = "list", setTextureDetailRed = "list", setTextureDetailGreen = "list",
        setTextureDetailBlue = "list", setSurfaceLayer = "list", getSurfaceLayer = "list",
        setLayerFromMaterial = "list", removeSurfaceLayer = "list", getNumLayers = "list",
        size = "float", maxHeight = "float", resolution = "int", textureBaseTiles = "int",
        textureDetailTiles = "int", rootGridSize = "int", levels = "int", visible = "bool",
        position = "vec3", rotation = "quat", scale = "vec3", entity = "int", name = "string"
    },
    seed = { size = 200.0, maxHeight = 5.0, resolution = 32, levels = 6, rootGridSize = 2 },
    methods = {
        createTerrain = function() return true end, setHeightMap = true, setBlendMap = true,
        setBlendMapIndex = true, setTextureLayer = true, setTextureDetailRed = true,
        setTextureDetailGreen = true, setTextureDetailBlue = true, setSurfaceLayer = true,
        getSurfaceLayer = function() return TerrainSurfaceLayer() end, setLayerFromMaterial = true,
        removeSurfaceLayer = true, getNumLayers = function() return 3 end
    }
})

TerrainSurfaceLayer = strict("TerrainSurfaceLayer", {
    props = { texture = "string", scale = "float", normalScale = "float" },
    methods = {}
})

Sound = strict("Sound", {
    state = frameFor,
    props = {
        loadSound = "list", destroySound = "list", getObject = "list", play = "list", pause = "list",
        stop = "list", seek = "list", getLength = "list", getPlayingTime = "list",
        isPlaying = "list", isPaused = "list", isStopped = "list", setInaudibleBehavior = "list",
        setMinMaxDistance = "list", getSoundComponent = "list", sound3D = "bool",
        clockedSound = "bool", volume = "float", speed = "float", pan = "float", looping = "bool",
        loopingPoint = "float", protectVoice = "bool", minDistance = "float", maxDistance = "float",
        attenuationModel = "int", attenuationRolloffFactor = "float", dopplerFactor = "float",
        position = "vec3", rotation = "quat", scale = "vec3", visible = "bool",
        entity = "int", name = "string"
    },
    seed = { volume = 1.0, speed = 1.0, looping = false, sound3D = true, maxDistance = 100.0 },
    methods = {
        loadSound = function() return 0 end, destroySound = true,
        getObject = function(t) return Object(t.scene, t.entity) end,
        play = true, pause = true, stop = true, seek = true,
        getLength = function() return 4.0 end, getPlayingTime = function() return 0.0 end,
        isPlaying = function(_, store) return store.__playing == true end,
        isPaused = function(_, store) return store.__paused == true end,
        isStopped = function(_, store) return store.__playing ~= true and store.__paused ~= true end,
        setInaudibleBehavior = true, setMinMaxDistance = true, getSoundComponent = function() return {} end
    }
})

Particles = strict("Particles", {
    state = frameFor,
    props = {
        reset = "list", setLifeInitializer = "list", setPositionInitializer = "list",
        setSpherePositionInitializer = "list", setHemispherePositionInitializer = "list",
        setCirclePositionInitializer = "list", setConePositionInitializer = "list",
        addBurst = "list", setBursts = "list", clearBursts = "list", setPositionModifier = "list",
        setVelocityInitializer = "list", setVelocityModifier = "list",
        setAccelerationInitializer = "list", setAccelerationModifier = "list",
        setColorInitializer = "list", setColorModifier = "list", addColorGradientStop = "list",
        setColorGradient = "list", clearColorGradient = "list", setColorGradientUseSRGB = "list",
        setAlphaInitializer = "list", setAlphaModifier = "list", setSizeInitializer = "list",
        setSizeModifier = "list", setSpriteIntializer = "list", setSpriteModifier = "list",
        setRotationInitializer = "list", setRotationModifier = "list", setScaleInitializer = "list",
        setScaleModifier = "list", getParticlesomponent = "list", rate = "int",
        maxPerUpdate = "int", maxParticles = "int", emitter = "bool", loop = "bool", localSpace = "bool",
        emitterShape = "int", visible = "bool", position = "vec3", rotation = "quat",
        scale = "vec3", entity = "int", name = "string",
        -- Particles derives from Action in the binding, so the action's own surface is part of what a
        -- script may touch on a Particles handle; leaving it out of the allowlist made every build that
        -- wires `target` and then `restart()` fail inside its own pcall.
        ownedTarget = "bool", target = "int", setTarget = "list", speed = "float", weight = "float",
        startOffset = "float", randomStart = "bool", start = "list", pause = "list", stop = "list",
        restart = "list", isRunning = "list", isStopped = "list", isPaused = "list",
        getTimeCount = "list", getActionComponent = "list"
    },
    seed = { rate = 20, emitter = true, loop = true, maxParticles = 100 },
    methods = {
        reset = true, restart = function(_, store) store.__running = true end,
        stop = function(_, store) store.__running = false end,
        isRunning = function(_, store) return store.__running == true end,
        start = function(_, store) store.__running = true end,
        pause = function(_, store) store.__running = false end,
        isStopped = function(_, store) return store.__running ~= true end,
        isPaused = function(_, store) return store.__running ~= true end,
        getTimeCount = function() return 0 end, getActionComponent = function() return {} end,
        setTarget = true,
        setLifeInitializer = true, setPositionInitializer = true,
        setSpherePositionInitializer = true, setHemispherePositionInitializer = true,
        setCirclePositionInitializer = true, setConePositionInitializer = true, addBurst = true,
        setBursts = true, clearBursts = true, setPositionModifier = true,
        setVelocityInitializer = true, setVelocityModifier = true, setAccelerationInitializer = true,
        setAccelerationModifier = true, setColorInitializer = true, setColorModifier = true,
        addColorGradientStop = true, setColorGradient = true, clearColorGradient = true,
        setColorGradientUseSRGB = true, setAlphaInitializer = true, setAlphaModifier = true,
        setSizeInitializer = true, setSizeModifier = true, setSpriteIntializer = true,
        setSpriteModifier = true, setRotationInitializer = true, setRotationModifier = true,
        setScaleInitializer = true, setScaleModifier = true, getParticlesomponent = function() return {} end
    }
})

-- Points is what every sprite particle layer in the library is drawn from (Fire, Precipitation,
-- ImpactEffect, TrailEffect). Without this stub those builds failed inside their own pcall and the
-- harness still printed ok, which is the failure mode a stub must never have.
Points = strict("Points", {
    state = frameFor,
    props = {
        addPoint = "list", getPoint = "list", updatePoint = "list", removePoint = "list",
        isPointVisible = "list", setPointVisible = "list", updatePoints = "list",
        getNumPoints = "list", clearPoints = "list", addSpriteFrame = "list",
        removeSpriteFrame = "list", setTexture = "list", setShaderUniform = "list",
        getShaderUniform = "list", removeShaderUniform = "list",
        maxPoints = "int", transparent = "bool", autoTransparency = "bool", customShader = "string",
        visible = "bool", position = "vec3", rotation = "quat", scale = "vec3",
        entity = "int", name = "string"
    },
    seed = { maxPoints = 100, transparent = true },
    methods = {
        addPoint = function(_, store) store.__count = (store.__count or 0) + 1 return store.__count end,
        getPoint = function() return {} end, updatePoint = true, removePoint = true,
        isPointVisible = function() return true end, setPointVisible = true, updatePoints = true,
        getNumPoints = function(_, store) return store.__count or 0 end,
        clearPoints = function(_, store) store.__count = 0 end,
        addSpriteFrame = function() return 1 end, removeSpriteFrame = true, setTexture = true,
        setShaderUniform = true, getShaderUniform = function() return Vector4(0, 0, 0, 0) end,
        removeShaderUniform = true
    }
})

Light = strict("Light", {
    state = frameFor,
    props = {
        setType = "list", setDirection = "list", setColor = "list", setRange = "list",
        setIntensity = "list", setConeAngle = "list", setInnerConeAngle = "list",
        setOuterConeAngle = "list", setSpotMask = "list", clearSpotMask = "list",
        setShadows = "list", setShadowCameraNearFar = "list", setCameraNear = "list",
        setCameraFar = "list", setNumCascades = "list", setBias = "list", setShadowMapSize = "list",
        type = "int", direction = "vec3", color = "color3", range = "float", intensity = "float",
        innerConeAngle = "float", outerConeAngle = "float", spotMask = "list", shadows = "bool",
        cameraNear = "float", cameraFar = "float", automaticShadowCamera = "bool",
        numCascades = "float", bias = "float", shadowMapSize = "int", visible = "bool",
        position = "vec3", rotation = "quat", scale = "vec3", entity = "int", name = "string"
    },
    seed = { type = LightType.DIRECTIONAL, intensity = 1.0, direction = Vector3(-0.5, -1.0, -0.3),
        color = Vector3(1.0, 1.0, 1.0) },
    methods = {
        setType = true, setDirection = true, setColor = true, setRange = true, setIntensity = true,
        setConeAngle = true, setInnerConeAngle = true, setOuterConeAngle = true, setSpotMask = true,
        clearSpotMask = true, setShadows = true, setShadowCameraNearFar = true, setCameraNear = true,
        setCameraFar = true, setNumCascades = true, setBias = true, setShadowMapSize = true
    }
})

Fog = strict("Fog", {
    state = frameFor,
    props = {
        setColor = "list", setLinearStartEnd = "list", type = "int", color = "color3",
        density = "float", linearStart = "float", linearEnd = "float", visible = "bool",
        position = "vec3", rotation = "quat", scale = "vec3", entity = "int", name = "string"
    },
    seed = { type = FogType.LINEAR, color = Vector3(0.6, 0.7, 0.8), linearStart = 10.0, linearEnd = 200.0 },
    methods = { setColor = true, setLinearStartEnd = true }
})

SkyBox = strict("SkyBox", {
    state = frameFor,
    props = {
        setTextures = "list", setTexture = "list", setTexturePositiveX = "list",
        setTextureNegativeX = "list", setTexturePositiveY = "list", setTextureNegativeY = "list",
        setTexturePositiveZ = "list", setTextureNegativeZ = "list", setColor = "list",
        setShaderUniform = "list", getShaderUniform = "list", removeShaderUniform = "list",
        color = "color4", alpha = "float", rotation = "float", customShader = "string",
        visible = "bool", position = "vec3", scale = "vec3", entity = "int", name = "string"
    },
    seed = { color = Vector4(0.4, 0.6, 0.9, 1.0), alpha = 1.0 },
    methods = {
        setTextures = true, setTexture = true, setTexturePositiveX = true, setTextureNegativeX = true,
        setTexturePositiveY = true, setTextureNegativeY = true, setTexturePositiveZ = true,
        setTextureNegativeZ = true, setColor = true, setShaderUniform = true,
        getShaderUniform = function() return Vector4(0, 0, 0, 0) end, removeShaderUniform = true
    }
})

Joint3D = strict("Joint3D", {
    state = frameFor,
    props = {
        setFixedJoint = "list", setDistanceJoint = "list", setPointJoint = "list",
        setHingeJoint = "list", setConeJoint = "list", setPrismaticJoint = "list",
        setSwingTwistJoint = "list", setSixDOFJoint = "list", setPathJoint = "list",
        setGearJoint = "list", setRackAndPinionJoint = "list", setPulleyJoint = "list",
        getType = "list", enabled = "bool", entity = "int", name = "string", position = "vec3",
        rotation = "quat", scale = "vec3", visible = "bool"
    },
    methods = {
        setFixedJoint = true, setDistanceJoint = true, setPointJoint = true, setHingeJoint = true,
        setConeJoint = true, setPrismaticJoint = true, setSwingTwistJoint = true,
        setSixDOFJoint = true, setPathJoint = true, setGearJoint = true, setRackAndPinionJoint = true,
        setPulleyJoint = true, getType = function() return Joint3DType.POINT end
    }
})

Joint2D = strict("Joint2D", {
    state = frameFor,
    props = {
        setDistanceJoint = "list", setRevoluteJoint = "list", setPrismaticJoint = "list",
        setMouseJoint = "list", setWheelJoint = "list", setWeldJoint = "list",
        setMotorJoint = "list", getType = "list", enabled = "bool", entity = "int", name = "string",
        position = "vec2", rotation = "quat", scale = "vec3", visible = "bool"
    },
    methods = {
        setDistanceJoint = true, setRevoluteJoint = true, setPrismaticJoint = true,
        setMouseJoint = true, setWheelJoint = true, setWeldJoint = true, setMotorJoint = true,
        getType = function() return Joint2DType.DISTANCE end
    }
})

Text = strict("Text", {
    state = frameFor,
    props = {
        setFixedSize = "list", setFont = "list", getFont = "list", setColor = "list",
        getAscent = "list", getDescent = "list", getLineGap = "list", getLineHeight = "list",
        getNumChars = "list", getCharPosition = "list", getCharWidth = "list",
        setShaderUniform = "list", getShaderUniform = "list", removeShaderUniform = "list",
        getAABB = "list", getWorldAABB = "list", fixedWidth = "bool", fixedHeight = "bool",
        maxTextSize = "int", text = "string", font = "string", fontSize = "int", multiline = "bool",
        color = "color4", alpha = "float", flipY = "bool", customShader = "string",
        pivotBaseline = "bool", pivotCentered = "bool", visible = "bool", position = "vec3",
        rotation = "quat", scale = "vec3", entity = "int", name = "string"
    },
    seed = { text = "", fontSize = 16, alpha = 1.0, color = Vector4(1, 1, 1, 1) },
    methods = {
        setFixedSize = true, setFont = true, getFont = function() return "" end, setColor = true,
        getAscent = function() return 10.0 end, getDescent = function() return 2.0 end,
        getLineGap = function() return 2.0 end, getLineHeight = function() return 14 end,
        getNumChars = function(_, store) return #(store.text or "") end,
        getCharPosition = function() return Vector2(0, 0) end, getCharWidth = function() return 8.0 end,
        setShaderUniform = true, getShaderUniform = function() return Vector4(0, 0, 0, 0) end,
        removeShaderUniform = true, getAABB = function() return AABB() end,
        getWorldAABB = function() return AABB() end
    }
})

Image = strict("Image", {
    state = frameFor,
    props = {
        setPatchMargin = "list", setTexture = "list", setColor = "list", getAABB = "list",
        getWorldAABB = "list", getUIComponent = "list", setShaderUniform = "list",
        getShaderUniform = "list", removeShaderUniform = "list", patchMarginBottom = "int",
        patchMarginLeft = "int", patchMarginRight = "int", patchMarginTop = "int",
        textureScaleFactor = "float", color = "color4", alpha = "float", flipY = "bool",
        customShader = "string", visible = "bool", position = "vec3", rotation = "quat",
        scale = "vec3", entity = "int", name = "string"
    },
    seed = { color = Vector4(1, 1, 1, 1), alpha = 1.0 },
    methods = {
        setPatchMargin = true, setTexture = true, setColor = true, getAABB = function() return AABB() end,
        getWorldAABB = function() return AABB() end, getUIComponent = function() return {} end,
        setShaderUniform = true, getShaderUniform = function() return Vector4(0, 0, 0, 0) end,
        removeShaderUniform = true
    }
})

Panel = strict("Panel", {
    state = frameFor,
    props = {
        getHeaderImageObject = "list", getHeaderContainerObject = "list", getHeaderTextObject = "list",
        setTitleColor = "list", setHeaderColor = "list", setHeaderPatchMargin = "list",
        setHeaderTexture = "list", setHeaderMargin = "list", setMinSize = "list",
        setWindowProperties = "list", getPanelComponent = "list", title = "string",
        titleAnchorPreset = "int", titleColor = "color4", titleFont = "string", titleFontSize = "int",
        headerColor = "color4", headerMarginLeft = "int", headerMarginTop = "int",
        headerMarginRight = "int", headerMarginBottom = "int", minWidth = "int", minHeight = "int",
        resizeMargin = "int", canMove = "bool", canResize = "bool", canBringToFront = "bool",
        visible = "bool", position = "vec3", rotation = "quat", scale = "vec3",
        entity = "int", name = "string"
    },
    methods = {
        getHeaderImageObject = function(t) return Image(t.scene, t.entity) end,
        getHeaderContainerObject = function(t) return Container(t.scene, t.entity) end,
        getHeaderTextObject = function(t) return Text(t.scene, t.entity) end,
        setTitleColor = true, setHeaderColor = true, setHeaderPatchMargin = true,
        setHeaderTexture = true, setHeaderMargin = true, setMinSize = true, setWindowProperties = true,
        getPanelComponent = function() return {} end
    }
})

Button = strict("Button", {
    state = frameFor,
    props = {
        getLabelObject = "list", setLabelColor = "list", setTextureNormal = "list",
        setTextureHovered = "list", setTexturePressed = "list", setTextureDisabled = "list",
        setColorNormal = "list", setColorHovered = "list", setColorPressed = "list",
        setColorDisabled = "list", getButtonComponent = "list", label = "string",
        labelColor = "color4", labelFont = "string", labelFontSize = "int", colorNormal = "color4",
        colorHovered = "color4", colorPressed = "color4", colorDisabled = "color4",
        scaleHovered = "float", scalePressed = "float", transitionTime = "float", disabled = "bool",
        visible = "bool", position = "vec3", rotation = "quat", scale = "vec3",
        entity = "int", name = "string"
    },
    methods = {
        getLabelObject = function(t) return Text(t.scene, t.entity) end, setLabelColor = true,
        setTextureNormal = true, setTextureHovered = true, setTexturePressed = true,
        setTextureDisabled = true, setColorNormal = true, setColorHovered = true,
        setColorPressed = true, setColorDisabled = true, getButtonComponent = function() return {} end
    }
})

Progressbar = strict("Progressbar", {
    state = frameFor,
    props = {
        getFillObject = "list", setFillTexture = "list", setFillColor = "list",
        setFillPatchMargin = "list", setFillMargin = "list", getProgressbarComponent = "list",
        type = "int", value = "float", fillColor = "color4", fillAlpha = "float", visible = "bool",
        position = "vec3", rotation = "quat", scale = "vec3", entity = "int", name = "string"
    },
    seed = { value = 0.0 },
    methods = {
        getFillObject = function(t) return Image(t.scene, t.entity) end, setFillTexture = true,
        setFillColor = true, setFillPatchMargin = true, setFillMargin = true,
        getProgressbarComponent = function() return {} end
    }
})

Container = strict("Container", {
    state = frameFor,
    props = {
        resize = "list", setBoxExpand = "list", isBoxExpand = "list", type = "int",
        useAllWrapSpace = "bool", wrapCellWidth = "int", wrapCellHeight = "int",
        contentWidth = "int", contentHeight = "int", visible = "bool", position = "vec3",
        rotation = "quat", scale = "vec3", entity = "int", name = "string"
    },
    methods = { resize = true, setBoxExpand = true, isBoxExpand = function() return false end }
})

UILayout = strict("UILayout", {
    state = frameFor,
    props = {
        setSize = "list", setAnchorPoints = "list", setAnchorOffsets = "list", setPivot = "list",
        getUILayoutComponent = "list", width = "int", height = "int", anchorPointLeft = "float",
        anchorPointTop = "float", anchorPointRight = "float", anchorPointBottom = "float",
        anchorOffsetLeft = "int", anchorOffsetTop = "int", anchorOffsetRight = "int",
        anchorOffsetBottom = "int", positionXOffset = "int", positionYOffset = "int",
        positionOffset = "vec2", pivot = "vec2", anchorPreset = "int", usingAnchors = "bool",
        ignoreScissor = "bool", visible = "bool", position = "vec3", rotation = "quat",
        scale = "vec3", entity = "int", name = "string"
    },
    methods = { setSize = true, setAnchorPoints = true, setAnchorOffsets = true, setPivot = true,
        getUILayoutComponent = function() return {} end }
})

Model = strict("Model", {
    state = frameFor,
    props = {
        loadOBJ = "list", loadGLTF = "list", loadModel = "list", getAnimation = "list",
        findAnimation = "list", playAnimation = "list", stopAnimations = "list", getBone = "list",
        getMorphWeight = "list", setMorphWeight = "list", resetToBindPose = "list",
        visible = "bool", position = "vec3", rotation = "quat", scale = "vec3",
        entity = "int", name = "string"
    },
    methods = {
        loadOBJ = function() return true end, loadGLTF = function() return true end,
        loadModel = function() return true end,
        getAnimation = function(t, store, index) return Animation(t.scene, t.entity, index) end,
        findAnimation = function(t) return Animation(t.scene, t.entity) end,
        playAnimation = true, stopAnimations = true, getBone = function() return {} end,
        getMorphWeight = function() return 0.0 end, setMorphWeight = true, resetToBindPose = true
    }
})

Animation = strict("Animation", {
    state = frameFor,
    props = {
        fadeIn = "list", fadeOut = "list", addActionFrame = "list", getActionFrameSize = "list",
        getActionFrame = "list", setActionFrameStartTime = "list", setActionFrameDuration = "list",
        setActionFrameEntity = "list", loop = "bool", ownedActions = "bool", blendWeight = "float",
        defaultFadeTime = "float", entity = "int", name = "string"
    },
    methods = {
        fadeIn = true, fadeOut = true, addActionFrame = true, getActionFrameSize = function() return 0 end,
        getActionFrame = function() return {} end, setActionFrameStartTime = true,
        setActionFrameDuration = true, setActionFrameEntity = true
    }
})

Sprite = strict("Sprite", {
    state = frameFor,
    props = {
        getOccluder2D = "list", removeOccluder2D = "list", setSize = "list", setTextureRect = "list",
        addFrame = "list", removeFrame = "list", setFrame = "list", startAnimation = "list",
        pauseAnimation = "list", stopAnimation = "list", resumeAnimation = "list",
        createSprite = "list", width = "int", height = "int", flipY = "bool",
        textureScaleFactor = "float", textureRect = "list", pivotPreset = "int", visible = "bool",
        position = "vec3", rotation = "quat", scale = "vec3", entity = "int", name = "string"
    },
    methods = {
        getOccluder2D = function() return {} end, removeOccluder2D = true, setSize = true,
        setTextureRect = true, addFrame = function() return 1 end, removeFrame = true, setFrame = true,
        startAnimation = true, pauseAnimation = true, stopAnimation = true, resumeAnimation = true,
        createSprite = function() return true end
    }
})

SpriteAnimation = strict("SpriteAnimation", {
    state = frameFor,
    props = {
        setAnimation = "list", entity = "int", name = "string", visible = "bool", position = "vec3",
        rotation = "quat", scale = "vec3"
    },
    methods = { setAnimation = true }
})

Tilemap = strict("Tilemap", {
    state = frameFor,
    props = {
        findRectByString = "list", findTileByString = "list", addRect = "list", removeRect = "list",
        clearRects = "list", getRect = "list", addTile = "list", removeTile = "list",
        clearTiles = "list", getTile = "list", getWidth = "list", getHeight = "list",
        clearAll = "list", textureScaleFactor = "float", reserveTiles = "int", visible = "bool",
        position = "vec3", rotation = "quat", scale = "vec3", entity = "int", name = "string"
    },
    methods = {
        findRectByString = function() return nil end, findTileByString = function() return nil end,
        addRect = true, removeRect = true, clearRects = true, getRect = function() return nil end,
        addTile = true, removeTile = true, clearTiles = true, getTile = function() return nil end,
        getWidth = function() return 10 end, getHeight = function() return 10 end, clearAll = true
    }
})

Mesh = strict("Mesh", {
    state = frameFor,
    props = {
        setTexture = "list", setColor = "list", setMaterial = "list", getMaterial = "list",
        setPrimitiveType = "list", getPrimitiveType = "list", setFaceCulling = "list",
        isFaceCulling = "list", setCastShadowsWithTexture = "list", isCastShadowsWithTexture = "list",
        getAABB = "list", getVerticesAABB = "list", getWorldAABB = "list", getNumSubmeshes = "list",
        setShaderUniform = "list", getShaderUniform = "list", removeShaderUniform = "list",
        setAsMirror = "list", removeMirror = "list", isMirror = "list", createInstancedMesh = "list",
        removeInstancedMesh = "list", hasInstancedMesh = "list", setInstancedBillboard = "list",
        setFadeRange = "list", addInstance = "list", getInstance = "list", updateInstance = "list",
        removeInstance = "list", isInstanceVisible = "list", setInstanceVisible = "list",
        updateInstances = "list", getNumInstances = "list", clearInstances = "list",
        color = "color4", alpha = "float", material = "list", primitiveType = "int",
        faceCulling = "bool", castShadowsWithTexture = "bool", cullingMode = "int",
        windingOrder = "int", receiveLights = "bool", receiveIBL = "bool", castShadows = "bool",
        receiveShadows = "bool", lodEnabled = "bool", lodBias = "float",
        renderInReflectionProbes = "bool", shadowsBillboard = "bool", transparent = "bool",
        autoTransparency = "bool", customShader = "string", customDepthShader = "string",
        instancedBillboard = "bool", instancedCylindricalBillboard = "bool", maxInstances = "int",
        distanceFade = "bool", fadeStart = "float", fadeEnd = "float", cullDistance = "float",
        cullInstances = "bool", visible = "bool", position = "vec3", rotation = "quat",
        scale = "vec3", entity = "int", name = "string"
    },
    methods = {
        setTexture = true, setColor = true, setMaterial = true, getMaterial = function() return {} end,
        setPrimitiveType = true, getPrimitiveType = function() return 0 end, setFaceCulling = true,
        isFaceCulling = function() return true end, setCastShadowsWithTexture = true,
        isCastShadowsWithTexture = function() return false end, getAABB = function() return AABB() end,
        getVerticesAABB = function() return AABB() end, getWorldAABB = function() return AABB() end,
        getNumSubmeshes = function() return 1 end, setShaderUniform = true,
        getShaderUniform = function() return Vector4(0, 0, 0, 0) end, removeShaderUniform = true,
        setAsMirror = true, removeMirror = true, isMirror = function() return false end,
        createInstancedMesh = true, removeInstancedMesh = true, hasInstancedMesh = function() return false end,
        setInstancedBillboard = true, setFadeRange = true, addInstance = function() return 1 end,
        getInstance = function() return {} end, updateInstance = true, removeInstance = true,
        isInstanceVisible = function() return true end, setInstanceVisible = true,
        updateInstances = true, getNumInstances = function() return 0 end, clearInstances = true
    }
})

Mirror = strict("Mirror", {
    state = frameFor,
    props = { setNormal = "list", normal = "vec3", visible = "bool", position = "vec3",
        rotation = "quat", scale = "vec3", entity = "int", name = "string" },
    methods = { setNormal = true }
})

-- Ray is a value type: Ray(), Ray(Vector3, Vector3), and intersects() over the scene with the
-- RayFilter the LineOfSight style components need.
local rayReturnShape = { hit = true, distance = 40.0, point = Vector3(0, 0, 0),
    normal = Vector3(0.0, 1.0, 0.0), body = 2, shapeIndex = 0 }

local function makeRay(origin, direction)
    local ray = { origin = origin or Vector3(0.0, 0.0, 0.0),
        direction = (direction or Vector3(0.0, 0.0, 1.0)):normalized() }
    function ray:getPoint(distance)
        return self.origin + self.direction * (distance or 0.0)
    end
    function ray:intersects()
        local hit = { hit = rayReturnShape.hit, distance = rayReturnShape.distance,
            point = self:getPoint(rayReturnShape.distance), normal = rayReturnShape.normal,
            body = rayReturnShape.body, shapeIndex = rayReturnShape.shapeIndex }
        return setmetatable(hit, {
            __index = function(_, key)
                error(string.format("RayReturn has no member '%s'", tostring(key)), 2)
            end
        })
    end
    return setmetatable(ray, {
        __index = function(t, key)
            if rawget(t, key) ~= nil then return rawget(t, key) end
            error(string.format("Ray has no member '%s' (not in the Lua binding)", tostring(key)), 2)
        end
    })
end

Ray = setmetatable({}, {
    __call = function(_, origin, direction) return makeRay(origin, direction) end
})
Ray.NO_HIT = { hit = false, distance = 0.0, point = Vector3(0, 0, 0), normal = Vector3(0, 1, 0),
    body = NULL_ENTITY, shapeIndex = 0 }

-- ---------------------------------------------------------------- global managers

SceneManager = {
    registerScene = function() return nil end,
    loadScene = function(name) return true end,
    addChildScene = function(id) return true end,
    removeChildScene = function(id) return true end,
    getSceneId = function(name) return 1 end,
    getSceneName = function(id) return "Main" end,
    getSceneNames = function() return { "Main" } end,
    setLoadingScene = function(name) return true end,
    holdLoading = function() return true end,
    releaseLoading = function() return nil end,
    isCoveredByLoading = function(s) return false end,
    preloadScene = function(name) return true end,
    getPreloadProgress = function(name) return 0.5 end,
    cancelPreload = function(name) return nil end,
    clearAll = function() return nil end,
    setScenePtr = function(id, s) return nil end,
    getScenePtr = function(id) return scene end,
    removeScenePtr = function(id) return nil end,
    sceneCount = 1, currentSceneId = 1, currentSceneName = "Main", loadPending = false,
    loadingSceneId = 0
}

EntityManager = strict("EntityManager", {
    props = {
        createUserEntity = "list", createSystemEntity = "list", destroy = "list",
        setSignature = "list", getSignature = "list", setName = "list", getName = "list",
        isCreated = "list", recreateEntity = "list", getLastUserEntity = "list"
    },
    methods = {
        createUserEntity = function() return allocEntity() end,
        createSystemEntity = function() return allocEntity() end,
        destroy = true, setSignature = true, getSignature = function() return 0 end,
        setName = true, getName = function() return "" end, isCreated = function() return true end,
        recreateEntity = function() return true end, getLastUserEntity = function() return 1 end
    }
})

EntityRegistry = strict("EntityRegistry", {
    props = {
        createEntity = "list", createUserEntity = "list", createSystemEntity = "list",
        recreateEntity = "list", isEntityCreated = "list", destroyEntity = "list",
        setEntityName = "list", getEntityName = "list", findEntity = "list",
        findEntityInParent = "list", getSignature = "list", addEntityChild = "list",
        moveChildToIndex = "list", moveChildToTop = "list"
    },
    methods = {
        createEntity = function() return allocEntity() end,
        createUserEntity = function() return allocEntity() end,
        createSystemEntity = function() return allocEntity() end,
        recreateEntity = function() return true end, isEntityCreated = function() return true end,
        destroyEntity = true, setEntityName = true, getEntityName = function() return "" end,
        findEntity = function(_, store, name) return scene.findEntity(name) end,
        findEntityInParent = function() return NULL_ENTITY end, getSignature = function() return 0 end,
        addEntityChild = true, moveChildToIndex = true, moveChildToTop = true
    }
})

-- ---------------------------------------------------------------- events

local subscribed = {}
function RegisterEngineEvent(instance, name)
    subscribed[instance.entity] = subscribed[instance.entity] or {}
    subscribed[instance.entity][name] = true
end

-- ---------------------------------------------------------------- run

local order = {
    -- the original 19
    "FollowCamera", "OrbitCamera", "CameraShake", "CharacterController3D", "PlayerController", "VehicleController", "PhysicsMover3D",
    "TopDownController", "PlatformerController2D", "Rotator", "Bobber", "LookAt", "Health",
    "Projectile", "Spawner", "TriggerZone", "Timer", "Lifetime", "Pickup", "HudValue", "HudBar",
    -- water and shore
    "WaterWaves", "BeachShore", "FloatOnWater", "SwimController", "WaterRipples",
    -- physics
    "Rope", "Slingshot", "FallingObject", "GravityZone", "PhysicsGrabber",
    "CapsuleCollider", "MeshCollider",
    -- environment and movement
    "DayNightCycle", "WeatherSystem", "Lightning", "Precipitation", "WindZone", "WindForceController", "WindForceController2D", "FloatOnWater2D", "MovePlatform", "GamepadMover",
    "Fire", "Burnable",
    -- interaction and flow
    "Interactable", "ScenePortal", "Camera2DFollow", "PatrolPath", "LineOfSight", "DamageArea",
    -- audio and particles
    "AmbientSound", "SoundOnEvent", "MusicPlayer", "ParticleBurst",
    -- gfx
    "ScreenFade", "PostEffectPulse", "ImpactEffect", "TrailEffect",
    -- characters
    "AvatarPart", "HairSwing"
}

local entityIds = {}
for i, name in ipairs(order) do entityIds[name] = i + 1 end

local failures = 0
local report = {}
local checked = 0

-- The engine builds Lua 5.4 without LUA_COMPAT_MATHLIB, so these are nil there even though a
-- distribution interpreter (this one is 5.4.8 with the compat names kept) still answers to them.
local removed = {
    "math.atan2", "math.pow", "math.cosh", "math.sinh", "math.tanh", "math.frexp", "math.ldexp",
    "math.log10", "table.getn", "table.foreach", "table.foreachi", "loadstring", "setfenv", "getfenv",
}

local function usesRemovedApi(path, name)
    local file = io.open(path, "r")
    if not file then
        return false
    end
    local source = file:read("a")
    file:close()
    for _, api in ipairs(removed) do
        if source:find(api, 1, true) then
            print(string.format("FAIL %-24s %s does not exist in the engine's Lua 5.4", name, api))
            return true
        end
    end
    return false
end

-- Burnable reaches for Fire through `require` (inside a pcall, at ignite time). A project has the
-- library on the Lua path; here the same file is loaded out of the library directory, so a component
-- built on top of another component is exercised instead of silently degrading to "no visuals".
-- It raises for an unknown name exactly like real `require` does.
local requiredModules = {}
require = function(name)
    local leaf = string.match(tostring(name), "([^%.]+)$") or tostring(name)
    local hit = requiredModules[leaf]
    if hit ~= nil then return hit end
    local chunk = loadfile(dir .. "\\" .. leaf .. ".lua")
    if not chunk then
        error("module '" .. tostring(name) .. "' not found", 2)
    end
    local ok, result = pcall(chunk)
    if not ok or type(result) ~= "table" then
        error("module '" .. tostring(name) .. "' is not a component", 2)
    end
    requiredModules[leaf] = result
    return result
end

for _, name in ipairs(order) do
    if not onlyFilter or onlyFilter[name] then
        checked = checked + 1
        local path = dir .. "\\" .. name .. ".lua"
        local module, loadError = loadfile(path)
        if not module then
            print(string.format("MISSING %-22s %s", name, tostring(loadError)))
            failures = failures + 1
        elseif usesRemovedApi(path, name) then
            failures = failures + 1
        else
            local okLoad, loaded = pcall(module)
            if not okLoad or type(loaded) ~= "table" then
                print(string.format("FAIL %-24s the file returns %s, not a table", name, type(loaded)))
                failures = failures + 1
            else
                local properties = loaded.properties
                local okProps = type(properties) == "table" and #properties > 0
                for _, prop in ipairs(properties or {}) do
                    if not prop.name or not prop.type or prop.displayName == nil then
                        okProps = false
                    end
                    if prop.default == nil then
                        print(string.format("  note %s.%s has no default", name, tostring(prop.name)))
                    end
                    if type(prop.default) == "function" then
                        print(string.format("  FAIL %s.%s default is computed", name, tostring(prop.name)))
                        okProps = false
                    end
                end

                if not okProps then
                    print(string.format("FAIL %-24s the properties table is malformed", name))
                    failures = failures + 1
                end

                local id = entityIds[name]
                local instance = setmetatable({ __name = name, scene = scene, entity = id },
                    { __index = loaded })
                for _, prop in ipairs(properties or {}) do
                    instance[prop.name] = prop.default
                end

                local ok, err = pcall(function() instance:init() end)
                if not ok then
                    print(string.format("FAIL %-24s init: %s", name, err))
                    failures = failures + 1
                else
                    for frame = 1, 240 do
                        Engine.deltatime = 1.0 / 60.0
                        Engine.systemTime = Engine.systemTime + Engine.deltatime
                        pressed[Input.KEY_W] = frame % 90 < 45
                        pressed[Input.KEY_A] = frame % 60 < 20
                        pressed[Input.KEY_SPACE] = frame % 100 < 3
                        pressed[Input.KEY_LEFT_SHIFT] = frame % 30 < 10
                        pressed[Input.KEY_E] = frame % 110 < 3
                        pressed["m" .. Input.MOUSE_BUTTON_LEFT] = frame % 80 < 20
                        pressed["m" .. Input.MOUSE_BUTTON_RIGHT] = frame % 80 < 20
                        pressed[Input.MOUSE_BUTTON_LEFT] = frame % 80 < 20
                        gamepadButtons[Input.GAMEPAD_BUTTON_A] = frame % 100 < 3
                        gamepadAxes[Input.GAMEPAD_AXIS_LEFT_X] = math.sin(frame * 0.1)
                        gamepadAxes[Input.GAMEPAD_AXIS_LEFT_Y] = math.cos(frame * 0.1)
                        if frame % 70 == 0 then scrollY = 1.0 end
                        if frame % 97 == 0 then scrollY = -1.0 end
                        local subscribedNames = subscribed[id] or {}
                        if subscribedNames["onFixedUpdate"] then
                            ok, err = pcall(function() instance:onFixedUpdate() end)
                            if not ok then break end
                        end
                        if subscribedNames["onUpdate"] then
                            ok, err = pcall(function() instance:onUpdate() end)
                            if not ok then break end
                        end
                        if not ok then break end
                    end
                    if not ok then
                        print(string.format("FAIL %-24s update: %s", name, err))
                        failures = failures + 1
                    else
                        local events = {}
                        for key in pairs(subscribed[id] or {}) do events[#events + 1] = key end
                        table.sort(events)
                        report[#report + 1] = string.format("ok   %-24s %2d properties  events: %s",
                            name, #(properties or {}), table.concat(events, ", "))
                    end
                end
            end
        end
    end
end

for _, line in ipairs(report) do print(line) end
print(string.format("%d components checked, %d failures", checked, failures))
os.exit(failures == 0 and 0 or 1)
