// (c) SR VINIX
// SPDX-License-Identifier: MIT
//
// The FASE 1 acceptance test: does a plugin binary really map into a running process. Everything
// else in this suite is reachable by reading code; LoadLibrary, the ABI check and a vtable call
// across a module boundary are not, and they are exactly what a clean compile cannot prove.

#include "doctest.h"

#include "PluginManager.h"

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

using namespace quantum;
using namespace quantum::plugin;

namespace quantum {
namespace test {
namespace {

    // Each case gets its own settings directory, so a saved "enabled: false" cannot leak into the
    // next case and make a passing test fail for the wrong reason.
    struct Fixture {
        std::filesystem::path root;
        std::filesystem::path plugins;
        std::filesystem::path settings;

        explicit Fixture(const std::string& name) {
            std::error_code ec;
            root = std::filesystem::temp_directory_path(ec) / ("quantum-plugin-tests-" + name);
            std::filesystem::remove_all(root, ec);
            plugins = root / "plugins";
            settings = root / "settings";
            std::filesystem::create_directories(plugins, ec);
            std::filesystem::create_directories(settings, ec);
        }

        ~Fixture() {
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
        }

        void writePlugin(const std::string& folder, const std::string& body) const {
            const std::filesystem::path directory = plugins / folder;
            std::error_code ec;
            std::filesystem::create_directories(directory, ec);

            std::ofstream stream(directory / "plugin.json", std::ios::binary | std::ios::trunc);
            stream << body;
            stream.close();
        }

        void configure(PluginManager& manager) const {
            manager.setPluginsDirectory(plugins.generic_string());
            manager.setSettingsDirectory(settings.generic_string());
        }
    };

    std::string manifestJson(const std::string& id, const std::string& library,
                             const std::string& extra = std::string()) {
        std::string text = "{\n";
        text += "  \"id\": \"" + id + "\",\n";
        text += "  \"name\": \"" + id + " fixture\",\n";
        text += "  \"version\": \"1.0.0\",\n";
        text += "  \"abiVersion\": " + std::to_string(QUANTUM_PLUGIN_ABI_VERSION) + ",\n";
        text += "  \"libraries\": [\"" + library + "\"]";
        if (!extra.empty()) text += ",\n  " + extra;
        text += "\n}\n";
        return text;
    }

    #if defined(QUANTUM_TEST_PLUGINS_DIR)
        // A settings directory of its own per case, under the build tree rather than the temp one,
        // because these cases load the binaries CMake built next to this probe.
        std::string builtPlugins() { return QUANTUM_TEST_PLUGINS_DIR; }

        std::string builtSettings(const std::string& caseName) {
            return std::string(QUANTUM_TEST_PLUGINS_DIR) + "/test-settings/" + caseName;
        }

        void configureBuilt(PluginManager& manager, const std::string& caseName) {
            // Wiped once per run, per case: these cases load the real binaries, so they also read and
            // write a real settings file, and a "Saved greeting" left by the previous run would make
            // the manifest-default assertion fail for the wrong reason. The wipe has to happen only
            // on the first call for a case, because the reload and disable cases point a second
            // manager at the same directory to prove the file survived.
            static std::set<std::string> wiped;

            std::error_code ec;
            const std::filesystem::path directory(builtSettings(caseName));
            if (wiped.insert(caseName).second) std::filesystem::remove_all(directory, ec);
            std::filesystem::create_directories(directory, ec);

            manager.setPluginsDirectory(builtPlugins());
            manager.setSettingsDirectory(directory.generic_string());
        }
    #endif

}

    TEST_CASE("the plugins built beside this probe really map into the process") {
        #if defined(QUANTUM_TEST_PLUGINS_DIR)
            PluginManager manager;
            configureBuilt(manager, "load");

            CHECK(manager.discover() >= 2);
            CHECK(manager.loadAll() >= 1);

            PluginRecord* hello = manager.find("quantum.hello");
            REQUIRE(hello != nullptr);
            CHECK(hello->state == PluginState::Active);
            CHECK(hello->instance != nullptr);
            CHECK(hello->libraryHandle != nullptr);
            CHECK(hello->reportedAbiVersion == QUANTUM_PLUGIN_ABI_VERSION);

            // The negative half. The binary exports ABI 999 while its manifest claims the matching
            // number, so the refusal can only have come from the symbol - which is the check that
            // actually catches a stale plugin.
            PluginRecord* broken = manager.find("quantum.broken");
            REQUIRE(broken != nullptr);
            CHECK(broken->state == PluginState::Incompatible);
            CHECK(broken->error.find("abi") != std::string::npos);
            CHECK(broken->instance == nullptr);

            manager.shutdownAll();
        #else
            MESSAGE("QUANTUM_TEST_PLUGINS_DIR was not configured; the load test is skipped");
        #endif
    }

