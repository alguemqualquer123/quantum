// (c) SR VINIX
// SPDX-License-Identifier: MIT
//
// GPU-free unit tests of the plugin contract: the manifest parser, the version comparison a
// plugin is accepted or refused by, and the UI schema the editor renders in a plugin's place.
// This file owns the doctest main.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "PluginAbi.h"
#include "PluginManifest.h"
#include "PluginUiSchema.h"
#include "PluginVersion.h"

#include <string>

using namespace quantum;
using namespace quantum::plugin;

namespace quantum {
namespace test {

    // The "abiVersion" below is the literal QUANTUM_PLUGIN_ABI_VERSION, and the test that reads it
    // compares against the macro: when the contract changes, this fixture is supposed to fail so the
    // sample manifests get looked at too instead of being refused at load in someone else's project.
    static const char* VALID_MANIFEST = R"json({
      "id": "acme.thing",
      "name": "Thing",
      "description": "A thing that plugins.",
      "author": "Acme",
      "version": "2.1.0",
      "minEngineVersion": "0.1.0",
      "abiVersion": 2,
      "platforms": ["windows", "linux", "macos"],
      "type": "engine",
      "dependencies": ["quantum.hello", {"id": "acme.base", "minVersion": "1.0.0"}],
      "libraries": ["acme-thing"],
      "entryPoint": "QuantumPluginCreate",
      "permissions": ["log", "write"],
      "commands": ["thing.do"],
      "panel": { "title": "Thing" },
      "settings": { "count": 3, "ratio": 0.25, "label": "hello", "enabled": true }
    })json";

    TEST_CASE("a complete manifest parses into every field the loader needs") {
        PluginManifest manifest;
        CHECK(parsePluginManifest(VALID_MANIFEST, manifest));
        CHECK(manifest.valid);
        CHECK(manifest.errors.empty());

        CHECK(manifest.id == "acme.thing");
        CHECK(manifest.name == "Thing");
        CHECK(manifest.author == "Acme");
        CHECK(manifest.version == "2.1.0");
        CHECK(manifest.minEngineVersion == "0.1.0");
        CHECK(manifest.abiVersion == QUANTUM_PLUGIN_ABI_VERSION);
        CHECK(manifest.type == "engine");
        CHECK(manifest.panelTitle == "Thing");

        REQUIRE(manifest.dependencies.size() == 2);
        CHECK(manifest.dependencies[0].id == "quantum.hello");
        CHECK(manifest.dependencies[0].minVersion.empty());
        CHECK(manifest.dependencies[1].id == "acme.base");
        CHECK(manifest.dependencies[1].minVersion == "1.0.0");

        CHECK(manifest.hasPermission("log"));
        CHECK(manifest.hasPermission("write"));
        CHECK_FALSE(manifest.hasPermission("delete"));

        // Defaults carry their type, because a plugin reading "3" as a string would silently
        // lose the difference between an int slider and a text row.
        REQUIRE(manifest.defaults.size() == 4);
        CHECK(manifest.defaults.at("count").toInt() == 3);
        CHECK(manifest.defaults.at("count").kind == UiValue::Kind::Int);
        CHECK(manifest.defaults.at("ratio").toFloat() == doctest::Approx(0.25));
        CHECK(manifest.defaults.at("label").toString() == "hello");
        CHECK(manifest.defaults.at("enabled").toBool());
    }

    TEST_CASE("a malformed manifest reports every problem instead of the first one") {
        PluginManifest manifest;
        CHECK_FALSE(parsePluginManifest("{ this is not json", manifest));
        CHECK_FALSE(manifest.valid);
        CHECK_FALSE(manifest.errors.empty());
        CHECK(manifest.errorsAsText().find("not valid JSON") != std::string::npos);
    }

    TEST_CASE("required fields are named in the error text") {
        PluginManifest manifest;
        CHECK_FALSE(parsePluginManifest(R"json({"name": "No Id"})json", manifest));

        const std::string text = manifest.errorsAsText();
        CHECK(text.find("\"id\"") != std::string::npos);
        CHECK(text.find("\"version\"") != std::string::npos);
        CHECK(text.find("abiVersion") != std::string::npos);
        CHECK(text.find("libraries") != std::string::npos);
    }

    TEST_CASE("an empty library list is a manifest error, not a load crash later") {
        PluginManifest manifest;
        CHECK_FALSE(parsePluginManifest(
            R"json({"id":"a.b","name":"A","version":"1.0.0","abiVersion":1,"libraries":[]})json",
            manifest));
        CHECK(manifest.errorsAsText().find("empty") != std::string::npos);
    }

    TEST_CASE("libraryFileName adds the platform suffix once") {
        PluginManifest manifest;
        REQUIRE(parsePluginManifest(VALID_MANIFEST, manifest));

        const std::string withSuffix = manifest.libraryFileName();
        CHECK(withSuffix.size() > 4);
        CHECK(withSuffix.find('.') != std::string::npos);

        // A manifest that already spelled the extension must not get a second one.
        PluginManifest spelled;
        REQUIRE(parsePluginManifest(
            R"json({"id":"a.b","name":"A","version":"1.0.0","abiVersion":1,
                    "libraries":["acme-thing.dll"]})json",
            spelled));
        const std::string spelledName = spelled.libraryFileName();
        CHECK(spelledName.find(".dll.dll") == std::string::npos);
        CHECK(spelledName == "acme-thing.dll");
    }

    TEST_CASE("versions parse, compare, and survive the shapes git describe produces") {
        CHECK(parseVersion("1.2.3").parsed);
        CHECK(parseVersion("v1.2.3") == parseVersion("1.2.3"));
        CHECK(parseVersion("1.2").patch == 0);
        CHECK(parseVersion("2.1.0-10-gabc123").major == 2);
        CHECK(parseVersion("2.1.0-dirty").minor == 1);

        // The local build version is a commit hash. Reading it as 0.0.0 would refuse every plugin
        // with a minEngineVersion, so it has to come back unparsable instead.
        CHECK_FALSE(parseVersion("297f87f4-dirty").parsed);
        CHECK_FALSE(parseVersion("").parsed);
        CHECK_FALSE(parseVersion("beta").parsed);

        CHECK(compareVersions(parseVersion("1.2.3"), parseVersion("1.2.4")) < 0);
        CHECK(compareVersions(parseVersion("1.10.0"), parseVersion("1.9.9")) > 0);
        CHECK(compareVersions(parseVersion("1.2.3"), parseVersion("1.2.3")) == 0);
        CHECK(versionToString(parseVersion("1.2.3")) == "1.2.3");
    }

    TEST_CASE("minEngineVersion is enforced when both sides are numbers and skipped when they are not") {
        std::string reason;

        CHECK(isVersionCompatible(parseVersion("1.4.0"), parseVersion("1.2.0"), reason));
        CHECK(reason.empty());

        CHECK_FALSE(isVersionCompatible(parseVersion("1.0.0"), parseVersion("1.2.0"), reason));
        CHECK(reason.find("needs engine") != std::string::npos);

        const Version unparsable = parseVersion("297f87f4-dirty");
        CHECK(isVersionCompatible(unparsable, parseVersion("1.2.0"), reason));
        CHECK_FALSE(reason.empty());

        CHECK_FALSE(isVersionCompatible(parseVersion("1.0.0"), parseVersion("nonsense"), reason));
    }

    TEST_CASE("the schema builder produces what the editor can render") {
        UiSchema schema;
        schema.pluginId = "acme.thing";

        UiSchemaBuilder builder(schema);
        builder.title("Thing");
        builder.group("Generation");
        builder.intSlider("seed", "Seed", 7, 0, 9999, 1);
        builder.floatSlider("scale", "Scale", 1.5, 0.1, 10.0, 0.05);
        builder.boolean("water", "Add water", true);
        builder.enumeration("biome", "Biome", 2, {"Tundra", "Grassland", "Forest"});
        builder.color("tint", "Tint", 0.5f, 0.25f, 0.1f);
        builder.button("generate", "Generate");
        builder.label("Preview updates on release.");
        builder.group("Export");
        builder.text("path", "Output", "out/thing");

        REQUIRE(schema.groups.size() == 2);
        CHECK(schema.groups[0].title == "Generation");
        CHECK(schema.groups[0].fields.size() == 7);
        CHECK(schema.groups[1].fields.size() == 1);

        const UiField* seed = schema.findField("seed");
        REQUIRE(seed != nullptr);
        CHECK(seed->type == UiControlType::IntSlider);
        CHECK(seed->value.toInt() == 7);
        CHECK(seed->minValue == 0.0);
        CHECK(seed->maxValue == 9999.0);

        const UiField* biome = schema.findField("biome");
        REQUIRE(biome != nullptr);
        CHECK(biome->optionIndex == 2);
        CHECK(biome->value.toString() == "Forest");
        CHECK(biome->options.size() == 3);

        CHECK(schema.findField("does-not-exist") == nullptr);
    }

    TEST_CASE("collecting values hands the plugin only the fields that carry data") {
        UiSchema schema;
        UiSchemaBuilder builder(schema);
        builder.group("G");
        builder.boolean("on", "On", true);
        builder.button("go", "Go");
        builder.label("read only");
        builder.intInput("count", "Count", 12, 0, 100);

        const UiValueMap values = collectSchemaValues(schema);
        CHECK(values.size() == 2);
        CHECK(values.count("go") == 0);
        CHECK(values.at("on").toBool());
        CHECK(values.at("count").toInt() == 12);
    }

    TEST_CASE("a plugin that forgets group() still gets its fields") {
        UiSchema schema;
        UiSchemaBuilder builder(schema);
        builder.boolean("orphan", "Orphan", false);

        REQUIRE(schema.groups.size() == 1);
        CHECK(schema.groups[0].fields.size() == 1);
        CHECK(schema.findField("orphan") != nullptr);
    }

    TEST_CASE("UiValue coerces between the types a settings file mixes") {
        CHECK(UiValue::fromString("36").toInt() == 36);
        CHECK(UiValue::fromString("1.5").toFloat() == doctest::Approx(1.5));
        CHECK(UiValue::fromString("true").toBool());
        CHECK(UiValue::fromString("no").toBool(false) == false);
        CHECK(UiValue::fromInt(5).toString() == "5");
        CHECK(UiValue::fromFloat(0.5).toString() == "0.5");
        CHECK(UiValue::fromFloat(2.0).toString() == "2");
        CHECK(UiValue().toInt(-1) == -1);
        CHECK(UiValue::fromString("not a number").toFloat(3.0) == doctest::Approx(3.0));
    }
    TEST_CASE("the permission names the engine knows are a closed list") {
        // Enforced means a call is refused without it, so these three have to stay the exact set the
        // gates in PluginContext and PluginManager check.
        CHECK(isEnforcedPermission("log"));
        CHECK(isEnforcedPermission("scene"));
        CHECK(isEnforcedPermission("tick"));
        CHECK_FALSE(isEnforcedPermission("settings"));

        // Known but not enforced: declarations the editor shows and the plugin is expected to honour.
        CHECK(isKnownPermission("settings"));
        CHECK(isKnownPermission("files"));
        CHECK(isKnownPermission("ui"));

        // Unknown: reported at discovery, because a misspelling silently denies the capability.
        CHECK_FALSE(isKnownPermission("scense"));
        CHECK_FALSE(isKnownPermission("Tick"));
        CHECK_FALSE(isKnownPermission(""));
    }

}
}
