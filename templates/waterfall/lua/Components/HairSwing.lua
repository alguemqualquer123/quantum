-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Make an attached part swing: hair trails when the head turns, a cape keeps going after the body
-- stops, an earring keeps ringing. The part is rigid - the shipped hair binds to a single joint -
-- so the honest model is a spherical pendulum around the bone it hangs from, not a strand chain.
-- The spring state lives in world space, which is what produces the lag: the bone moves, the rest
-- target moves with it, and the part's orientation follows behind.
-- Put this on the same entity as AvatarPart (mode "attach"), or on any entity that hangs off a
-- transform you want it to lag behind.
-- category: Characters
local HairSwing = {
    properties = {
        { name = "enabled", displayName = "Enabled", type = "bool", default = true },
        { name = "stiffness", displayName = "Spring stiffness (1/s^2)", type = "float", default = 42.0 },
        { name = "damping", displayName = "Damping (1/s)", type = "float", default = 5.0 },
        { name = "droop", displayName = "Gravity droop (rad/s^2)", type = "float", default = 6.0 },
        { name = "hangAxis", displayName = "Axis the part hangs along", type = "vector3", default = { 0.0, -1.0, 0.0 } },
        { name = "wind", displayName = "Wind direction", type = "vector3", default = { 0.0, 0.0, -1.0 } },
        { name = "windStrength", displayName = "Wind strength (rad/s^2)", type = "float", default = 0.0 },
        { name = "flutter", displayName = "Wind flutter (0 = steady, 1 = full swing)", type = "float", default = 0.0 },
        { name = "flutterSpeed", displayName = "Flutter rate (Hz)", type = "float", default = 1.7 },
        { name = "maxAngle", displayName = "Max deviation (degrees)", type = "float", default = 45.0 },
        { name = "maxSpeed", displayName = "Max angular speed (rad/s)", type = "float", default = 18.0 },
        { name = "startAfter", displayName = "Settle time before it starts (s)", type = "float", default = 0.25 }
    }
}

local RAD = math.pi / 180.0
local DOWN = { x = 0.0, y = -1.0, z = 0.0 }

local function num(value, fallback)
    if type(value) == "number" and value == value then return value end
    return fallback
end

-- Vector rows reach Lua as Vector3 userdata from the engine and as a table from the Inspector
-- default, and the swing integrates on plain numbers, so both shapes become one here.
local function asVec(value, fallback)
    local ok, out = pcall(function()
        if type(value) == "table" then
            if type(value.x) == "number" then return { x = value.x, y = value.y or 0.0, z = value.z or 0.0 } end
            if type(value[1]) == "number" then return { x = value[1], y = value[2] or 0.0, z = value[3] or 0.0 } end
            return fallback
        end
        if type(value) == "userdata" and type(value.x) == "number" then
            return { x = value.x, y = value.y, z = value.z }
        end
        return fallback
    end)
    if ok and out then return out end
    return fallback
end

local function warn(self, message)
    if Engine.systemTime - (self.lastWarn or -1.0) > 1.0 then
        self.lastWarn = Engine.systemTime
        print("[HairSwing] " .. message)
    end
end

-- Quaternion and vector algebra on plain tables. The engine's Quaternion has no scalar multiply
-- bound to Lua and its euler constructors read Engine.useDegrees, so the integration keeps its own
-- numbers in radians and crosses into userdata only at the two edges of the frame.
local function qid()
    return { w = 1.0, x = 0.0, y = 0.0, z = 0.0 }
end

local function qof(source)
    local ok, out = pcall(function()
        if type(source) ~= "table" and type(source) ~= "userdata" then return nil end
        if type(source.w) ~= "number" then return nil end
        return { w = source.w, x = source.x or 0.0, y = source.y or 0.0, z = source.z or 0.0 }
    end)
    if not ok or not out then return qid() end
    -- Skinned animation blends quaternions, so a read from the engine is not guaranteed to be unit
    -- length - and every step below assumes it is: log()/exp() only mean an angle on a unit quat.
    local n = math.sqrt(out.w * out.w + out.x * out.x + out.y * out.y + out.z * out.z)
    if n < 1e-8 then return qid() end
    return { w = out.w / n, x = out.x / n, y = out.y / n, z = out.z / n }