    TEST_CASE("a loaded plugin answers commands through the registry") {
        #if defined(QUANTUM_TEST_PLUGINS_DIR)
            PluginManager manager;
            configureBuilt(manager, "commands");
            manager.discover();
            REQUIRE(manager.load("quantum.hello"));

            PluginCommandInfo info;
            CHECK(manager.registry().findCommand("hello.ping", info));
            CHECK(info.pluginId == "quantum.hello");
            CHECK(manager.registry().ownerOfCommand("hello.echo") == "quantum.hello");

            UiValueMap arguments;
            arguments["text"] = UiValue::fromString("round trip");
            UiValue result;
            std::string error;

            REQUIRE(manager.executeCommand("hello.echo", arguments, result, error));
            CHECK(result.toString() == "round trip");

            arguments.clear();
            arguments["repeat"] = UiValue::fromInt(3);
            REQUIRE(manager.executeCommand("hello.ping", arguments, result, error));
            CHECK(result.toString() ==
                  "Hello from a plugin Hello from a plugin Hello from a plugin");

            // The counter lives in the instance, which is what makes the unload case below mean
            // something rather than merely compiling.
            result = UiValue();
            REQUIRE(manager.executeCommand("hello.calls", UiValueMap{}, result, error));
            CHECK(result.toInt() == 1);

            manager.shutdownAll();
        #else
            MESSAGE("QUANTUM_TEST_PLUGINS_DIR was not configured; the command test is skipped");
        #endif
    }

    TEST_CASE("the panel a plugin describes is what the editor would draw") {
        #if defined(QUANTUM_TEST_PLUGINS_DIR)
            PluginManager manager;
            configureBuilt(manager, "panel");
            manager.discover();
            REQUIRE(manager.load("quantum.hello"));

            const UiSchema* schema = manager.schemaOf("quantum.hello");
            REQUIRE(schema != nullptr);
            CHECK(schema->pluginId == "quantum.hello");
            CHECK(schema->title == "Hello");
            CHECK(schema->groups.size() >= 2);
            CHECK(schema->findField("greeting") != nullptr);
            CHECK(schema->findField("loud") != nullptr);
            CHECK(schema->findField("repeat") != nullptr);

            const UiField* repeat = schema->findField("repeat");
            REQUIRE(repeat != nullptr);
            CHECK(repeat->type == UiControlType::IntSlider);
            CHECK(repeat->minValue == 1.0);
            CHECK(repeat->maxValue == 8.0);

            bool listed = false;
            for (const PluginPanelInfo& panel : manager.registry().panels()) {
                if (panel.pluginId == "quantum.hello") listed = true;
            }
            CHECK(listed);

            manager.shutdownAll();
        #else
            MESSAGE("QUANTUM_TEST_PLUGINS_DIR was not configured; the schema test is skipped");
        #endif
    }

    TEST_CASE("unloading takes the instance and its commands with it, and loading again works") {
        #if defined(QUANTUM_TEST_PLUGINS_DIR)
            PluginManager manager;
            configureBuilt(manager, "unload");
            manager.discover();
            REQUIRE(manager.load("quantum.hello"));

            PluginRecord* hello = manager.find("quantum.hello");
            REQUIRE(hello != nullptr);

            UiValue result;
            std::string error;
            UiValueMap arguments;
            arguments["repeat"] = UiValue::fromInt(1);
            REQUIRE(manager.executeCommand("hello.ping", arguments, result, error));
            CHECK(result.toString() == "Hello from a plugin");

            REQUIRE(manager.unload("quantum.hello"));
            CHECK(hello->instance == nullptr);
            CHECK(hello->libraryHandle == nullptr);
            CHECK(hello->state == PluginState::Disabled);
            CHECK(manager.registry().ownerOfCommand("hello.ping").empty());

            result = UiValue();
            CHECK_FALSE(manager.executeCommand("hello.ping", UiValueMap{}, result, error));
            CHECK_FALSE(error.empty());

            // Reload is the path the editor takes every time a plugin is recompiled.
            CHECK(manager.load("quantum.hello"));
            CHECK(hello->state == PluginState::Active);
            CHECK(manager.registry().ownerOfCommand("hello.ping") == "quantum.hello");

            result = UiValue();
            REQUIRE(manager.executeCommand("hello.calls", UiValueMap{}, result, error));
            CHECK(result.toInt() == 0);

            manager.shutdownAll();
        #else
            MESSAGE("QUANTUM_TEST_PLUGINS_DIR was not configured; the unload test is skipped");
        #endif
    }

