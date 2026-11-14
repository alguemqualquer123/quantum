// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "window/PluginBrowserWindow.h"

#include "window/PluginSchemaPanel.h"
#include "Out.h"

#include "PluginManifest.h"

#include <cstdarg>
#include <cstdio>
#include <string>

using namespace quantum;
using namespace quantum::plugin;

namespace {

    const char* levelName(PluginLogLevel level) {
        switch (level) {
            case PluginLogLevel::Verbose: return "verbose";
            case PluginLogLevel::Debug: return "debug";
            case PluginLogLevel::Warning: return "warning";
            case PluginLogLevel::Error: return "error";
            case PluginLogLevel::Info: break;
        }
        return "info";
    }

    ImU32 levelColor(PluginLogLevel level) {
        switch (level) {
            case PluginLogLevel::Error: return ImGui::GetColorU32(ImVec4(0.90f, 0.33f, 0.30f, 1.0f));
            case PluginLogLevel::Warning: return ImGui::GetColorU32(ImVec4(0.90f, 0.68f, 0.25f, 1.0f));
            case PluginLogLevel::Debug:
            case PluginLogLevel::Verbose: return ImGui::GetColorU32(ImVec4(0.55f, 0.58f, 0.62f, 1.0f));
            case PluginLogLevel::Info: break;
        }
        return ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    }

    // The three colours a row can have: running, refused before the binary was mapped, and
    // broken after it was. A user scanning the list needs the difference at a glance.
    ImU32 stateColor(PluginState state) {
        switch (state) {
            case PluginState::Active:
            case PluginState::Initialized: return ImGui::GetColorU32(ImVec4(0.35f, 0.78f, 0.40f, 1.0f));
            case PluginState::Incompatible: return ImGui::GetColorU32(ImVec4(0.90f, 0.68f, 0.25f, 1.0f));
            case PluginState::Failed: return ImGui::GetColorU32(ImVec4(0.90f, 0.33f, 0.30f, 1.0f));
            case PluginState::Disabled:
            case PluginState::Discovered:
            case PluginState::Loaded: break;
        }
        return ImGui::GetColorU32(ImVec4(0.55f, 0.58f, 0.62f, 1.0f));
    }

    void drawKeyValue(const char* key, const std::string& value) {
        if (value.empty()) return;
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", key);
        ImGui::SameLine(140.0f);
        ImGui::TextWrapped("%s", value.c_str());
    }

    std::string joinValues(const UiValueMap& values) {
        std::string text;
        for (const auto& entry : values) {
            if (!text.empty()) text += ", ";
            text += entry.first;
            text += "=";
            text += entry.second.toString("-");
        }
        return text;
    }

}

editor::PluginBrowserWindow::PluginBrowserWindow()
    : manager(PluginManager::instance()) {
}

void editor::PluginBrowserWindow::show() {
    if (!windowOpen) return;

    if (focusRequested) {
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }

    const bool wasOpen = windowOpen;
    if (!ImGui::Begin(WINDOW_NAME, &windowOpen)) {
        ImGui::End();
        if (wasOpen && !windowOpen) setOpen(false);
        return;
    }

    // The record list is the manager's, and it changes when a plugin is installed, so it is read
    // every frame rather than cached.
    const std::vector<PluginRecord*>& records = manager.records();
    if (selected >= static_cast<int>(records.size())) selected = records.empty() ? -1 : 0;

    drawToolbar();
    ImGui::Spacing();

    ImGui::BeginGroup();
    drawList();
    ImGui::EndGroup();

    ImGui::SameLine();
    ImGui::BeginGroup();
    if (PluginRecord* record = selectedRecord()) {
        drawDetails(*record);
    } else {
        ImGui::TextDisabled("No plugin selected. The directory shown above is what was scanned.");
    }
    ImGui::EndGroup();

    ImGui::End();
}

void editor::PluginBrowserWindow::drawToolbar() {
    if (ImGui::Button("Rescan")) {
        const int found = manager.discover();
        setStatus("discover: %d record(s)", found);
    }
    ImGui::SameLine();
    if (ImGui::Button("Load all")) {
        const int loaded = manager.loadAll();
        setStatus("loadAll: %d plugin(s) up", loaded);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear log")) {
        manager.clearLog();
    }

    ImGui::SameLine();
    ImGui::TextDisabled("%d active, %d failed", manager.activeCount(), manager.failedCount());

    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - 150.0f);
    ImGui::InputText("##installpath", installPathInput, sizeof(installPathInput));
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Install")) {
        std::string error;
        const std::string id = manager.installFromDirectory(installPathInput, overwriteInstall, error);
        if (id.empty()) {
            setStatus("install failed: %s", error.c_str());
            Out::error("Plugin install failed: %s", error.c_str());
        } else {
            setStatus("installed %s", id.c_str());
            Out::success("Plugin installed: %s", id.c_str());
        }
    }
    ImGui::SameLine();
    ImGui::Checkbox("Overwrite", &overwriteInstall);

    ImGui::TextDisabled("plugins: %s", manager.pluginsDirectory().c_str());
    if (!statusLine.empty()) ImGui::TextUnformatted(statusLine.c_str());
}

