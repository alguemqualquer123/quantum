-- (c) SR VINIX
-- SPDX-License-Identifier: MIT

-- Headless proof that every library component loads inside the engine's own Lua state and exposes
-- a properties table the editor can parse. Run with:
--   quantum-engine.exe --project <KitCheck> --scene kit_check
local components = {
    "FollowCamera", "OrbitCamera", "CameraShake", "CharacterController3D", "PhysicsMover3D",
    "TopDownController", "PlatformerController2D", "Rotator", "Bobber", "LookAt", "Health",
    "Projectile", "Spawner", "TriggerZone", "Timer", "Lifetime", "Pickup", "HudValue", "HudBar",
    "WaterWaves", "BeachShore", "FloatOnWater", "SwimController", "WaterRipples",
    "Rope", "Slingshot", "FallingObject", "GravityZone", "PhysicsGrabber",
    "DayNightCycle", "WeatherSystem", "WindZone", "MovePlatform",
    "GamepadMover", "Interactable", "ScenePortal", "Camera2DFollow", "PatrolPath",
    "LineOfSight", "DamageArea",
    "AmbientSound", "SoundOnEvent", "MusicPlayer",
    "ParticleBurst", "ScreenFade", "PostEffectPulse", "ImpactEffect", "TrailEffect",
    "Precipitation", "Lightning", "Fire", "Burnable"
}

-- Slots are allowed to have no default (an empty slot); a scalar row with no default is a bug,
-- because the inspector row would be blank and the script would read nil.
local scalar = {
    bool = true, boolean = true, int = true, integer = true, float = true, number = true,
    string = true, vector2 = true, vec2 = true, vector3 = true, vec3 = true,
    vector4 = true, vec4 = true, color3 = true, color4 = true
}

local failures = 0
local lines = {}

-- The installer drops the library into Components/, so try the project root first and then that
-- subfolder; both are places a real project keeps these files.
local function loadComponent(name)
    local ok, module = pcall(require, name)
    if ok then
        return ok, module
    end
    return pcall(require, "Components." .. name)
end

for _, name in ipairs(components) do
    local ok, module = loadComponent(name)
    if not ok or type(module) ~= "table" then
        failures = failures + 1
        lines[#lines + 1] = string.format("FAIL %-24s %s", name, tostring(module))
    else
        local properties = module.properties
        if type(properties) ~= "table" or #properties == 0 then
            failures = failures + 1
            lines[#lines + 1] = string.format("FAIL %-24s no properties table", name)
        else
            local bad = {}
            local rows = {}
            for _, prop in ipairs(properties) do
                local t = tostring(prop.type)
                if not prop.name or not prop.displayName then
                    bad[#bad + 1] = "missing name or displayName"
                elseif scalar[string.lower(t)] then
                    -- A scalar row must carry a literal default or the inspector shows a blank
                    -- field and the script reads nil.
                    if prop.default == nil then
                        bad[#bad + 1] = tostring(prop.name) .. " (" .. t .. " has no default)"
                    end
                else
                    rows[#rows + 1] = t
                end
            end
            -- Slot types are the ones that must be recognised by the resolver, so list them.
            local unique = {}
            for _, t in ipairs(rows) do unique[t] = true end
            local names = {}
            for t in pairs(unique) do names[#names + 1] = t end
            table.sort(names)
            if #bad > 0 then
                failures = failures + 1
                lines[#lines + 1] = string.format("FAIL %-24s %s", name, table.concat(bad, ", "))
            else
                lines[#lines + 1] = string.format("ok   %-24s %2d rows   slots: %s",
                    name, #properties, #names > 0 and table.concat(names, ", ") or "-")
            end
        end
    end
end

for _, line in ipairs(lines) do
    print("[kit] " .. line)
end
print(string.format("[kit] %d components checked in the engine Lua state, %d failures",
    #components, failures))
print(string.format("[kit] lua path in use: %s", System.getLuaPath()))

System.quit()