    TEST_CASE("a disabled plugin stays disabled across managers") {
        #if defined(QUANTUM_TEST_PLUGINS_DIR)
        {
            PluginManager manager;
            configureBuilt(manager, "disabled");
            manager.discover();
            REQUIRE(manager.load("quantum.hello"));
            REQUIRE(manager.setEnabled("quantum.hello", false));
            CHECK(manager.find("quantum.hello")->state == PluginState::Disabled);
        }

        PluginManager reopened;
        configureBuilt(reopened, "disabled");
        reopened.loadAll();

        PluginRecord* hello = reopened.find("quantum.hello");
        REQUIRE(hello != nullptr);
        CHECK(hello->state == PluginState::Disabled);
        CHECK(hello->instance == nullptr);

        // Put it back, or every later case in this binary runs against a disabled plugin.
        CHECK(reopened.setEnabled("quantum.hello", true));
        CHECK(hello->state == PluginState::Active);
        reopened.shutdownAll();
        #else
        MESSAGE("QUANTUM_TEST_PLUGINS_DIR was not configured; the disable test is skipped");
        #endif
    }

    TEST_CASE("discovery reads a folder that is not a plugin without breaking the run") {
        Fixture fixture("discovery");

        fixture.writePlugin("alpha", manifestJson("test.alpha", "test-alpha"));
        std::error_code ec;
        std::filesystem::create_directories(fixture.plugins / "not-a-plugin", ec);
        fixture.writePlugin("garbage", "{ this is not json at all");

        PluginManager manager;
        fixture.configure(manager);
        CHECK(manager.discover() == 1);

        CHECK(manager.find("test.alpha") != nullptr);
        PluginRecord* garbage = manager.find("garbage");
        REQUIRE(garbage != nullptr);
        CHECK(garbage->state == PluginState::Failed);
        CHECK(garbage->error.find("invalid manifest") != std::string::npos);

        manager.shutdownAll();
    }

    TEST_CASE("a manifest naming a library that is not there fails with the reason") {
        Fixture fixture("missing-library");
        fixture.writePlugin("ghost", manifestJson("test.ghost", "test-ghost"));

        PluginManager manager;
        fixture.configure(manager);
        manager.discover();
        CHECK_FALSE(manager.load("test.ghost"));

        PluginRecord* ghost = manager.find("test.ghost");
        REQUIRE(ghost != nullptr);
        CHECK(ghost->state == PluginState::Failed);
        CHECK(ghost->error.find("not found") != std::string::npos);
    }

    TEST_CASE("a plugin for another platform is refused before anything is mapped") {
        Fixture fixture("platform");
        fixture.writePlugin("other",
            manifestJson("test.other", "test-other", "\"platforms\": [\"amiga\"]"));

        PluginManager manager;
        fixture.configure(manager);
        manager.discover();
        CHECK_FALSE(manager.load("test.other"));

        PluginRecord* other = manager.find("test.other");
        REQUIRE(other != nullptr);
        CHECK(other->state == PluginState::Incompatible);
        CHECK(other->error.find("not built for") != std::string::npos);
    }

    TEST_CASE("a manifest declaring the wrong ABI is refused without mapping the binary") {
        Fixture fixture("manifest-abi");
        fixture.writePlugin("old",
            "{\n  \"id\": \"test.old\",\n  \"name\": \"Old\",\n  \"version\": \"1.0.0\",\n"
            "  \"abiVersion\": 999,\n  \"libraries\": [\"test-old\"]\n}\n");

        PluginManager manager;
        fixture.configure(manager);
        manager.discover();
        CHECK_FALSE(manager.load("test.old"));

        PluginRecord* old = manager.find("test.old");
        REQUIRE(old != nullptr);
        CHECK(old->state == PluginState::Incompatible);
        CHECK(old->error.find("abi") != std::string::npos);
    }

    TEST_CASE("a dependency that is not installed stops the plugin that needs it") {
        Fixture fixture("missing-dependency");
        fixture.writePlugin("needs",
            manifestJson("test.needs", "test-needs",
                         "\"dependencies\": [\"test.does-not-exist\"]"));

        PluginManager manager;
        fixture.configure(manager);
        manager.loadAll();

        PluginRecord* needs = manager.find("test.needs");
        REQUIRE(needs != nullptr);
        CHECK(needs->state == PluginState::Incompatible);
        CHECK(needs->error.find("not installed") != std::string::npos);
    }

