// (c) SR VINIX
// SPDX-License-Identifier: MIT
//
// Reference plugin. It is the FASE 1 acceptance test: if this loads, describes a panel, answers a
// command and unloads without leaking the process, the plugin system works. Everything a real
// plugin does goes through the same calls - buildUiSchema, executeCommand, settings - and since
// ABI 2 also a hosted window (onDrawWindow / onWindowInput), a menu item, and its own writable
// data directory through the "files" capability.

#include "PluginInterface.h"

#include <string>
#include <vector>

using namespace quantum::plugin;

namespace {

    // Uppercasing without <algorithm> and without a locale: the "loud" setting is a demo of a
    // boolean coming back through the schema, not a text-processing exercise.
    std::string shout(const std::string& text) {
        std::string out;
        out.reserve(text.size());
        for (char c : text) {
            out.push_back(c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
        }
        return out;
    }

    const char* const STATS_WINDOW_ID = "hello.stats";
    const char* const NOTES_FILE = "notes.txt";

}

class HelloPlugin: public PluginBase {
public:
    const char* pluginId() const override { return "quantum.hello"; }
    const char* pluginVersion() const override { return "1.0.0"; }

    bool initialize(PluginContext& context) override {
        if (!PluginBase::initialize(context)) return false;

        context.registerCommand("hello.ping", "Greets, and counts how many times it has been called");
        context.registerCommand("hello.echo", "Returns the \"text\" argument back unchanged");
        context.registerCommand("hello.calls", "How many hello.ping calls this instance has served");
        context.registerCommand("hello.ticks", "How many frames this instance has been ticked for");

        // The ABI 2 surface. Both of these are gated calls: a plugin.json without "ui" in its
        // permissions gets one warning line in the plugin log and no window, which is the whole
        // point of the capability - the editor menu cannot grow from a plugin nobody authorised.
        context.registerCommand("hello.open.stats",
                                "Asks the editor to open the stats window this plugin registered");
        context.registerCommand("hello.note", "Appends a line to notes.txt in the plugin's own data");
        context.registerCommand("hello.stats", "How many notes are on disk, and where they live");

        if (context.registerWindow(STATS_WINDOW_ID, "Hello Stats", "Tools", "", 380, 340)) {
            // The command is registered above, so the item is allowed to name it. A menu item that
            // named a command this plugin does not own is refused with a line saying so.
            context.registerMenuItem("Hello Stats", "hello.open.stats", "Tools");
        }

        if (context.hasPermission("log")) {
            context.log(PluginLogLevel::Info, "ready, greeting \"%s\"",
                        context.settings().getString("greeting").c_str());
        }
        return true;
    }

    bool activate() override {
        servedCalls = 0;
        ticks = 0;
        accumulatedSeconds = 0.0;
        return true;
    }

    void shutdown() override {
        if (pluginContext && pluginContext->hasPermission("settings")) {
            pluginContext->settings().save();
        }
    }

    void buildUiSchema(UiSchemaBuilder& builder) override {
        PluginSettings& settings = context().settings();

        builder.title("Hello");

        builder.group("Greeting");
        builder.text("greeting", "Greeting", settings.getString("greeting"));
        builder.boolean("loud", "Shout it", settings.getBool("loud"));

        builder.group("Ping");
        builder.intSlider("repeat", "Repeat", settings.getInt("repeat", 1), 1, 8, 1);
        builder.button("hello.ping", "Ping now");
        builder.label("hello.ping answers through the command registry; the counter lives in the "
                      "plugin instance, not in the panel.");

        // The tick counter, read at describe() time: a plugin that never gets called even though it
        // declares "tick" shows up here as 0, which is the difference between a bad manifest and a
        // missing engine call.
        builder.group("Runtime");
        builder.label("Ticked " + std::to_string(ticks) + " frame(s), " +
                      std::to_string(accumulatedSeconds) + " s accumulated.");
    }

    bool executeCommand(const std::string& command, const UiValueMap& arguments,
                        UiValue& result) override {
        if (command == "hello.echo") {
            auto text = arguments.find("text");
            result = UiValue::fromString(text == arguments.end() ? std::string()
                                                                 : text->second.toString());
            return true;
        }

        if (command == "hello.calls") {
            result = UiValue::fromInt(servedCalls);
            return true;
        }

        if (command == "hello.ticks") {
            result = UiValue::fromInt(ticks);
            return true;
        }

        // The window request. It is answered by the editor when it next draws, not here - a plugin
        // that runs a command from a Lua hook has no window to open by itself.
        if (command == "hello.open.stats") {
            result = UiValue::fromBool(context().openWindow(STATS_WINDOW_ID));
            return true;
        }

        if (command == "hello.note") {
            long long lines = 1;
            auto count = arguments.find("lines");
            if (count != arguments.end()) lines = count->second.toInt(1);
            result = UiValue::fromInt(writeNote(lines));
            return true;
        }

        if (command == "hello.stats") {
            std::string text = std::to_string(countNoteLines()) + " note line(s), " +
                               std::to_string(windowSubmits) + " window input(s), in " +
                               context().dataDirectory();
            result = UiValue::fromString(text);
            return true;
        }

        if (command != "hello.ping") return false;

        PluginSettings& settings = context().settings();

        auto repeat = arguments.find("repeat");
        long long times = repeat == arguments.end()
            ? settings.getInt("repeat", 1)
            : repeat->second.toInt(1);
        if (times < 1) times = 1;
        if (times > 8) times = 8;

        std::string greeting = settings.getString("greeting");
        if (settings.getBool("loud")) greeting = shout(greeting);

        std::string message;
        for (long long i = 0; i < times; i++) {
            if (!message.empty()) message += " ";
            message += greeting;
        }

        servedCalls++;
        result = UiValue::fromString(message);
        return true;
    }