end

local function qmul(a, b)
    return {
        w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
        x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w
    }
end

local function qconj(a)
    return { w = a.w, x = -a.x, y = -a.y, z = -a.z }
end

local function qnormalize(a)
    local n = math.sqrt(a.w * a.w + a.x * a.x + a.y * a.y + a.z * a.z)
    if n < 1e-8 then return qid() end
    return { w = a.w / n, x = a.x / n, y = a.y / n, z = a.z / n }
end

-- log of a unit quaternion: the pure part is axis * (angle / 2), the same convention the engine's
-- Quaternion::log uses, so exp(log(q)) == q here too.
-- The engine embeds Lua 5.4 built without LUA_COMPAT_MATHLIB, so the old two-argument alias is gone
-- and math.atan itself already takes (y, x) - which is what this wraps.
local function atan2(y, x)
    return math.atan(y, x)
end

local function qlog(a)
    local magnitude = math.sqrt(a.x * a.x + a.y * a.y + a.z * a.z)
    if magnitude < 1e-8 then return { x = 0.0, y = 0.0, z = 0.0 } end
    local scale = atan2(magnitude, a.w) / magnitude
    return { x = a.x * scale, y = a.y * scale, z = a.z * scale }
end

local function qexp(v)
    local half = math.sqrt(v.x * v.x + v.y * v.y + v.z * v.z)
    if half < 1e-8 then return qid() end
    local scale = math.sin(half) / half
    return { w = math.cos(half), x = v.x * scale, y = v.y * scale, z = v.z * scale }
end

local function qrotate(a, v)
    local rotated = qmul(qmul(a, { w = 0.0, x = v.x, y = v.y, z = v.z }), qconj(a))
    return { x = rotated.x, y = rotated.y, z = rotated.z }
end

local function vlen(v)
    return math.sqrt(v.x * v.x + v.y * v.y + v.z * v.z)
end

local function vnormalized(v, fallback)
    local n = vlen(v)
    if n < 1e-8 then return fallback end
    return { x = v.x / n, y = v.y / n, z = v.z / n }
end

local function vcross(a, b)
    return { x = a.y * b.z - a.z * b.y, y = a.z * b.x - a.x * b.z, z = a.x * b.y - a.y * b.x }
end

-- The part's own transform, flattened out of the engine's userdata. Only the change test reads this
-- shape, so the rows are named after what they hold: the spring's numbers always come from qof().
local function localSnapshot(object)
    local position = object.position
    local rotation = object.rotation
    local scale = object.scale
    return {
        x = num(position.x, 0.0), y = num(position.y, 0.0), z = num(position.z, 0.0),
        w = num(rotation.w, 1.0), rx = num(rotation.x, 0.0), ry = num(rotation.y, 0.0),
        rz = num(rotation.z, 0.0), sx = num(scale.x, 1.0), sy = num(scale.y, 1.0),
        sz = num(scale.z, 1.0)
    }
end

local function sameSnapshot(a, b)
    if not a or not b then return false end
    return a.x == b.x and a.y == b.y and a.z == b.z and a.w == b.w and a.rx == b.rx and
        a.ry == b.ry and a.rz == b.rz and a.sx == b.sx and a.sy == b.sy and a.sz == b.sz
end

function HairSwing:object()
    local ok, object = pcall(function()
        return Object(self.scene, self.entity)
    end)
    if ok and object then return object end
    return nil
end