    TEST_CASE("a dependency cycle is reported instead of hanging the load pass") {
        Fixture fixture("cycle");
        fixture.writePlugin("one",
            manifestJson("test.one", "test-one", "\"dependencies\": [\"test.two\"]"));
        fixture.writePlugin("two",
            manifestJson("test.two", "test-two", "\"dependencies\": [\"test.one\"]"));

        PluginManager manager;
        fixture.configure(manager);
        CHECK(manager.loadAll() == 0);

        PluginRecord* one = manager.find("test.one");
        PluginRecord* two = manager.find("test.two");
        REQUIRE(one != nullptr);
        REQUIRE(two != nullptr);
        CHECK(one->state == PluginState::Incompatible);
        CHECK(two->state == PluginState::Incompatible);
    }

    TEST_CASE("settings come from the manifest and survive a reload") {
        #if defined(QUANTUM_TEST_PLUGINS_DIR)
            PluginManager manager;
            configureBuilt(manager, "settings-round");
            manager.discover();
            REQUIRE(manager.load("quantum.hello"));

            PluginRecord* hello = manager.find("quantum.hello");
            REQUIRE(hello != nullptr);

            // The manifest default arrived, and it kept its type.
            CHECK(hello->settings.getString("greeting") == "Hello from a plugin");
            CHECK(hello->settings.getInt("repeat", 0) == 1);
            CHECK_FALSE(hello->settings.getBool("loud", true));

            hello->settings.set("greeting", UiValue::fromString("Saved greeting"));
            hello->settings.set("loud", UiValue::fromBool(true));
            REQUIRE(hello->settings.save());

            UiValue result;
            std::string error;
            REQUIRE(manager.executeCommand("hello.ping", UiValueMap{}, result, error));
            CHECK(result.toString() == "SAVED GREETING");

            manager.unload("quantum.hello");
            manager.shutdownAll();

            // A second manager over the same settings directory reads the file back.
            PluginManager reopened;
            configureBuilt(reopened, "settings-round");
            reopened.discover();
            REQUIRE(reopened.load("quantum.hello"));
            CHECK(reopened.find("quantum.hello")->settings.getString("greeting") ==
                  "Saved greeting");

            reopened.shutdownAll();
        #else
            MESSAGE("QUANTUM_TEST_PLUGINS_DIR was not configured; the settings test is skipped");
        #endif
    }

    TEST_CASE("installFromDirectory copies a plugin in and discovery picks it up") {
        Fixture fixture("install");

        const std::filesystem::path source = fixture.root / "dropin";
        std::error_code ec;
        std::filesystem::create_directories(source / "Resources", ec);
        {
            std::ofstream manifest(source / "plugin.json", std::ios::binary);
            manifest << manifestJson("test.dropin", "test-dropin");
        }
        {
            std::ofstream note(source / "Resources" / "note.txt", std::ios::binary);
            note << "shipped beside the manifest";
        }

        PluginManager manager;
        fixture.configure(manager);

        std::string error;
        const std::string installed = manager.installFromDirectory(source.generic_string(), false,
                                                                   error);
        CHECK(installed == "test.dropin");
        CHECK(error.empty());

        CHECK(std::filesystem::exists(fixture.plugins / "dropin" / "plugin.json"));
        CHECK(std::filesystem::exists(fixture.plugins / "dropin" / "Resources" / "note.txt"));

        // Refusing to overwrite is the default, and it has to say so.
        error.clear();
        CHECK(manager.installFromDirectory(source.generic_string(), false, error).empty());
        CHECK(error.find("already exists") != std::string::npos);

        // It loads as any plugin would - and fails for the honest reason: nothing was built for it.
        CHECK_FALSE(manager.load("test.dropin"));
        CHECK(manager.find("test.dropin")->error.find("not found") != std::string::npos);
    }