    // The reference for every runtime plugin: the engine only asks when plugin.json names "tick",
    // and the delta is the capped frame delta the scenes saw.
    void onUpdate(float deltaTime) override {
        ticks++;
        accumulatedSeconds += deltaTime;

        if (ticks == 1 && context().hasPermission("log")) {
            context().log(PluginLogLevel::Info, "first tick, %.3f s", deltaTime);
        }
    }

    void onSettingChanged(const std::string& key, const UiValue& value) override {
        PluginSettings& settings = context().settings();
        settings.set(key, value);
        if (key == "repeat") settings.save();
    }

    // ABI 2. The editor calls this every frame its hosted window is open, and the plugin owns the
    // layout - no ImGui type crosses the line, which is why this binary links against the engine
    // only. Describing nothing is allowed and shows an empty window.
    void onDrawWindow(const std::string& windowId, UiSchemaBuilder& builder) override {
        if (windowId != STATS_WINDOW_ID) return;

        builder.title("Hello Stats");

        builder.group("Runtime");
        builder.label("Served " + std::to_string(servedCalls) + " command(s), ticked " +
                      std::to_string(ticks) + " frame(s).");
        builder.label("The editor hosts this window; the plugin only described it.");

        builder.group("Notes");
        builder.intInput("lines", "Lines per note", linesPerNote, 1, 4);
        builder.button("note", "Append to notes.txt");
        builder.button("clear", "Remove notes.txt");
        builder.separator();
        builder.label(std::to_string(countNoteLines()) + " line(s) in the plugin's own data "
                      "directory, described at draw time so the number is the file's, not a "
                      "cached one.");
        builder.text("last", "Last note", lastNote);
    }

    // ABI 2. What the user did to the window. Values arrive for every field that carries data and
    // the buttons that were pressed, and this is only called when something actually happened.
    void onWindowInput(const std::string& windowId, const UiValueMap& values,
                       const std::vector<std::string>& pressedButtons) override {
        if (windowId != STATS_WINDOW_ID) return;

        windowSubmits++;

        auto lines = values.find("lines");
        if (lines != values.end()) {
            linesPerNote = static_cast<long long>(lines->second.toInt(1));
            if (linesPerNote < 1) linesPerNote = 1;
            if (linesPerNote > 4) linesPerNote = 4;
        }

        for (const std::string& button : pressedButtons) {
            if (button == "note") {
                writeNote(linesPerNote);
                continue;
            }
            if (button != "clear") continue;
            context().removeFile(NOTES_FILE);
            lastNote.clear();
        }
    }

private:
    // The "files" capability in its whole truth: one relative name, the engine's own join, and a
    // refusal for anything that tries to leave the directory. Nothing here knows the absolute path
    // until the plugin prints it in hello.stats.
    long long writeNote(long long lines) {
        if (lines < 1) lines = 1;
        if (lines > 4) lines = 4;

        std::string text;
        for (long long i = 0; i < lines; i++) {
            text += "note " + std::to_string(notesWritten + i + 1) + " from " +
                    context().manifest().name + "\n";
        }
        if (!context().appendTextFile(NOTES_FILE, text)) return notesWritten;
        notesWritten += lines;

        readLastNote();
        return notesWritten;
    }

    void readLastNote() {
        lastNote.clear();
        std::string content;
        if (!context().readFile(NOTES_FILE, content)) return;

        const size_t body = content.find_last_not_of("\r\n");
        if (body == std::string::npos) return;
        const size_t start = content.find_last_of("\r\n", body);
        lastNote = start == std::string::npos ? content.substr(0, body + 1)
                                              : content.substr(start + 1, body - start);
    }

    long long countNoteLines() {
        std::string content;
        if (!context().readFile(NOTES_FILE, content)) return 0;

        long long lines = 0;
        for (char c : content) if (c == '\n') lines++;
        return lines;
    }

    long long servedCalls = 0;
    long long ticks = 0;
    float accumulatedSeconds = 0.0f;

    // The window state. `notesWritten` counts what this instance appended, while the label in the
    // window reads the file itself - so a note written by the command line shows up in the window
    // without the plugin having to keep a second copy of the truth.
    long long notesWritten = 0;
    long long windowSubmits = 0;
    long long linesPerNote = 1;
    std::string lastNote;
};

QUANTUM_PLUGIN_ENTRY(HelloPlugin)