function HairSwing:init()
    RegisterEngineEvent(self, "onUpdate")

    self.state = qid()
    self.omega = { x = 0.0, y = 0.0, z = 0.0 }
    self.localWritten = nil
    self.bindLocal = nil
    self.stillSince = Engine.systemTime
    self.stillLocal = nil
    self.lastWarn = -1.0
    self.settled = false
    self.angle = 0.0

    local object = self:object()
    if object then
        self.localWritten = qof(object.rotation)
    else
        warn(self, "the entity needs a Transform component")
    end
end

-- The bone's world orientation is never read directly: `world == parent * local`, so
-- `world * inverse(local)` recovers it exactly, whatever animated it this frame. That is what lets
-- this component drive any attached part without a slot pointing at the body.
function HairSwing:drive(world)
    local localNow = self.localWritten or qid()
    return qmul(world, qconj(localNow))
end

function HairSwing:onUpdate()
    local object = self:object()
    if not object then return end

    local now = Engine.systemTime
    local dt = math.min(num(Engine.deltatime, 1.0 / 60.0), 0.05)
    local world = qof(object:getWorldRotation())
    local parent = self:drive(world)

    -- The settle window watches the part's own transform, because that is what has to stop changing
    -- before the rest pose can be frozen: AvatarPart writes it once when it hangs the part, and a
    -- body that is spinning keeps the part's *world* position moving every single frame, so a world
    -- watch could never settle and the spring would silently never engage.
    if not self.settled then
        local localNow = localSnapshot(object)
        if not sameSnapshot(self.stillLocal, localNow) then
            self.stillSince = now
        end
        self.stillLocal = localNow
        -- Read the rotation off the object, never out of the snapshot: that table's x/y/z row is the
        -- position, and a quaternion built from it is not a rotation at all - the spring would drive
        -- the part toward a target whose norm is metres, and the transform written back to the engine
        -- ends up a near-zero quaternion.
        self.localWritten = qof(object.rotation)
        self.state = world
        self.omega = { x = 0.0, y = 0.0, z = 0.0 }
        if now - (self.stillSince or now) >= math.max(0.0, num(self.startAfter, 0.25)) then
            self.bindLocal = self.localWritten
            self.settled = true
        end
        return
    end

    if self.enabled == false then
        self.localWritten = qof(object.rotation)
        self.state = world
        self.angle = 0.0
        return
    end

    local stiffness = num(self.stiffness, 42.0)
    local damping = num(self.damping, 5.0)
    local maxAngle = math.max(0.0, num(self.maxAngle, 45.0)) * RAD
    local maxSpeed = math.max(0.0, num(self.maxSpeed, 18.0))
    local hang = vnormalized(asVec(self.hangAxis, DOWN), DOWN)
    local wind = asVec(self.wind, nil)
    local droop = num(self.droop, 6.0)
    local windGain = num(self.windStrength, 0.0)

    local target = qmul(parent, self.bindLocal)
    local remaining = dt
    while remaining > 1e-6 do
        -- An explicit spring at these stiffnesses needs the smaller step to stay stable.
        local h = math.min(remaining, 1.0 / 120.0)
        remaining = remaining - h

        -- 2 * log() is axis * angle: the error the spring pulls against.
        local errorVec = qlog(qmul(target, qconj(self.state)))
        local e = { x = 2.0 * errorVec.x, y = 2.0 * errorVec.y, z = 2.0 * errorVec.z }
        local pull = vnormalized(e, nil)

        local acceleration = {
            x = stiffness * e.x - damping * self.omega.x,
            y = stiffness * e.y - damping * self.omega.y,
            z = stiffness * e.z - damping * self.omega.z
        }

        -- Droop and wind both turn the part's hang axis toward a world direction; the cross product
        -- is the shortest arc between them and its length is sin(angle), which is the right shape.
        local aim = nil
        if droop ~= 0.0 then
            aim = { x = DOWN.x * droop, y = DOWN.y * droop, z = DOWN.z * droop }
        end
        if wind and windGain ~= 0.0 then
            local gain = windGain
            local flutter = num(self.flutter, 0.0)
            if flutter ~= 0.0 then
                gain = gain * (1.0 + flutter * math.sin(now * num(self.flutterSpeed, 1.7) * 2.0 * math.pi))
            end
            aim = aim or { x = 0.0, y = 0.0, z = 0.0 }
            aim = { x = aim.x + wind.x * gain, y = aim.y + wind.y * gain, z = aim.z + wind.z * gain }
        end
        if aim and vlen(aim) > 1e-6 then
            local dir = qrotate(self.state, hang)
            local torque = vcross(dir, vnormalized(aim, hang))
            local strength = vlen(aim)
            acceleration.x = acceleration.x + torque.x * strength
            acceleration.y = acceleration.y + torque.y * strength
            acceleration.z = acceleration.z + torque.z * strength
        end

        self.omega = {
            x = self.omega.x + acceleration.x * h,
            y = self.omega.y + acceleration.y * h,
            z = self.omega.z + acceleration.z * h
        }
        local speed = vlen(self.omega)
        if maxSpeed > 0.0 and speed > maxSpeed then
            local scale = maxSpeed / speed
            self.omega = { x = self.omega.x * scale, y = self.omega.y * scale, z = self.omega.z * scale }
        end

        -- exp() wants axis * (angle / 2), hence the half step.
        self.state = qnormalize(qmul(
            qexp({ x = 0.5 * self.omega.x * h, y = 0.5 * self.omega.y * h, z = 0.5 * self.omega.z * h }),
            self.state))

        -- "Max deviation" is a limit on the angle, not on the pull: a bone that turns faster than
        -- the spring can chase it would otherwise wind the part around and around forever. The state
        -- is put back on the limit cone, along the same axis it was leaving on, and the velocity that
        -- was pushing it out is cancelled so the clamp does not fight the spring every step.
        if maxAngle > 0.0 then
            local leaving = qlog(qmul(target, qconj(self.state)))
            local deviation = 2.0 * vlen(leaving)
            if deviation > maxAngle and deviation > 1e-6 then
                local half = 0.5 * deviation
                local axis = { x = leaving.x / half, y = leaving.y / half, z = leaving.z / half }
                local back = maxAngle - 1e-4
                self.state = qnormalize(qmul(
                    qexp({ x = -0.5 * back * axis.x, y = -0.5 * back * axis.y, z = -0.5 * back * axis.z }),
                    target))
                if pull then
                    local outward = self.omega.x * pull.x + self.omega.y * pull.y + self.omega.z * pull.z
                    if outward < 0.0 then
                        self.omega = {
                            x = self.omega.x - pull.x * outward,
                            y = self.omega.y - pull.y * outward,
                            z = self.omega.z - pull.z * outward
                        }
                    end
                end
            end
        end
    end

    local offset = qlog(qmul(target, qconj(self.state)))
    self.angle = 2.0 * vlen(offset) / RAD

    local written = qnormalize(qmul(qconj(parent), self.state))
    object.rotation = Quaternion(written.w, written.x, written.y, written.z)
    self.localWritten = written
    pcall(function() object:updateTransform() end)
end

-- How far the part is holding away from its rest pose, in degrees: what a probe reads to prove the
-- spring really lags and then settles.
function HairSwing:deviation()
    return num(self.angle, 0.0)
end

-- A gameplay script (a jump, a hit, a landing) can flick the hair without knowing its state.
function HairSwing:impulse(axis, strength)
    local direction = vnormalized(asVec(axis, DOWN), DOWN)
    local gain = num(strength, 4.0)
    self.omega = {
        x = self.omega.x + direction.x * gain,
        y = self.omega.y + direction.y * gain,
        z = self.omega.z + direction.z * gain
    }
    return vlen(self.omega)
end

-- Re-freeze the rest pose, for when the part has been moved or re-attached by hand.
function HairSwing:release()
    self.settled = false
    self.stillSince = Engine.systemTime
    self.stillLocal = nil
    self.omega = { x = 0.0, y = 0.0, z = 0.0 }
    return true
end

return HairSwing