    TEST_CASE("describe() accounts for every record, including the ones that were refused") {
        Fixture fixture("describe");
        fixture.writePlugin("alpha", manifestJson("test.alpha", "test-alpha"));
        fixture.writePlugin("other",
            manifestJson("test.other", "test-other", "\"platforms\": [\"amiga\"]"));

        PluginManager manager;
        fixture.configure(manager);
        manager.loadAll();

        const std::string text = manager.describe();
        CHECK(text.find("test.alpha") != std::string::npos);
        CHECK(text.find("test.other") != std::string::npos);
        CHECK(text.find("incompatible") != std::string::npos);
        CHECK(text.find("failed") != std::string::npos);
        CHECK(text.find("not built for") != std::string::npos);

        CHECK(manager.failedCount() >= 1);
        CHECK(manager.activeCount() == 0);
    }
    TEST_CASE("the frame tick reaches the plugin the manifest granted") {
        #if defined(QUANTUM_TEST_PLUGINS_DIR)
            PluginManager manager;
            configureBuilt(manager, "tick");
            manager.discover();
            REQUIRE(manager.load("quantum.hello"));

            // The permission is what makes the call happen at all, so assert the fixture has it
            // before asserting the counter it drives.
            REQUIRE(manager.find("quantum.hello")->manifest.hasPermission("tick"));

            for (int frame = 0; frame < 4; frame++) manager.update(0.25f);

            UiValue result;
            std::string error;
            REQUIRE(manager.executeCommand("hello.ticks", UiValueMap{}, result, error));
            CHECK(result.toInt() == 4);

            // A refused plugin has a record and no instance; the tick loop must leave it alone
            // rather than crash on the null.
            PluginRecord* broken = manager.find("quantum.broken");
            REQUIRE(broken != nullptr);
            CHECK(broken->instance == nullptr);

            manager.shutdownAll();
        #else
            MESSAGE("QUANTUM_TEST_PLUGINS_DIR was not configured; the tick test is skipped");
        #endif
    }

    TEST_CASE("a capability the manifest does not name is refused, and refused only once") {
        PluginManifest granted;
        granted.id = "test.gate";
        granted.permissions = { "log" };

        PluginSettings settings;
        PluginRegistry registry;
        std::vector<PluginLogEntry> sink;

        PluginContext context(granted, settings, registry, sink);

        CHECK(context.require("log"));
        CHECK(sink.empty());

        CHECK_FALSE(context.require("tick"));
        REQUIRE(sink.size() == 1);
        CHECK(sink[0].level == PluginLogLevel::Warning);
        CHECK(sink[0].message.find("tick") != std::string::npos);

        // The gate sits in a per-frame path, so the repeat call must stay quiet.
        CHECK_FALSE(context.require("tick"));
        CHECK(sink.size() == 1);

        // scene() is gated too, and the denial is a nullptr instead of a crash. No scene is loaded
        // in this probe, so the granted answer is nullptr as well - what differs is the log.
        CHECK(context.scene() == nullptr);
        CHECK(sink.size() == 2);
        CHECK(sink[1].message.find("scene") != std::string::npos);
        CHECK(context.scene() == nullptr);
        CHECK(sink.size() == 2);

        context.log(PluginLogLevel::Info, "visible");
        REQUIRE(sink.size() == 3);
        CHECK(sink[2].message == "visible");
    }

    TEST_CASE("a plugin without the log permission is silenced, with one line saying why") {
        PluginManifest silent;
        silent.id = "test.silent";

        PluginSettings settings;
        PluginRegistry registry;
        std::vector<PluginLogEntry> sink;

        PluginContext context(silent, settings, registry, sink);
        context.log(PluginLogLevel::Info, "text that must never appear");

        // Exactly one entry, and it is the refusal rather than the message: a plugin cannot silence
        // itself without leaving a trace the user can read.
        REQUIRE(sink.size() == 1);
        CHECK(sink[0].message.find("must never appear") == std::string::npos);
        CHECK(sink[0].message.find("\"log\"") != std::string::npos);
        CHECK(sink[0].level == PluginLogLevel::Warning);

        context.log(PluginLogLevel::Error, "also dropped");
        CHECK(sink.size() == 1);
    }

    TEST_CASE("a permission name the plugin system does not know is reported at discovery") {
        Fixture fixture("permission-name");
        fixture.writePlugin("typo", manifestJson("test.typo", "test-typo",
                                                 "\"permissions\": [\"scense\", \"log\"]"));

        PluginManager manager;
        fixture.configure(manager);
        manager.discover();

        bool reported = false;
        for (const PluginLogEntry& entry : manager.log()) {
            if (entry.pluginId != "test.typo") continue;
            if (entry.message.find("unknown permission \"scense\"") != std::string::npos) {
                reported = true;
            }
            // The valid name beside it is not complained about.
            CHECK(entry.message.find("unknown permission \"log\"") == std::string::npos);
        }
        CHECK(reported);
    }

