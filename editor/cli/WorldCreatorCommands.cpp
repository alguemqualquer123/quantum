// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "WorldCreatorCommands.h"

#include "window/PluginBrowserWindow.h"
#include "window/PluginSchemaPanel.h"
#include "window/WorldCreatorWindow.h"

#include "PluginInterface.h"
#include "PluginManager.h"
#include "PluginManifest.h"
#include "PluginSettings.h"
#include "PluginUiSchema.h"

#include "imgui.h"

#include "util/PngWriter.h"
#include "util/TerrainGenerator.h"
#include "util/TerrainMaterialLibrary.h"
#include "util/TerrainSplat.h"

#include "stb_image.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using namespace quantum;
using namespace quantum::plugin;

namespace {

    int passedCount = 0;
    int failedCount = 0;

    void report(bool ok, const std::string& name, const std::string& detail = std::string()) {
        if (ok) passedCount++;
        else failedCount++;
        std::cout << (ok ? "PASS " : "FAIL ") << name;
        if (!detail.empty()) std::cout << "  [" << detail << "]";
        std::cout << "\n";
    }

    std::string shown(float value, int digits = 3) {
        char text[64];
        std::snprintf(text, sizeof(text), "%.*f", digits, static_cast<double>(value));
        return std::string(text);
    }

    std::vector<unsigned char> readFile(const std::string& path) {
        std::ifstream stream(path, std::ios::binary);
        return std::vector<unsigned char>((std::istreambuf_iterator<char>(stream)),
                                          std::istreambuf_iterator<char>());
    }

    // An ImGui context with no renderer: NewFrame/Render run the whole widget pipeline and the
    // vertices are simply never submitted anywhere. This is how the schema panel and the two FASE 3
    // windows are exercised without launching the editor.
    class HeadlessUi {
    public:
        HeadlessUi() {
            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(1600.0f, 1200.0f);
            io.DeltaTime = 1.0f / 60.0f;
            unsigned char* pixels = nullptr;
            int width = 0, height = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        }

        HeadlessUi(const HeadlessUi&) = delete;
        HeadlessUi& operator=(const HeadlessUi&) = delete;

        ~HeadlessUi() { ImGui::DestroyContext(); }
    };

    struct ItemRect {
        ImVec2 from;
        ImVec2 to;
        bool valid = false;
    };

    // One frame of the panel. Mouse events go in before NewFrame so the widgets see them the same
    // frame; the rect read before End() is the panel's last control, which is the one under test when
    // the schema was built with a single field.
    ItemRect panelFrame(UiSchema& schema, const char* windowName, const ImVec2* mouse, bool pressed,
                        editor::PluginSchemaPanel::Touched* touched, const char* keyPrefix = "probe") {
        ImGuiIO& io = ImGui::GetIO();
        if (mouse) {
            io.AddMousePosEvent(mouse->x, mouse->y);
            io.AddMouseButtonEvent(0, pressed);
        }

        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(1400.0f, 1000.0f), ImGuiCond_Always);
        ImGui::Begin(windowName, nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);

        editor::PluginSchemaPanel::Touched local;
        editor::PluginSchemaPanel::draw(keyPrefix, schema, local);

        ItemRect rect;
        rect.from = ImGui::GetItemRectMin();
        rect.to = ImGui::GetItemRectMax();
        rect.valid = rect.to.y > rect.from.y && rect.to.x > rect.from.x;

        ImGui::End();
        ImGui::Render();