void editor::PluginBrowserWindow::drawList() {
    ImGui::TextDisabled("Plugins");
    const ImVec2 listSize(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y - 24.0f);
    if (!ImGui::BeginChild("pluginlist", listSize, ImGuiChildFlags_Borders)) {
        ImGui::EndChild();
        return;
    }

    const std::vector<PluginRecord*>& records = manager.records();
    for (int index = 0; index < static_cast<int>(records.size()); index++) {
        const PluginRecord* record = records[index];
        if (!record) continue;

        const std::string label = record->manifest.name.empty() ? record->manifest.id
                                                               : record->manifest.name;
        ImGui::PushStyleColor(ImGuiCol_Text, stateColor(record->state));
        ImGui::PushID(index);
        if (ImGui::Selectable(label.c_str(), selected == index)) selected = index;
        ImGui::PopID();
        ImGui::PopStyleColor();

        if (ImGui::IsItemHovered() && !record->error.empty()) {
            ImGui::SetTooltip("%s", record->error.c_str());
        }
    }

    if (records.empty()) {
        ImGui::TextDisabled("none found");
    }
    ImGui::EndChild();
}

void editor::PluginBrowserWindow::drawDetails(PluginRecord& record) {
    const ImVec2 detailSize(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y - 24.0f);
    if (!ImGui::BeginChild("plugindetails", detailSize)) {
        ImGui::EndChild();
        return;
    }

    ImGui::SeparatorText(record.manifest.name.empty() ? record.manifest.id.c_str()
                                                     : record.manifest.name.c_str());
    drawManifest(record);

    ImGui::Spacing();
    ImGui::SeparatorText("Controls");
    // Not const: ImGui::Checkbox writes the new value here, and that is what setEnabled receives.
    bool enabled = manager.isEnabled(record.manifest.id);
    if (ImGui::Checkbox("Enabled", &enabled)) {
        const bool applied = manager.setEnabled(record.manifest.id, enabled);
        setStatus("%s: %s%s", record.manifest.id.c_str(), enabled ? "enabled" : "disabled",
                  applied ? "" : " refused");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload")) {
        setStatus("%s: reload %s", record.manifest.id.c_str(),
                  manager.reload(record.manifest.id) ? "ok" : "failed");
    }
    ImGui::SameLine();
    if (ImGui::Button(record.state == PluginState::Active || record.state == PluginState::Initialized
                          ? "Unload"
                          : "Load")) {
        const bool loaded = record.instance == nullptr ? manager.load(record.manifest.id)
                                                     : manager.unload(record.manifest.id);
        setStatus("%s: %s", record.manifest.id.c_str(), loaded ? "loaded" : "unload refused");
    }
    ImGui::SameLine();
    if (ImGui::Button("Rebuild panel")) {
        manager.refreshSchema(record.manifest.id);
    }

    drawPanel(record);
    drawSettings(record);
    drawLog(record.manifest.id);

    ImGui::EndChild();
}

void editor::PluginBrowserWindow::drawManifest(PluginRecord& record) {
    const PluginManifest& manifest = record.manifest;

    drawKeyValue("id", manifest.id);
    drawKeyValue("version", manifest.version);
    drawKeyValue("author", manifest.author);
    drawKeyValue("description", manifest.description);
    drawKeyValue("type", manifest.type);

    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("state");
    ImGui::SameLine(140.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, stateColor(record.state));
    ImGui::TextUnformatted(pluginStateName(record.state));
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::Text("abi %u (binary reports %u)", manifest.abiVersion, record.reportedAbiVersion);

    if (!record.error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImVec4(0.90f, 0.33f, 0.30f, 1.0f)));
        ImGui::TextWrapped("%s", record.error.c_str());
        ImGui::PopStyleColor();
    }
    if (!manifest.valid) {
        const std::string errors = manifest.errorsAsText();
        if (!errors.empty()) ImGui::TextWrapped("plugin.json: %s", errors.c_str());
    }

    drawKeyValue("directory", manifest.directory);
    drawKeyValue("library", record.libraryPath.empty() ? manifest.libraryFileName()
                                                      : record.libraryPath);

    if (!manifest.permissions.empty()) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("permissions");
        ImGui::SameLine(140.0f);
        std::string text;
        for (const std::string& permission : manifest.permissions) {
            if (!text.empty()) text += ", ";
            text += permission;
            // Only three of these the engine actually gates; the rest are declarations, and the
            // row says which is which rather than implying more protection than there is.
            text += isEnforcedPermission(permission) ? "(enforced)" : "(declared)";
            if (!isKnownPermission(permission)) text += "(unknown)";
        }
        ImGui::TextWrapped("%s", text.c_str());
    }

    if (!manifest.commands.empty()) {
        std::string text;
        for (const std::string& command : manifest.commands) {
            if (!text.empty()) text += ", ";
            text += command;
        }
        drawKeyValue("commands", text);
    }
}