    TEST_CASE("a plugin window and a menu item are registry entries only their owner can hold") {
        PluginRegistry registry;

        PluginWindowInfo stats;
        stats.pluginId = "test.a";
        stats.id = "a.stats";
        stats.title = "Stats";
        stats.category = "Tools";

        CHECK(registry.registerWindow(stats));
        REQUIRE(registry.windows().size() == 1);

        // The same plugin re-describing its own window replaces it instead of doubling the entry.
        stats.title = "Stats And Notes";
        stats.width = 800;
        CHECK(registry.registerWindow(stats));
        CHECK(registry.windows().size() == 1);
        PluginWindowInfo stored;
        REQUIRE(registry.findWindow("a.stats", stored));
        CHECK(stored.title == "Stats And Notes");
        CHECK(stored.width == 800);

        // Another plugin cannot take the id, whichever order they load in.
        PluginWindowInfo thief = stats;
        thief.pluginId = "test.b";
        CHECK_FALSE(registry.registerWindow(thief));
        CHECK(registry.lastError().find("test.a") != std::string::npos);

        PluginMenuItemInfo item;
        item.pluginId = "test.a";
        item.category = "Tools";
        item.label = "Open Stats";
        item.command = "a.open";
        CHECK(registry.registerMenuItem(item));

        // The key is what the user reads, so the same words in a different menu are a different
        // item - even from another plugin.
        PluginMenuItemInfo otherMenu = item;
        otherMenu.pluginId = "test.b";
        otherMenu.category = "Window";
        CHECK(registry.registerMenuItem(otherMenu));
        CHECK(registry.menuItems().size() == 2);

        // And the same words in the same menu, from a second plugin, are the collision.
        PluginMenuItemInfo clash = item;
        clash.pluginId = "test.b";
        CHECK_FALSE(registry.registerMenuItem(clash));

        // An item with nothing to run would be a greyed line in a menu forever.
        PluginMenuItemInfo noCommand = item;
        noCommand.pluginId = "test.c";
        noCommand.label = "Broken";
        noCommand.command.clear();
        CHECK_FALSE(registry.registerMenuItem(noCommand));

        CHECK(registry.ownerOfWindow("a.stats") == "test.a");

        registry.clearPlugin("test.a");
        CHECK(registry.windows().empty());
        // test.b kept its copy in the "Window" menu; test.a's two entries are gone.
        CHECK(registry.menuItems().size() == 1);
        CHECK(registry.menuItems()[0].category == "Window");
    }

    TEST_CASE("the \"ui\" capability is what lets a plugin add a window or a menu item") {
        Fixture fixture("ui-gate");
        PluginManifest silent;
        silent.id = "test.silent-ui";
        silent.permissions = { "log" };

        PluginSettings settings;
        PluginRegistry registry;
        std::vector<PluginLogEntry> sink;

        PluginContext context(silent, settings, registry, sink);

        CHECK_FALSE(context.registerWindow("x.stats", "Stats", "Tools"));
        CHECK_FALSE(context.registerMenuItem("Stats", "x.open", "Tools"));
        CHECK_FALSE(context.openWindow("x.stats"));

        CHECK(registry.windows().empty());
        CHECK(registry.menuItems().empty());

        // Three calls, and the plugin is told once - the same rule the tick gate follows, because a
        // window that draws per frame would otherwise write three lines per frame.
        size_t denials = 0;
        for (const PluginLogEntry& entry : sink) {
            if (entry.message.find("\"ui\"") != std::string::npos) denials++;
        }
        CHECK(denials == 1);

        PluginManifest granted;
        granted.id = "test.granted-ui";
        granted.permissions = { "ui" };

        PluginRegistry openRegistry;
        std::vector<PluginLogEntry> openSink;
        PluginContext grantedContext(granted, settings, openRegistry, openSink);

        CHECK(openSink.empty());
        CHECK(grantedContext.registerWindow("x.stats", "Stats", "Tools", "", 640, 480));
        REQUIRE(openRegistry.windows().size() == 1);
        CHECK(openRegistry.windows()[0].pluginId == "test.granted-ui");
        CHECK(openRegistry.windows()[0].title == "Stats");

        // A menu item has to name a command the same plugin registered, so the item can never be a
        // way to reach another plugin's code.
        CHECK_FALSE(grantedContext.registerMenuItem("Stats", "someone.elses.command", "Tools"));
        CHECK(grantedContext.registerCommand("x.open", "opens"));
        CHECK(grantedContext.registerMenuItem("Stats", "x.open", "Tools"));
        REQUIRE(openRegistry.menuItems().size() == 1);
        CHECK(openRegistry.menuItems()[0].command == "x.open");

        // Opening is a request, and it is drained once rather than re-drawn forever.
        CHECK(grantedContext.openWindow("x.stats"));
        CHECK(grantedContext.openWindow("x.stats"));
        CHECK(openRegistry.windowOpenRequests().size() == 1);
        CHECK(openRegistry.takeWindowOpenRequests().size() == 1);
        CHECK(openRegistry.windowOpenRequests().empty());

        // A window nobody registered cannot be asked for.
        CHECK_FALSE(grantedContext.openWindow("x.nope"));

        // And a plugin cannot raise another plugin's window: the queue is keyed on the owner, so the
        // request either resolves to a live plugin or is refused here rather than at draw time.
        PluginManifest second;
        second.id = "test.granted-ui-2";
        second.permissions = { "ui" };
        PluginContext other(second, settings, openRegistry, openSink);
        CHECK_FALSE(other.openWindow("x.stats"));
        CHECK(other.registerWindow("y.stats", "Mine"));
        CHECK(openRegistry.windows().size() == 2);
        // The same id from the second plugin is the collision the registry refuses.
        CHECK_FALSE(other.registerWindow("x.stats", "Still mine"));
    }

