-- Headless proof that the plugin registry is reachable from Lua. This file is the project entry the
-- engine runs at boot (tools/engine/Main.cpp:224), so no scene and no ScriptComponent is involved --
-- and because that boot chunk runs before PluginManager::loadAll(), every assertion sits in the frame
-- callback at the bottom rather than at the top level.

local passed = 0
local failed = 0

local function check(name, ok)
    if ok then
        passed = passed + 1
        Log.print("  [PASS] " .. name)
    else
        failed = failed + 1
        Log.print("  [FAIL] " .. name)
    end
end

local function run()
    Log.print("========== PLUGINS LUA PROBE ==========")

    check("Plugins namespace exists", type(Plugins) == "table")
    check("Plugins.call is a function", type(Plugins.call) == "function")

    local ids = Plugins.list()
    local found = false
    for _, id in ipairs(ids) do
        if id == "quantum.hello" then found = true end
    end
    check("Plugins.list contains quantum.hello (" .. #ids .. " plugin(s))", found)

    check("Plugins.state of a loaded plugin is active", Plugins.state("quantum.hello") == "active")
    check("Plugins.state of a refused plugin is incompatible",
        Plugins.state("quantum.broken") == "incompatible")
    check("Plugins.state of an unknown id is missing", Plugins.state("quantum.nope") == "missing")

    local info = Plugins.describe("quantum.hello")
    check("Plugins.describe returns the version", type(info.version) == "string" and #info.version > 0)
    check("Plugins.describe returns permissions",
        type(info.permissions) == "table" and #info.permissions >= 3)
    local broken = Plugins.describe("quantum.broken")
    check("Plugins.describe reports the refusal reason",
        type(broken.error) == "string" and #broken.error > 0)
    check("Plugins.describe of an unknown id is an empty table",
        Plugins.describe("quantum.nope").state == nil)

    check("Plugins.isEnabled", Plugins.isEnabled("quantum.hello") == true)

    local commands = Plugins.commands()
    local hasPing = false
    for _, name in ipairs(commands) do
        if name == "hello.ping" then hasPing = true end
    end
    check("Plugins.commands contains hello.ping (" .. #commands .. " command(s))", hasPing)

    -- "repeat" is a reserved word, so the field the CLI writes as `repeat=3` needs brackets here.
    local ping = Plugins.call("hello.ping", { ["repeat"] = 3 })
    check("Plugins.call succeeded", ping.ok == true)
    check("Plugins.call returned a string", type(ping.value) == "string")

    local echo = Plugins.call("hello.echo", { text = "ola" })
    check("an argument reaches the plugin", echo.ok == true and echo.value == "ola")

    local counted = Plugins.call("hello.calls")
    check("an int survives the round trip", counted.ok == true and counted.value == 1)

    local missing = Plugins.call("no.such.command")
    check("an unknown command answers, it does not raise", missing.ok == false)
    check("an unknown command says why", type(missing.error) == "string" and #missing.error > 0)

    check("Plugins.call refuses a non-string command", Plugins.call(7).ok == false)

    -- The tick counter only moves once Engine::runFrame has asked the plugin, so this is the proof of
    -- the whole round trip: frame -> PluginManager::update -> onUpdate -> Lua -> Plugins.call.
    local ticks = Plugins.call("hello.ticks")
    Log.print("  [TICK] the plugin ticked " .. tostring(ticks.value) .. " frame(s)")
    check("the engine tick reached the plugin", ticks.ok == true and tonumber(ticks.value) > 0)

    Log.print("========== RESULT: " .. passed .. " passed, " .. failed .. " failed ==========")
end

local frames = 0
local ran = false
Engine.onUpdate = function(dt)
    if ran then return end
    frames = frames + 1
    if frames < 4 then return end
    ran = true
    run()
end