void editor::PluginBrowserWindow::drawPanel(PluginRecord& record) {
    if (!record.instance) {
        if (!record.manifest.panelTitle.empty()) {
            ImGui::TextDisabled("Panel unavailable: the plugin is not loaded.");
        }
        return;
    }

    if (manager.schemaOf(record.manifest.id) == nullptr || record.schema.groups.empty()) {
        ImGui::TextDisabled("This plugin describes no panel.");
        return;
    }

    ImGui::Spacing();
    ImGui::SeparatorText(record.schema.title.empty() ? "Panel" : record.schema.title.c_str());

    PluginSchemaPanel::Touched touched;
    PluginSchemaPanel::draw(record.manifest.id, record.schema, touched);

    if (!touched.edited.empty()) {
        std::string error;
        if (!PluginSchemaPanel::applyEdits(record, record.schema, touched.edited, error)) {
            setStatus("settings not saved: %s", error.c_str());
            Out::warning("Plugin settings could not be saved: %s", error.c_str());
        }
    }

    for (const std::string& command : touched.pressed) {
        UiValue result;
        std::string error;
        if (manager.executeCommand(command, collectSchemaValues(record.schema), result, error)) {
            lastResult = command + " -> " + result.toString("-");
        } else {
            lastResult = command + " refused: " + error;
        }
    }

    if (!lastResult.empty()) ImGui::TextWrapped("%s", lastResult.c_str());
}

void editor::PluginBrowserWindow::drawSettings(PluginRecord& record) {
    ImGui::Spacing();
    ImGui::SeparatorText("Settings");
    drawKeyValue("file", record.settings.path());

    const UiValueMap& values = record.settings.values();
    if (values.empty()) {
        ImGui::TextDisabled("no keys");
    } else {
        ImGui::TextWrapped("%s", joinValues(values).c_str());
    }

    if (record.settings.isDirty()) ImGui::TextDisabled("unsaved changes");
    const std::string& loadError = record.settings.lastError();
    if (!loadError.empty()) ImGui::TextWrapped("settings: %s", loadError.c_str());
}

void editor::PluginBrowserWindow::drawLog(const std::string& pluginId) {
    ImGui::Spacing();
    ImGui::SeparatorText("Log");

    const std::vector<PluginLogEntry>& entries = manager.log();
    int shown = 0;
    for (const PluginLogEntry& entry : entries) {
        if (!pluginId.empty() && entry.pluginId != pluginId) continue;
        ImGui::PushStyleColor(ImGuiCol_Text, levelColor(entry.level));
        ImGui::Text("[%s] %s", levelName(entry.level), entry.message.c_str());
        ImGui::PopStyleColor();
        shown++;
    }
    if (shown == 0) ImGui::TextDisabled("nothing logged");
}

PluginRecord* editor::PluginBrowserWindow::selectedRecord() {
    const std::vector<PluginRecord*>& records = manager.records();
    if (selected < 0 || selected >= static_cast<int>(records.size())) return nullptr;
    return records[selected];
}

void editor::PluginBrowserWindow::setStatus(const char* format, ...) {
    char text[512];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    statusLine = text;
}

void editor::PluginBrowserWindow::setOpen(bool open) {
    if (open == windowOpen) return;
    windowOpen = open;
    if (open) {
        focusRequested = true;
        if (manager.records().empty()) manager.discover();
        if (selected < 0 && !manager.records().empty()) selected = 0;
    }
}

bool editor::PluginBrowserWindow::isOpen() const {
    return windowOpen;
}

void editor::PluginBrowserWindow::focus() {
    setOpen(true);
}