    TEST_CASE("unloading a plugin drops its windows, items and pending open requests") {
        PluginRegistry registry;

        PluginWindowInfo window;
        window.pluginId = "test.a";
        window.id = "a.stats";
        window.title = "Stats";
        CHECK(registry.registerWindow(window));
        CHECK(registry.requestWindowOpen("test.a", "a.stats"));
        REQUIRE(registry.windowOpenRequests().size() == 1);

        registry.clearPlugin("test.a");
        CHECK(registry.windows().empty());
        // The host would otherwise be handed a name it cannot resolve to a live plugin.
        CHECK(registry.windowOpenRequests().empty());
    }

    TEST_CASE("the file API writes inside the plugin data directory and nowhere else") {
        Fixture fixture("files");
        const std::string root = (fixture.root / "data" / "test.writer").generic_string();

        PluginManifest granted;
        granted.id = "test.writer";
        granted.permissions = { "files" };

        PluginSettings settings;
        PluginRegistry registry;
        std::vector<PluginLogEntry> sink;

        PluginContext context(granted, settings, registry, sink, root);
        CHECK(context.dataDirectory() == root);

        std::string read;
        CHECK_FALSE(context.readFile("missing.txt", read));
        CHECK_FALSE(context.fileExists("missing.txt"));

        CHECK(context.writeTextFile("notes/first.txt", "one\n"));
        CHECK(context.fileExists("notes/first.txt"));
        CHECK(context.appendTextFile("notes/first.txt", "two\n"));
        CHECK(context.readFile("notes/first.txt", read));
        CHECK(read == "one\ntwo\n");

        const std::vector<std::string> listed = context.listFiles();
        REQUIRE(listed.size() == 1);
        CHECK(listed[0] == "notes/first.txt");

        const std::vector<std::string> withDirectories = context.listFiles("", true);
        REQUIRE(withDirectories.size() == 2);
        CHECK(withDirectories[0] == "notes/");

        CHECK(context.removeFile("notes/first.txt"));
        CHECK_FALSE(context.fileExists("notes/first.txt"));
        CHECK(context.listFiles().empty());

        // The traversal refusals. Each one is checked against the disk as well: a path that is
        // refused but still created something is the bug this whole API exists to avoid.
        const char* const escapes[] = {
            "../outside.txt", "notes/../../outside.txt", "/tmp/outside.txt",
            "\\server\\share\\outside.txt", "C:/Windows/outside.txt", "..", "notes/../.."
        };
        for (const char* path : escapes) {
            std::string absolute;
            CHECK_MESSAGE(context.resolveDataPath(path, absolute) == false, path);
            CHECK_MESSAGE(context.writeTextFile(path, "nope") == false, path);
            CHECK_MESSAGE(context.readFile(path, read) == false, path);
        }

        std::error_code ec;
        CHECK_FALSE(std::filesystem::exists(fixture.root / "outside.txt", ec));
        CHECK_FALSE(std::filesystem::exists(std::filesystem::path("C:/Windows/outside.txt"), ec));
    }