        if (touched) *touched = local;
        return rect;
    }

    void mergeTouch(editor::PluginSchemaPanel::Touched& into,
                    const editor::PluginSchemaPanel::Touched& added) {
        for (const std::string& id : added.edited) {
            if (std::find(into.edited.begin(), into.edited.end(), id) == into.edited.end()) {
                into.edited.push_back(id);
            }
        }
        for (const std::string& id : added.pressed) {
            if (std::find(into.pressed.begin(), into.pressed.end(), id) == into.pressed.end()) {
                into.pressed.push_back(id);
            }
        }
    }

    // A checkbox only answers inside its square, which is a frame-height wide at the left of the item
    // - its label is not clickable. Sliders, buttons and everything else take the middle.
    float clickXFor(UiControlType type, const ItemRect& rect) {
        if (type == UiControlType::Bool) {
            const float frameHeight = rect.to.y - rect.from.y;
            return rect.from.x + frameHeight * 0.5f;
        }
        return -1.0f;
    }

    std::string describe(const ItemRect& rect) {
        if (!rect.valid) return "rect invalid";
        char text[96];
        std::snprintf(text, sizeof(text), "%.0f,%.0f-%.0f,%.0f", static_cast<double>(rect.from.x),
                      static_cast<double>(rect.from.y), static_cast<double>(rect.to.x),
                      static_cast<double>(rect.to.y));
        return std::string(text);
    }

    // Press then release on the control the schema ends with. A slider moves while the button is held
    // and a checkbox or button fires on the release frame, so both frames' reports are the answer -
    // which is exactly what the editor does across a real drag.
    editor::PluginSchemaPanel::Touched clickControl(UiSchema& schema, const char* windowName,
                                                    UiControlType type, ItemRect* layoutOut) {
        editor::PluginSchemaPanel::Touched touched;
        const ItemRect layout = panelFrame(schema, windowName, nullptr, false, nullptr);
        if (layoutOut) *layoutOut = layout;
        if (!layout.valid) return touched;

        const float x = clickXFor(type, layout);
        const ImVec2 point(x >= 0.0f ? x : (layout.from.x + layout.to.x) * 0.5f,
                           (layout.from.y + layout.to.y) * 0.5f);

        editor::PluginSchemaPanel::Touched held;
        panelFrame(schema, windowName, &point, true, &held);
        mergeTouch(touched, held);

        editor::PluginSchemaPanel::Touched released;
        panelFrame(schema, windowName, &point, false, &released);
        mergeTouch(touched, released);
        return touched;
    }

    template<typename F>
    UiSchema buildSchema(F&& fill) {
        UiSchema schema;
        schema.pluginId = "worldcreator.probe";
        UiSchemaBuilder builder(schema);
        builder.title("probe panel");
        fill(builder);
        return schema;
    }

    // The plugin side of applyEdits: a callback the panel is supposed to drive. Only the one override
    // exists, because every IPlugin member has a default.
    class ProbePlugin : public IPlugin {
    public:
        void onSettingChanged(const std::string& key, const UiValue& value) override {
            calls.push_back(key + "=" + value.toString());
        }

        std::vector<std::string> calls;
    };

    std::string joinedCalls(const ProbePlugin& plugin) {
        std::string text;
        for (const std::string& call : plugin.calls) {
            if (!text.empty()) text += ",";
            text += call;
        }
        return text;
    }

    void checkSchemaShape() {
        UiSchema schema = buildSchema([](UiSchemaBuilder& builder) {
            builder.group("Read only");
            builder.label("a label carries no value");
            builder.separator();
            builder.group("Controls");
            builder.boolean("probe.enabled", "Enabled", true);
            builder.intSlider("probe.count", "Count", 3, 0, 10, 1);
            builder.intInput("probe.ports", "Ports", 42, 0, 100);
            builder.floatSlider("probe.mix", "Mix", 0.2, 0.0, 1.0, 0.05);
            builder.floatInput("probe.gain", "Gain", 3.5, 0.0, 10.0);
            builder.enumeration("probe.mode", "Mode", 1, {"soft", "hard", "blit"});
            builder.color("probe.tint", "Tint", 0.5f, 0.25f, 1.0f, 1.0f);
            builder.text("probe.comment", "Comment", "hello");
            builder.button("probe.press", "Press me");
        });

        const char* ids[] = {"probe.enabled", "probe.count", "probe.ports", "probe.mix",
                             "probe.gain", "probe.mode", "probe.tint", "probe.comment",
                             "probe.press"};
        bool everyId = true;
        for (const char* id : ids) {
            if (!schema.findField(id)) {
                everyId = false;
                std::cout << "  missing field " << id << "\n";
            }
        }
        report(everyId && !schema.findField("probe.absent"), "findField over 9 ids",
               "11 controls built, 9 with ids");

        const UiValueMap values = collectSchemaValues(schema);
        report(values.size() == 8 && values.count("probe.press") == 0 &&
                   values.count("probe.gain") == 1,
               "collectSchemaValues skips label, separator and button",
               std::to_string(values.size()) + " entries");
        report(values.at("probe.enabled").kind == UiValue::Kind::Bool &&
                   values.at("probe.enabled").toBool(),
               "a bool keeps its kind");
        report(values.at("probe.count").toInt() == 3 && values.at("probe.ports").toInt() == 42,
               "ints arrive as ints");
        report(std::fabs(values.at("probe.mix").toFloat() - 0.2) < 0.0001 &&
                   std::fabs(values.at("probe.gain").toFloat() - 3.5) < 0.0001,
               "floats arrive as floats");
        report(values.at("probe.mode").toString() == "hard",
               "an enum travels as its entry text", values.at("probe.mode").toString());

        const UiField* tint = schema.findField("probe.tint");
        report(tint && tint->value.toString() == std::string("#8040ffff"),
               "a colour seeds the whole colour, not one channel",
               tint ? tint->value.toString() : std::string("field gone"));

        report(schema.groups.size() == 2 && schema.groups[0].title == "Read only" &&
                   schema.groups[1].title == "Controls",
               "groups keep their titles and order", std::to_string(schema.groups.size()));
    }

    void checkColourText() {
        const float opaque[4] = {0.5f, 0.25f, 1.0f, 1.0f};
        const std::string text = editor::PluginSchemaPanel::colorToText(opaque);
        report(text == "#8040ffff", "colour to text", text);

        float back[4] = {0, 0, 0, 0};
        const bool parsed = editor::PluginSchemaPanel::colorFromText(text, back);
        bool close = parsed;
        for (int i = 0; i < 4 && close; i++) {
            close = std::fabs(back[i] - opaque[i]) < 0.004f;
        }
        report(close, "text to colour round trip", parsed ? text : "did not parse");

        const bool sixDigit = editor::PluginSchemaPanel::colorFromText("#ff8000", back);
        report(sixDigit && back[0] == 1.0f && back[2] == 0.0f && back[3] == 1.0f,
               "six digits mean opaque", sixDigit ? shown(back[3]) : "rejected");

        float untouched[4] = {7, 7, 7, 7};
        report(!editor::PluginSchemaPanel::colorFromText("not a colour", untouched) &&
                   !editor::PluginSchemaPanel::colorFromText("#12345", untouched) &&
                   !editor::PluginSchemaPanel::colorFromText("", untouched) && untouched[0] == 7.0f,
               "bad colour text is rejected without writing");

        // The engine and the panel must not diverge: a plugin reads the spelling the builder wrote.
        UiSchema schema = buildSchema([](UiSchemaBuilder& builder) {
            builder.color("probe.seed", "Seed colour", 0.2f, 0.4f, 0.6f, 0.5f);
        });
        const UiField* seeded = schema.findField("probe.seed");
        const float seedColour[4] = {0.2f, 0.4f, 0.6f, 0.5f};
        report(seeded && seeded->value.toString() ==
                               editor::PluginSchemaPanel::colorToText(seedColour),
               "the builder and the panel spell a colour alike",
               seeded ? seeded->value.toString() : "field gone");
    }

    void checkInteractions() {
        HeadlessUi ui;

        struct Case {
            const char* name;
            UiControlType type;
            bool expectEdit;
            bool expectPress;
        };

        // What a single click does per control: the three that change a value on click are the two
        // sliders and the checkbox; inputs and popups only take focus, which is not an edit.
        const Case cases[] = {
            {"Label", UiControlType::Label, false, false},
            {"Separator", UiControlType::Separator, false, false},
            {"Bool", UiControlType::Bool, true, false},
            {"IntSlider", UiControlType::IntSlider, true, false},
            {"IntInput", UiControlType::IntInput, false, false},
            {"FloatSlider", UiControlType::FloatSlider, true, false},
            {"FloatInput", UiControlType::FloatInput, false, false},
            {"Enum", UiControlType::Enum, false, false},
            {"Color", UiControlType::Color, false, false},
            {"MultilineText", UiControlType::MultilineText, false, false},
            {"Button", UiControlType::Button, false, true},
        };

        for (const Case& item : cases) {
            UiSchema schema = buildSchema([&item](UiSchemaBuilder& builder) {
                const std::string id = std::string("probe.") + item.name;
                switch (item.type) {
                    case UiControlType::Label: builder.label("just text"); break;
                    case UiControlType::Separator: builder.separator(); break;
                    case UiControlType::Bool: builder.boolean(id, "Enabled", true); break;
                    // Sliders start at their floor: a click in the middle has to move them, and a
                    // value already there would prove nothing.
                    case UiControlType::IntSlider: builder.intSlider(id, "Count", 0, 0, 10, 1); break;
                    case UiControlType::IntInput: builder.intInput(id, "Ports", 42, 0, 100); break;
                    case UiControlType::FloatSlider:
                        builder.floatSlider(id, "Mix", 0.0, 0.0, 1.0, 0.05);
                        break;
                    case UiControlType::FloatInput: builder.floatInput(id, "Gain", 3.5, 0.0, 10.0); break;
                    case UiControlType::Enum:
                        builder.enumeration(id, "Mode", 1, {"soft", "hard", "blit"});
                        break;
                    case UiControlType::Color: builder.color(id, "Tint", 0.5f, 0.25f, 1.0f, 1.0f); break;
                    case UiControlType::MultilineText: builder.text(id, "Comment", "hello"); break;
                    case UiControlType::Button: builder.button(id, "Press me"); break;
                }
            });

            ItemRect layout;
            const editor::PluginSchemaPanel::Touched touched =
                clickControl(schema, "probelayout", item.type, &layout);

            const bool edited = !touched.edited.empty();
            const bool pressed = !touched.pressed.empty();
            const std::string id = std::string("probe.") + item.name;
            const bool editIdsOk = !edited || (touched.edited.size() == 1 && touched.edited[0] == id);
            const bool pressIdsOk = !pressed || (touched.pressed.size() == 1 && touched.pressed[0] == id);

            report(edited == item.expectEdit && pressed == item.expectPress && editIdsOk && pressIdsOk &&
                       layout.valid,
                   std::string("one click on ") + item.name,
                   std::string(edited ? "edited" : "no edit") + "," +
                       (pressed ? "pressed" : "no press") + "," + describe(layout));
        }

        // The value the panel reports must be the value the field now carries - that is what
        // applyEdits writes and what the plugin is handed.
        UiSchema flip = buildSchema([](UiSchemaBuilder& builder) {
            builder.boolean("probe.flip", "Enabled", true);
        });
        // Every click below goes through the one host window: ImGui hands hover to the front window,
        // so a control drawn into a window created after it would swallow the click and the check
        // would pass for the wrong reason.
        ItemRect flipLayout;
        const editor::PluginSchemaPanel::Touched flipped =
            clickControl(flip, "probelayout", UiControlType::Bool, &flipLayout);
        const UiField* field = flip.findField("probe.flip");
        report(field && !field->value.toBool() && field->changed && flipped.edited.size() == 1,
               "a click flips the value the plugin will be given",
               field ? std::string(field->value.toBool() ? "still true, " : "false, ") +
                           describe(flipLayout)
                     : "field gone");

        // A slider click must report the number it moved to, not just that something moved.
        UiSchema slide = buildSchema([](UiSchemaBuilder& builder) {
            builder.intSlider("probe.range", "Range", 0, 0, 10, 1);
        });
        ItemRect slideLayout;
        const editor::PluginSchemaPanel::Touched slid =
            clickControl(slide, "probelayout", UiControlType::IntSlider, &slideLayout);
        const UiField* range = slide.findField("probe.range");
        report(range && slid.edited.size() == 1 && slid.edited[0] == "probe.range" &&
                   range->value.toInt() > 0 && range->value.toInt() < 10,
               "a slider click carries its new value",
               range ? std::to_string(range->value.toInt()) + ", " + describe(slideLayout)
                     : "field gone");

        UiSchema locked = buildSchema([](UiSchemaBuilder& builder) {
            builder.boolean("probe.locked", "Locked", false);
            builder.lastField().enabled = false;
        });
        const editor::PluginSchemaPanel::Touched ignored =
            clickControl(locked, "probelayout", UiControlType::Bool, nullptr);
        report(!ignored.any() && !locked.findField("probe.locked")->value.toBool(),
               "a disabled control does not edit");

        // Proof the aim really reaches this row: the same schema with the field enabled does flip, so
        // the disabled one staying put is the panel's doing and not a click that landed nowhere.
        UiSchema unlocked = buildSchema([](UiSchemaBuilder& builder) {
            builder.boolean("probe.locked", "Locked", false);
        });
        const editor::PluginSchemaPanel::Touched reached =
            clickControl(unlocked, "probelayout", UiControlType::Bool, nullptr);
        report(reached.edited.size() == 1 && unlocked.findField("probe.locked")->value.toBool(),
               "the same aim edits an enabled control");

        // The panel keeps a text buffer per plugin, keyed by the prefix it is handed: two plugins that
        // reuse a field id must not collide, and redrawing what is already on screen must not invent an
        // edit - the editor calls draw() on every frame the panel is open.
        UiSchema first = buildSchema([](UiSchemaBuilder& builder) {
            builder.text("probe.name", "Name", "one");
        });
        UiSchema second = buildSchema([](UiSchemaBuilder& builder) {
            builder.text("probe.name", "Name", "two");
        });
        editor::PluginSchemaPanel::Touched a1, a2, b1;
        panelFrame(first, "probekeyed", nullptr, false, &a1, "worldcreator.pluginA");
        panelFrame(first, "probekeyed", nullptr, false, &a2, "worldcreator.pluginA");
        panelFrame(second, "probekeyed", nullptr, false, &b1, "worldcreator.pluginB");
        report(!a1.any() && !a2.any() && !b1.any() &&
                   first.findField("probe.name")->value.toString() == "one" &&
                   second.findField("probe.name")->value.toString() == "two",
               "a redraw invents no edit, and two prefixes keep their own text");
    }

    void checkApplyEdits(const fs::path& tempDir) {
        UiSchema schema = buildSchema([](UiSchemaBuilder& builder) {
            builder.boolean("probe.enabled", "Enabled", false);
            builder.intSlider("probe.count", "Count", 0, 0, 10, 1);
            builder.enumeration("probe.mode", "Mode", 0, {"soft", "hard", "blit"});
            builder.text("probe.comment", "Comment", "before");
            builder.button("probe.go", "Go");
        });

        PluginRecord record;
        record.manifest.id = "worldcreator.probe";
        record.settings.setPluginId(record.manifest.id);
        record.settings.setPath((tempDir / "probe_settings.yaml").generic_string());

        ProbePlugin plugin;
        record.instance = &plugin;

        UiField* enabled = schema.findField("probe.enabled");
        UiField* count = schema.findField("probe.count");
        UiField* mode = schema.findField("probe.mode");
        UiField* comment = schema.findField("probe.comment");
        if (!enabled || !count || !mode || !comment) {
            report(false, "applyEdits finds its fields", "a field is missing");
            return;
        }

        // Exactly what the panel leaves behind after the user has edited these four.
        enabled->value = UiValue::fromBool(true);
        enabled->changed = true;
        count->value = UiValue::fromInt(7);
        count->changed = true;
        mode->value = UiValue::fromString("blit");
        mode->changed = true;
        comment->value = UiValue::fromString("after");
        comment->changed = true;

        const std::vector<std::string> edited = {"probe.enabled", "probe.count", "probe.mode",
                                                 "probe.comment", "probe.unknown", ""};
        std::string error;
        const bool applied = editor::PluginSchemaPanel::applyEdits(record, schema, edited, error);

        report(applied && error.empty(), "applyEdits saves", error);
        report(plugin.calls.size() == 4, "onSettingChanged runs once per known field",
               joinedCalls(plugin));
        report(record.settings.getBool("probe.enabled") && record.settings.getInt("probe.count") == 7 &&
                   record.settings.getString("probe.mode") == "blit" &&
                   record.settings.getString("probe.comment") == "after",
               "the settings hold the edited values");
        report(!record.settings.isDirty(), "saving clears the dirty flag");
        report(record.settings.lastError().empty(), "saving reports no error",
               record.settings.lastError());

        PluginSettings reread;
        reread.setPluginId(record.manifest.id);
        reread.setPath(record.settings.path());
        report(reread.load() && reread.getBool("probe.enabled") &&
                   reread.getInt("probe.count") == 7 && reread.getString("probe.comment") == "after",
               "the file on disk reloads with them", reread.lastError());
        report(fs::exists(record.settings.path()), "the settings file was created",
               record.settings.path());

        // A record whose plugin never started still gets its file; only a live instance is notified.
        PluginRecord unloaded;
        unloaded.manifest.id = "worldcreator.probe2";
        unloaded.settings.setPluginId(unloaded.manifest.id);
        unloaded.settings.setPath((tempDir / "probe_settings2.yaml").generic_string());
        unloaded.instance = nullptr;
        const std::vector<std::string> one = {"probe.count"};
        std::string unusedError;
        report(editor::PluginSchemaPanel::applyEdits(unloaded, schema, one, unusedError) &&
                   unloaded.settings.getInt("probe.count") == 7,
               "a disabled plugin keeps the value without a callback", unusedError);
    }

    void checkHeightmapWrite(const fs::path& tempDir) {
        const fs::path maps = tempDir / "maps";
        TerrainGenParams island = TerrainGenerator::preset(TerrainPreset::ISLAND);
        island.size = 64;
        island.seed = 7;
        island.erosionSeed = 7;

        std::string written;
        std::string error;
        std::vector<float> field;
        editor::WorldCreatorWindow::HeightmapStats stats;
        const bool ok = editor::WorldCreatorWindow::writeHeightmap(maps, "probe_island.png", island,
                                                                  written, stats, &field, error);
        report(ok && fs::exists(written), "writeHeightmap writes the Generate button's file",
               ok ? written : error);
        if (!ok) return;

        report(field.size() == 64u * 64u, "the field comes back for the preview",
               std::to_string(field.size()));
        report(stats.lowest <= stats.average && stats.average <= stats.highest &&
                   stats.lowest >= 0.0f && stats.highest <= 1.0f,
               "statistics are ordered and normalized",
               shown(stats.lowest) + "/" + shown(stats.average) + "/" + shown(stats.highest));
        report(stats.submerged > 0.0f && stats.submerged < 1.0f,
               "an island has both ocean and land", shown(stats.submerged, 2));
        report(stats.lowest <= island.seaLevel, "its lowest texel is at or below sea level",
               shown(stats.lowest) + " vs " + shown(island.seaLevel));

        int width = 0, height = 0, channels = 0;
        unsigned short* raw = static_cast<unsigned short*>(
            stbi_load_16(written.c_str(), &width, &height, &channels, 0));
        report(raw != nullptr && width == 64 && height == 64 && channels == 1,
               "the PNG decodes as 64x64 gray16",
               raw ? (std::to_string(width) + "x" + std::to_string(height) + "x" +
                      std::to_string(channels))
                   : std::string(stbi_failure_reason() ? stbi_failure_reason() : "unreadable"));
        if (raw) {
            unsigned int lowestValue = 65535u, highestValue = 0u;
            for (int i = 0; i < width * height; i++) {
                lowestValue = std::min(lowestValue, static_cast<unsigned int>(raw[i]));
                highestValue = std::max(highestValue, static_cast<unsigned int>(raw[i]));
            }
            const long expectedHigh = std::lround(stats.highest * 65535.0f);
            const long expectedLow = std::lround(stats.lowest * 65535.0f);
            report(std::fabs(static_cast<long>(highestValue) - expectedHigh) <= 1 &&
                       std::fabs(static_cast<long>(lowestValue) - expectedLow) <= 1,
                   "the 16-bit texels are the generated heights",
                   std::to_string(lowestValue) + ".." + std::to_string(highestValue) + " vs " +
                       std::to_string(expectedLow) + ".." + std::to_string(expectedHigh));
            stbi_image_free(raw);
        }

        // A world must be reproducible from its parameters, which is the whole reason a heightmap is
        // a file rather than a random run: same seed, same bytes.
        std::string againPath;
        std::string againError;
        editor::WorldCreatorWindow::HeightmapStats againStats;
        const bool again = editor::WorldCreatorWindow::writeHeightmap(maps, "probe_again.png", island,
                                                                     againPath, againStats, nullptr,
                                                                     againError);
        const std::vector<unsigned char> firstBytes = readFile(written);
        const std::vector<unsigned char> againBytes = again ? readFile(againPath) : std::vector<unsigned char>();
        report(again && againBytes == firstBytes && !firstBytes.empty() &&
                   againStats.lowest == stats.lowest && againStats.highest == stats.highest,
               "the same seed writes the same map",
               std::to_string(firstBytes.size()) + " vs " + std::to_string(againBytes.size()) + " bytes");

        TerrainGenParams other = island;
        other.seed = island.seed + 1;
        std::string otherPath;
        std::string otherError;
        editor::WorldCreatorWindow::HeightmapStats otherStats;
        const bool different = editor::WorldCreatorWindow::writeHeightmap(maps, "probe_other.png", other,
                                                                         otherPath, otherStats, nullptr,
                                                                         otherError);
        const std::vector<unsigned char> otherBytes = different ? readFile(otherPath) : std::vector<unsigned char>();
        report(different && otherBytes != firstBytes && !otherBytes.empty(),
               "a different seed writes a different map",
               shown(otherStats.average) + " vs " + shown(stats.average) + " mean height");

        // The window shows these numbers next to the preview; a field sampled twice must not drift.
        editor::WorldCreatorWindow::HeightmapStats measured =
            editor::WorldCreatorWindow::measureField(field, island);
        report(measured.lowest == stats.lowest && measured.highest == stats.highest &&
                   measured.average == stats.average && measured.submerged == stats.submerged,
               "measureField is deterministic on the same field");

        // A size out of range is clamped, not refused: MIN_SIZE keeps the caller's map valid instead
        // of failing a whole generation run.
        TerrainGenParams tiny = island;
        tiny.size = 3;
        std::string tinyPath;
        std::string tinyError;
        editor::WorldCreatorWindow::HeightmapStats tinyStats;
        const bool clamped = editor::WorldCreatorWindow::writeHeightmap(maps, "probe_tiny.png", tiny,
                                                                       tinyPath, tinyStats, nullptr,
                                                                       tinyError);
        int clampW = 0, clampH = 0, clampC = 0;
        unsigned short* clampPixels = clamped
                                          ? static_cast<unsigned short*>(
                                                stbi_load_16(tinyPath.c_str(), &clampW, &clampH, &clampC, 0))
                                          : nullptr;
        if (clampPixels) stbi_image_free(clampPixels);
        report(clamped && clampW == TerrainGenerator::MIN_SIZE && clampH == TerrainGenerator::MIN_SIZE,
               "a size below MIN_SIZE is clamped, not refused",
               std::to_string(clampW) + "x" + std::to_string(clampH));

        // Every preset must be able to write: the window offers all five, so the CLI has to be able
        // to prove all five too.
        bool everyPreset = true;
        std::string failedPreset;
        for (int index = 0; index <= static_cast<int>(TerrainPreset::CANYONS); index++) {
            TerrainGenParams params = TerrainGenerator::preset(static_cast<TerrainPreset>(index));
            params.size = 32;
            std::string path;
            std::string presetError;
            editor::WorldCreatorWindow::HeightmapStats presetStats;
            const bool wrote = editor::WorldCreatorWindow::writeHeightmap(
                maps, std::string("probe_preset_") + std::to_string(index) + ".png", params, path,
                presetStats, nullptr, presetError);
            if (!wrote || !(presetStats.lowest <= presetStats.highest)) {
                everyPreset = false;
                failedPreset = TerrainGenerator::presetName(static_cast<TerrainPreset>(index));
            }
        }
        report(everyPreset, "all five presets write a readable map", failedPreset);
    }

    void checkWindowsDraw() {
        HeadlessUi ui;

        editor::PluginBrowserWindow browser;
        browser.setOpen(true);
        for (int frame = 0; frame < 3; frame++) {
            ImGui::NewFrame();
            browser.show();
            ImGui::Render();
        }
        report(browser.isOpen(), "the Plugins window lays out three frames",
               std::to_string(PluginManager::instance().records().size()) + " records");

        // No project: generate and apply stay behind their own guards, and layout is what this check
        // is about. The target picker needs a live scene, so the editor owns that.
        editor::WorldCreatorWindow creator(nullptr);
        creator.setOpen(true);
        for (int frame = 0; frame < 3; frame++) {
            ImGui::NewFrame();
            creator.show();
            ImGui::Render();
        }
        report(creator.isOpen(), "the World Creator window lays out three frames");

        editor::PluginBrowserWindow closed;
        closed.setOpen(false);
        ImGui::NewFrame();
        closed.show();
        ImGui::Render();
        report(!closed.isOpen(), "a closed window draws nothing");
    }

    // Mean |difference| across the tile's seam divided by the same mean across interior neighbours.
    // A lattice that wraps gives ~1; a field that does not close on itself gives a multiple of it,
    // and that multiple is the visible wall where two tiles meet.
    float seamRatio(const std::vector<unsigned char>& pixels, int side, int channels) {
        const size_t needed = static_cast<size_t>(side) * static_cast<size_t>(side) * channels;
        if (side < 2 || pixels.size() < needed) return 1.0f;

        double seam = 0.0;
        double inside = 0.0;
        size_t seamCount = 0;
        size_t insideCount = 0;
        for (int y = 0; y < side; y++) {
            const size_t row = static_cast<size_t>(y) * side * channels;
            for (int c = 0; c < channels; c++) {
                seam += std::fabs(static_cast<double>(pixels[row + c])
                                  - static_cast<double>(pixels[row + static_cast<size_t>(side - 1) * channels + c]));
                seamCount++;
                for (int x = 0; x + 1 < side; x++) {
                    inside += std::fabs(static_cast<double>(pixels[row + static_cast<size_t>(x) * channels + c])
                                        - static_cast<double>(pixels[row + static_cast<size_t>(x + 1) * channels + c]));
                    insideCount++;
                }
            }
        }

        const double seamMean = seam / static_cast<double>(seamCount);
        const double insideMean = inside / static_cast<double>(insideCount);
        if (insideMean <= 1e-6) return seamMean <= 1e-6 ? 1.0f : 1000.0f;
        return static_cast<float>(seamMean / insideMean);
    }

    // Decode one written PNG and report its channel count, or -1 when it does not decode.
    int decodedChannels(const fs::path& file, int& width, int& height) {
        int channels = 0;
        unsigned char* pixels = stbi_load(file.string().c_str(), &width, &height, &channels, 0);
        if (!pixels) return -1;
        stbi_image_free(pixels);
        return channels;
    }

    void checkMaterialMaps(const fs::path& tempDir) {
        std::cout << "-- material library\n";

        const int size = 64;
        TerrainMaterialLibrary::Maps grass;
        TerrainMaterialLibrary::Maps sameSeed;
        TerrainMaterialLibrary::Maps otherSeed;
        TerrainMaterialLibrary::build(TerrainMaterialKind::GRASS, 7, size, grass);
        TerrainMaterialLibrary::build(TerrainMaterialKind::GRASS, 7, size, sameSeed);
        TerrainMaterialLibrary::build(TerrainMaterialKind::GRASS, 8, size, otherSeed);

        const size_t rgb = static_cast<size_t>(size) * size * 3;
        const size_t r8 = static_cast<size_t>(size) * size;
        report(grass.size == size, "build() keeps a power-of-two side", shown(static_cast<float>(grass.size), 0));
        report(TerrainMaterialLibrary::clampedSize(300) == 256
                   && TerrainMaterialLibrary::clampedSize(9) == TerrainMaterialLibrary::MIN_SIZE
                   && TerrainMaterialLibrary::clampedSize(99999) == TerrainMaterialLibrary::MAX_SIZE,
               "clampedSize() rounds into the texture range",
               shown(static_cast<float>(TerrainMaterialLibrary::clampedSize(300)), 0));
        report(grass.baseColor.size() == rgb && grass.normal.size() == rgb
                   && grass.metallicRoughness.size() == rgb && grass.occlusion.size() == r8
                   && grass.height.size() == r8,
               "the five maps carry the channel counts their names promise");
        report(grass.baseColor == sameSeed.baseColor && grass.height == sameSeed.height,
               "the same seed rebuilds the same bytes");
        report(grass.baseColor != otherSeed.baseColor, "another seed gives another map");

        // A detail texture tiles across the terrain, so every map here has to close on itself.
        report(seamRatio(grass.baseColor, size, 3) < 2.0f, "the base colour map tiles without a wall",
               "ratio " + shown(seamRatio(grass.baseColor, size, 3), 2));
        report(seamRatio(grass.height, size, 1) < 2.0f, "the height map tiles without a wall",
               "ratio " + shown(seamRatio(grass.height, size, 1), 2));

        int belowZero = 0;
        int roughOutOfRange = 0;
        for (size_t i = 0; i < rgb; i += 3) {
            if (grass.normal[i + 2] < 128) belowZero++;          // nz is always toward the viewer
            const int rough = grass.metallicRoughness[i + 1];
            if (rough < 30 || rough > 250) roughOutOfRange++;
            if (grass.metallicRoughness[i] != 0) roughOutOfRange++;  // the glTF slot's red is unused
            if (grass.metallicRoughness[i + 2] != 0) roughOutOfRange++; // ground is not metal
        }
        report(belowZero == 0, "every normal texel faces out of the surface", shown(static_cast<float>(belowZero), 0));
        report(roughOutOfRange == 0, "the metallicRoughness map follows the glTF channel split");

        int heightMin = 255;
        int heightMax = 0;
        for (size_t i = 0; i < r8; i++) {
            heightMin = std::min(heightMin, static_cast<int>(grass.height[i]));
            heightMax = std::max(heightMax, static_cast<int>(grass.height[i]));
        }
        report(heightMax - heightMin > 24, "the height map has relief to blend with",
               std::to_string(heightMin) + ".." + std::to_string(heightMax));

        bool namesRoundTrip = true;
        for (const TerrainMaterialKind kind : TerrainMaterialLibrary::allKinds()) {
            TerrainMaterialKind readBack;
            if (!TerrainMaterialLibrary::tryMaterialFromName(TerrainMaterialLibrary::materialName(kind), readBack)
                || readBack != kind) {
                namesRoundTrip = false;
            }
        }
        TerrainMaterialKind ignored;
        report(namesRoundTrip && !TerrainMaterialLibrary::tryMaterialFromName("notamaterial", ignored),
               "every material name round-trips and an unknown one is refused");

        const fs::path dir = tempDir / "materials";
        TerrainMaterialLibrary::Files files;
        Material material;
        std::string error;
        const bool written = TerrainMaterialLibrary::writeMaps(TerrainMaterialKind::SAND, 7, size, dir,
                                                              "assets/probe", files, material, error);
        report(written, "writeMaps() lands five PNGs", error);
        if (written) {
            int width = 0;
            int height = 0;
            report(decodedChannels(dir / "sand_basecolor.png", width, height) == 3 && width == size && height == size,
                   "the base colour PNG decodes as RGB8", shown(static_cast<float>(width), 0));
            report(decodedChannels(dir / "sand_occlusion.png", width, height) == 1,
                   "the occlusion PNG decodes as a single channel");
            report(files.baseColor == "assets/probe/sand_basecolor.png"
                       && material.baseColorTexture.getPath(0) == files.baseColor
                       && material.heightTexture.getPath(0) == files.height,
                   "the material references the path-prefixed files it was given");

            // What a caller does with the material to get a .memory file on disk is editor code, but
            // the round trip through the layer bridge is engine code and is the FASE 4 contract.
            const TerrainSurfaceLayer layer = TerrainMaterialLibrary::layerFor(material,
                TerrainMaterialLibrary::spec(TerrainMaterialKind::SAND), 2.0f);
            report(layer.colorTexture.getPath(0) == files.baseColor
                       && layer.normalTexture.getPath(0) == files.normal
                       && layer.heightTexture.getPath(0) == files.height
                       && layer.uvScale.x == 2.0f,
                   "layerFor() wires all five maps onto a terrain surface layer");
        }

        std::vector<TerrainMaterialLibrary::Entry> entries;
        const std::vector<TerrainMaterialKind> picked = {TerrainMaterialKind::ROCK, TerrainMaterialKind::SNOW};
        report(TerrainMaterialLibrary::writeLibrary(picked, 7, size, tempDir / "library", "", entries, error)
                   && entries.size() == 2 && entries[0].kind == TerrainMaterialKind::ROCK,
               "writeLibrary() honours the kinds and the order it was given", error);
        std::vector<TerrainMaterialLibrary::Entry> none;
        report(!TerrainMaterialLibrary::writeLibrary({}, 7, size, tempDir / "none", "", none, error)
                   && !error.empty(), "an empty request is refused with a message", error);
    }

    void checkSplatBake(const fs::path& tempDir) {
        std::cout << "-- automatic splat\n";

        const int size = 32;
        TerrainSplatRules rules;
        rules.seed = 7;
        rules.resolution = size;
        // A probe asserts on a band edge, not on the noise that softens it, so every synthetic field
        // below gets a predictable answer; the noise has its own determinism check at the end.
        rules.variation = 0.0f;

        float weights[9];
        std::vector<float> shore(static_cast<size_t>(size) * size, rules.seaLevel + 0.02f);
        TerrainSplat::weightsAt(shore, size, 40.0f, 200.0f, rules, size / 2, size / 2, weights);
        report(weights[0] > 0.5f && weights[0] > weights[1], "a flat plane at the water line is beach");

        std::vector<float> plain(static_cast<size_t>(size) * size, 0.36f);
        TerrainSplat::weightsAt(plain, size, 40.0f, 200.0f, rules, size / 2, size / 2, weights);
        report(weights[1] > weights[0] && weights[1] > 0.4f, "a flat plain above the shore is grass");

        std::vector<float> cap(static_cast<size_t>(size) * size, 0.9f);
        TerrainSplat::weightsAt(cap, size, 40.0f, 200.0f, rules, size / 2, size / 2, weights);
        report(weights[6] > 0.5f, "a flat height above the snow line holds snow");

        // One step in x is the cheapest cliff the field can express, and the slope a central
        // difference reads from it is the same quantity the shader's triplanar mix uses.
        std::vector<float> wall(static_cast<size_t>(size) * size, 0.25f);
        for (int y = 0; y < size; y++) {
            for (int x = 16; x < size; x++) wall[static_cast<size_t>(y) * size + x] = 0.55f;
        }
        TerrainSplat::weightsAt(wall, size, 100.0f, 100.0f, rules, 15, size / 2, weights);
        report(weights[4] > 0.4f && weights[4] + weights[5] > 0.7f, "a vertical face becomes rock and cliff");
        TerrainSplat::weightsAt(wall, size, 100.0f, 100.0f, rules, 24, size / 2, weights);
        report(weights[4] + weights[5] < 0.2f, "the flat shoulder of that face is not rock");

        float sum = 0.0f;
        for (int slot = 0; slot < 9; slot++) sum += weights[slot];
        report(sum <= rules.maxWeight + 1e-4f, "one texel never claims more than maxWeight", shown(sum));
        report(weights[8] == 0.0f, "the lava slot stays off while the volcanic threshold is 0");

        TerrainSplat::Maps fromField;
        TerrainSplat::bake(wall, size, 100.0f, 100.0f, rules, fromField);
        report(fromField.resolution == size && fromField.blend[0].size() == static_cast<size_t>(size) * size * 3
                   && fromField.blend[1].size() == static_cast<size_t>(size) * size * 3
                   && fromField.blend[2].size() == static_cast<size_t>(size) * size * 3,
               "bake() gives three full RGB maps", shown(static_cast<float>(fromField.resolution), 0));

        // With the blend grid the same side as the field, a baked texel and weightsAt() are the same
        // query, so the two entry points have to agree byte for byte.
        bool matchesWeightsAt = true;
        for (int y = 0; y < size && matchesWeightsAt; y++) {
            for (int x = 0; x < size; x++) {
                float probe[9];
                TerrainSplat::weightsAt(wall, size, 100.0f, 100.0f, rules, x, y, probe);
                for (int slot = 0; slot < 9; slot++) {
                    const size_t o = (static_cast<size_t>(y) * size + x) * 3;
                    const unsigned char byte = fromField.blend[slot / 3][o + slot % 3];
                    if (std::abs(static_cast<int>(byte) - static_cast<int>(std::lround(probe[slot] * 255.0f))) > 1) {
                        matchesWeightsAt = false;
                        break;
                    }
                }
                if (!matchesWeightsAt) break;
            }
        }
        report(matchesWeightsAt, "bake() and weightsAt() are the same rule set");

        TerrainSplatRules noisy = rules;
        noisy.variation = 0.15f;
        TerrainSplat::Maps noiseA;
        TerrainSplat::Maps noiseB;
        TerrainSplat::bake(shore, size, 40.0f, 200.0f, noisy, noiseA);
        TerrainSplat::bake(shore, size, 40.0f, 200.0f, noisy, noiseB);
        TerrainSplat::Maps noiseOther;
        noisy.seed = 8;
        TerrainSplat::bake(shore, size, 40.0f, 200.0f, noisy, noiseOther);
        report(noiseA.blend[0] == noiseB.blend[0], "the same seed bakes the same blend maps");
        report(noiseA.blend[0] != noiseOther.blend[0], "another seed moves the band edges");

        // The file path has to read the field the generator wrote, so compare it against the same
        // field after the quantization the PNG imposes.
        std::vector<unsigned char> encoded;
        TerrainGenerator::encode16(shore, encoded);
        const fs::path heightPath = tempDir / "splat_height.png";
        report(writeGray16Png(heightPath, size, size, encoded.data(), encoded.size()),
               "the splat probe can write a 16-bit heightmap");
        std::vector<float> quantized(encoded.size());
        for (size_t i = 0; i < encoded.size(); i += 2) {
            const unsigned int low = encoded[i];
            const unsigned int high = encoded[i + 1];
            quantized[i / 2] = static_cast<float>(low | (high << 8)) / 65535.0f;
        }
        TerrainSplat::Maps quantizedMaps;
        TerrainSplat::bake(quantized, size, 40.0f, 200.0f, rules, quantizedMaps);

        TerrainSplat::Maps fromFile;
        std::string readError;
        const bool readBack = TerrainSplat::bakeFromFile(heightPath, 40.0f, 200.0f, rules, fromFile, readError);
        report(readBack && fromFile.resolution == size, "bakeFromFile() decodes a generated heightmap", readError);
        report(readBack && fromFile.blend[0] == quantizedMaps.blend[0] && fromFile.blend[1] == quantizedMaps.blend[1],
               "the map read from disk matches the field it came from");

        std::string missingError;
        TerrainSplat::Maps nothing;
        report(!TerrainSplat::bakeFromFile(tempDir / "nope.png", 40.0f, 200.0f, rules, nothing, missingError)
                   && !missingError.empty(), "a missing heightmap is refused with a message", missingError);

        TerrainSplat::Files blendFiles;
        std::string writeError;
        const bool blendWritten = TerrainSplat::write(fromFile, tempDir / "splat", "assets/probe", blendFiles, writeError);
        report(blendWritten, "write() lands the three blend maps", writeError);
        if (blendWritten) {
            int width = 0;
            int height = 0;
            report(decodedChannels(tempDir / "splat" / "blend_0.png", width, height) == 3
                       && width == size && height == size,
                   "a blend map decodes back as RGB8 at its own resolution");
            report(blendFiles.blend[1] == "assets/probe/blend_1.png"
                       && blendFiles.blend[2] == "assets/probe/blend_2.png",
                   "the returned paths take the prefix the caller asked for");
        }

        TerrainSplatRules lava = rules;
        lava.volcanic = 0.5f;
        float anyLava = 0.0f;
        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++) {
                TerrainSplat::weightsAt(plain, size, 40.0f, 200.0f, lava, x, y, weights);
                anyLava += weights[8];
            }
        }
        report(anyLava > 0.0f, "raising the volcanic threshold lets lava rock onto the map", shown(anyLava));

        report(TerrainSplat::clampedResolution(4) == TerrainSplat::MIN_RESOLUTION
                   && TerrainSplat::clampedResolution(99999) == TerrainSplat::MAX_RESOLUTION,
               "the blend resolution is clamped to what a texture can hold");
        report(TerrainSplat::layerPlan().size() == 9 && TerrainSplat::layerPlan()[0] == TerrainMaterialKind::SAND
                   && TerrainSplat::layerPlan()[8] == TerrainMaterialKind::VOLCANIC,
               "the nine-slot plan matches the maps the shader reads");
    }

    void usage(const char* executableName) {
        std::cout
            << "usage: " << (executableName ? executableName : "quantum-editor-cmd")
            << " worldcreator <selftest>\n"
            << "\n"
            << "  selftest    drive the World Creator code with an ImGui context that has no\n"
            << "              renderer: the plugin schema panel (all 11 control types, real clicks),\n"
            << "              applyEdits -> settings file -> onSettingChanged, the heightmap write\n"
            << "              behind the Generate button, both new windows' frames, the material\n"
            << "              library (channel counts, determinism, tile seams, PNG round trips) and\n"
            << "              the automatic splat (beach/plain/cliff/snow bands, maxWeight, the\n"
            << "              bakeFromFile and blend-map round trips). Prints PASS/FAIL lines and\n"
            << "              exits 1 if any check fails.\n"
            << "\n"
            << "Writes only into a throwaway directory under the system temp folder. Opens no\n"
            << "project, no scene and no window, so nothing here proves pixels.\n";
    }

}