    TEST_CASE("a plugin without \"files\" cannot reach the disk, and is told once") {
        Fixture fixture("files-denied");
        const std::string root = (fixture.root / "data" / "test.nofiles").generic_string();

        PluginManifest silent;
        silent.id = "test.nofiles";
        silent.permissions = { "log" };

        PluginSettings settings;
        PluginRegistry registry;
        std::vector<PluginLogEntry> sink;

        PluginContext context(silent, settings, registry, sink, root);

        CHECK_FALSE(context.writeTextFile("anything.txt", "text"));
        std::string read;
        CHECK_FALSE(context.readFile("anything.txt", read));
        CHECK_FALSE(context.fileExists("anything.txt"));
        CHECK(context.listFiles().empty());

        std::error_code ec;
        CHECK_FALSE(std::filesystem::exists(std::filesystem::path(root), ec));

        size_t denials = 0;
        for (const PluginLogEntry& entry : sink) {
            if (entry.message.find("\"files\"") != std::string::npos) denials++;
        }
        CHECK(denials == 1);
    }

    TEST_CASE("the permission list and the gates agree about what is enforced") {
        // These are the five names the engine checks on a call. A sixth gate added without a
        // permission name here would be silently unenforceable, so the two lists are pinned together.
        CHECK(isEnforcedPermission("log"));
        CHECK(isEnforcedPermission("scene"));
        CHECK(isEnforcedPermission("tick"));
        CHECK(isEnforcedPermission("files"));
        CHECK(isEnforcedPermission("ui"));
        CHECK_FALSE(isEnforcedPermission("settings"));
        CHECK(isKnownPermission("settings"));
    }

    #if defined(QUANTUM_TEST_PLUGINS_DIR)
        TEST_CASE("a real plugin's window is described and answered with no window open") {
            PluginManager manager;
            configureBuilt(manager, "window-frame");

            REQUIRE(manager.loadAll() >= 1);
            PluginRecord* hello = manager.find("quantum.hello");
            REQUIRE(hello != nullptr);
            REQUIRE(hello->state == PluginState::Active);

            // The registration the plugin did in initialize(), read back the way the editor's menu
            // does: by name, from the registry, with no ImGui anywhere near it.
            const std::vector<PluginWindowInfo>& windows = manager.registry().windows();
            REQUIRE_FALSE(windows.empty());
            PluginWindowInfo stats;
            REQUIRE(manager.registry().findWindow("hello.stats", stats));
            CHECK(stats.pluginId == "quantum.hello");
            CHECK(stats.title == "Hello Stats");
            CHECK_FALSE(manager.registry().menuItems().empty());

            // A frame of it. This is the D6 contract: the plugin describes, and the description is
            // printable by a process that never created a window.
            UiSchema frame;
            std::string error;
            REQUIRE(manager.buildWindowSchema("hello.stats", frame, error));
            CHECK(frame.pluginId == "quantum.hello");
            CHECK(frame.title == "Hello Stats");
            CHECK_FALSE(frame.groups.empty());

            UiField* lines = frame.findField("lines");
            REQUIRE(lines != nullptr);
            CHECK(lines->type == UiControlType::IntInput);
            REQUIRE(frame.findField("note") != nullptr);
            CHECK(frame.findField("note")->type == UiControlType::Button);

            // Unknown windows and a plugin that is not active are both reported rather than drawn.
            UiSchema nothing;
            CHECK_FALSE(manager.buildWindowSchema("hello.nope", nothing, error));
            CHECK(error.find("no plugin registered") != std::string::npos);

            // The input side: values plus a pressed button, into the binary, and the effect comes
            // back out through a command - the only proof a headless run can get that the call
            // landed on the other side of the DLL boundary.
            lines->value = UiValue::fromInt(3);
            lines->changed = true;
            const UiValueMap values = collectSchemaValues(frame);
            REQUIRE(values.size() >= 1);

            const std::vector<std::string> pressed{ "note" };
            REQUIRE(manager.sendWindowInput("hello.stats", values, pressed, error));

            UiValue result;
            REQUIRE(manager.executeCommand("hello.stats", UiValueMap(), result, error));
            const std::string text = result.toString();
            CHECK(text.find("3 note line(s)") != std::string::npos);
            CHECK(text.find("1 window input(s)") != std::string::npos);
            CHECK(text.find("data") != std::string::npos);

            // And the file is really on disk, inside the data directory the manager named.
            const std::string noteFile =
                manager.pluginDataDirectory("quantum.hello") + "/notes.txt";
            std::error_code ec;
            CHECK_MESSAGE(std::filesystem::exists(std::filesystem::path(noteFile), ec), noteFile);
            if (std::filesystem::exists(std::filesystem::path(noteFile), ec)) {
                std::filesystem::remove(std::filesystem::path(noteFile), ec);
            }

            manager.shutdownAll();
        }
    #endif

}
}