namespace quantum::editor::cli {

    int runWorldCreatorCommand(int argc, char** argv, const char* executableName) {
        const std::string verb = argc >= 2 ? argv[1] : std::string();
        if (verb == "help" || verb == "--help" || verb == "-h") {
            usage(executableName);
            return 0;
        }
        if (verb.empty() || verb != "selftest") {
            if (!verb.empty()) std::cout << "unknown worldcreator verb: " << verb << "\n";
            usage(executableName);
            return verb.empty() ? 1 : 2;
        }

        std::error_code ec;
        const fs::path tempDir = fs::temp_directory_path(ec) / "quantum-worldcreator-probe";
        if (ec) {
            std::cout << "FAIL no temp directory for the probe\n";
            return 1;
        }
        fs::remove_all(tempDir, ec);
        fs::create_directories(tempDir, ec);
        std::cout << "probe workdir " << tempDir.generic_string() << "\n";

        checkSchemaShape();
        checkColourText();
        checkInteractions();
        checkApplyEdits(tempDir);
        checkHeightmapWrite(tempDir);
        checkWindowsDraw();
        checkMaterialMaps(tempDir);
        checkSplatBake(tempDir);

        std::cout << (failedCount == 0 ? "OK" : "FAILED") << " " << passedCount << " passed, "
                  << failedCount << " failed\n";
        std::cout << "unverified: pixels (this context has no renderer), the target picker and the\n"
                  << "undo of an applied map - all three need a live scene in the editor.\n";
        return failedCount == 0 ? 0 : 1;
    }
}
