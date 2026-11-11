// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "EditorSettingsWindow.h"

#include "AppSettings.h"
#include "App.h"
#include "Backend.h"
#include "Catalog.h"
#include "Engine.h"
#include "EngineApiIndex.h"
#include "KeyBindings.h"
#include "Project.h"
#include "Theme.h"
#include "ai/McpServer.h"
#include "ai/SecretStore.h"
#include "cli/ProjectFileType.h"
#include "cli/ShortcutCommands.h"
#include "command/CommandHandle.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/ScenePropertyCmd.h"
#include "external/IconsFontAwesome6.h"
#include "util/FileDialogs.h"
#include "util/ProjectFile.h"
#include "window/Widgets.h"
#include "misc/cpp/imgui_stdlib.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iterator>
#include <string_view>

namespace quantum::editor {

namespace {
    namespace fs = std::filesystem;

    static constexpr float dialogWidth = 700.0f;
    static constexpr float dialogHeight = 620.0f;
    static constexpr float settingsLabelWidth = 190.0f;
    static constexpr float settingsPanelPadding = 12.0f;
    static constexpr float settingsButtonWidth = 120.0f;
    static constexpr ImGuiWindowFlags noScrollFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    const ImVec4 keySetColor(0.55f, 0.85f, 0.55f, 1.0f);

    // One scrolling child per tab, like the Project Settings panels
    bool beginSettingsPanel(const char* panelId) {
        const ImGuiStyle& style = ImGui::GetStyle();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Theme::dpi(ImVec2(settingsPanelPadding, settingsPanelPadding)));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(style.CellPadding.x, Theme::dpi(6.0f)));
        return ImGui::BeginChild(panelId, ImVec2(0, 0), ImGuiChildFlags_Borders);
    }

    void endSettingsPanel() {
        ImGui::EndChild();
        ImGui::PopStyleVar(3);
    }

    bool beginSettingsTable(const char* tableId) {
        float labelWidth = std::min(Theme::dpi(settingsLabelWidth), ImGui::GetContentRegionAvail().x * 0.4f);
        if (!ImGui::BeginTable(tableId, 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
            return false;
        }
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        return true;
    }

    template <typename DrawContents>
    void drawSettingsPanel(const char* panelId, DrawContents drawContents) {
        if (beginSettingsPanel(panelId) && beginSettingsTable("##EditorSettingsTable")) {
            drawContents();
            ImGui::EndTable();
        }
        endSettingsPanel();
    }

    // Section text that belongs outside the row table
    void drawSectionNote(const char* text) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::TextWrapped("%s", text);
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    void drawSectionTitle(const char* title) {
        ImGui::Spacing();
        ImGui::PushStyleVar(ImGuiStyleVar_SeparatorTextAlign, ImVec2(0.5f, 0.5f));
        ImGui::SeparatorText(title);
        ImGui::PopStyleVar();
    }

    void setBuffer(char* buffer, size_t size, const std::string& value) {
        std::snprintf(buffer, size, "%s", value.c_str());
    }

    // Filtering is case-insensitive over ASCII only: a chord is a key name plus modifiers,
    // and labels are English, so the locale-independent fold cannot mangle a needle.
    std::string lowerCopy(std::string_view text) {
        std::string out(text);
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    }

    // Borderless dim icon after a label
    bool inlineIconButton(const char* icon, const std::string& id, const char* tooltip) {
        ImGui::SameLine(0.0f, 2.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, ImGui::GetStyle().FramePadding.y));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        bool clicked = ImGui::Button((std::string(icon) + "##" + id).c_str());
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
        ImGui::SetItemTooltip("%s", tooltip);
        return clicked;
    }

    // Read-only value styled like Widgets::pathDisplay; the button copies `copied`
    void drawCopyField(const char* id, const std::string& shown, const std::string& copied, const char* tooltip) {
        const float buttonSize = ImGui::GetFrameHeight();
        const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x - buttonSize - ImGui::GetStyle().ItemSpacing.x);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(50, 50, 50, 255));
        ImGui::BeginChild(id, ImVec2(width, ImGui::GetFrameHeight()), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::SetCursorPosY(ImGui::GetStyle().FramePadding.y);
        ImGui::TextUnformatted(shown.c_str());
        ImGui::EndChild();
        ImGui::SetItemTooltip("%s", shown.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (Widgets::iconButton((std::string(id) + "Copy").c_str(), ICON_FA_COPY, ImVec2(buttonSize, buttonSize))) {
            ImGui::SetClipboardText(copied.c_str());
        }
        ImGui::SetItemTooltip("%s", tooltip);
    }

    bool beginSettingsRow(const char* label, const char* tooltip = nullptr, bool defChanged = false) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        if (tooltip) ImGui::SetItemTooltip("%s", tooltip);

        bool reset = defChanged && inlineIconButton(ICON_FA_ROTATE_LEFT, label, "Restore default");

        ImGui::TableNextColumn();
        return reset;
    }

    void showDisabledItemTooltip(const std::string& text) {
        if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) return;

        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 25.0f);
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    float buttonVisibleWidth(const char* label) {
        return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    }

    void drawPathPicker(
        const char* id,
        const char* browseId,
        const char* autoId,
        fs::path displayPath,
        bool canAuto,
        bool selectDirectory,
        const std::function<void(const std::string&)>& onBrowse,
        const std::function<void()>& onAuto
    ) {
        const ImGuiStyle& style = ImGui::GetStyle();
        float browseWidth = buttonVisibleWidth(browseId);
        float autoWidth = buttonVisibleWidth(autoId);
        float pathWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x - browseWidth - autoWidth - style.ItemSpacing.x * 2.0f);

        Widgets::pathDisplay(id, displayPath, Vector2(pathWidth, ImGui::GetFrameHeight()));

        ImGui::SameLine();
        if (ImGui::Button(browseId)) {
            std::string startDir;
            if (!displayPath.empty() && displayPath.string().rfind("<", 0) != 0) {
                startDir = displayPath.parent_path().string();
            }
            std::string selectedPath = FileDialogs::openFileDialog(startDir, FILE_DIALOG_ALL, selectDirectory);
            if (!selectedPath.empty()) {
                onBrowse(selectedPath);
            }
        }

        ImGui::SameLine();
        ImGui::BeginDisabled(!canAuto);
        if (ImGui::Button(autoId)) {
            onAuto();
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Clear the override and use auto-detect again.");
    }
}

void EditorSettingsWindow::open(Project* project, std::optional<Tab> tab, bool expandAiChat) {
    m_isOpen = true;
    m_requestedTab = tab;
    m_expandAiChat = expandAiChat;
    m_project = project;
    m_cacheStatus.clear();
    // The Graphics tab mirrors project.yaml into its buffers on first draw, so a reopen
    // after Cancel reads the stored values again instead of the discarded ones.
    m_graphicsLoaded = false;
    m_graphicsScene = 0;
    m_capturedId.clear();
    m_shortcutError.clear();
    m_shortcutNote.clear();
    // The API rows come from compiled-in metadata, but the filters are session state: a
    // reopen must not come back scoped to a class the user was looking at an hour ago.
    m_apiSearch[0] = '\0';
    m_apiKind = 0;
    m_apiOwner = 0;
    m_apiNote.clear();
    m_apiOwners = EngineApiIndex::owners();
    rebuildApiRows();

    m_kitsDetected = false;
    m_cmakeOverride = AppSettings::getCMakePath();
    m_cmakePickError.clear();

    m_emsdkDetected = false;
    m_emsdkOverride = AppSettings::getEmsdkPath();
    m_editorVSyncEnabled = AppSettings::getEditorVSyncEnabled();
    m_uiScalePercent = static_cast<int>(std::lround(AppSettings::getUiScale() * 100.0f));
    m_viewportRenderDistance = AppSettings::getViewportRenderDistance();
    m_defaultExportDirectory = AppSettings::getDefaultExportDirectory();
    // Shown in the normalized form, because that is what a marker file is named.
    setBuffer(m_projectFileExtension.data(), m_projectFileExtension.size(),
              cli::ProjectFileType::resolveExtension());

    m_aiSettings = AppSettings::getAiSettings();
    m_aiKeys.clear();
    m_aiEndpoints.clear();
    m_removedEndpoints.clear();
    m_editingEndpointId.clear();
    m_focusEndpointLabel = false;
    syncAiBuffers();

    m_mcpSettings = AppSettings::getMcpSettings();
    m_mcpToken = ai::McpServer::storedToken();
}

void EditorSettingsWindow::detectKits() {
    m_availableKits = Generator::detectAvailableKits();
    m_cmakeKitIndex = 0;
    refreshCMakeStatus();

    const LocalBuildSettings currentBuild = projectBuildSettings();
    const std::string& currentCxx = currentBuild.cxxCompiler;
    const std::string& currentGen = currentBuild.generator;
    if (!currentCxx.empty() || !currentGen.empty()) {
        for (size_t i = 0; i < m_availableKits.size(); i++) {
            if (m_availableKits[i].available && m_availableKits[i].cxxCompiler == currentCxx && m_availableKits[i].generator == currentGen) {
                m_cmakeKitIndex = static_cast<int>(i + 1);
                break;
            }
        }
    }
    m_kitsDetected = true;
}

LocalBuildSettings EditorSettingsWindow::projectBuildSettings() const {
    if (!m_project) return LocalBuildSettings();
    return AppSettings::getBuildSettings(m_project->getProjectPath() / "project.yaml");
}

void EditorSettingsWindow::refreshCMakeStatus() {
    m_cmakeInfo = Generator::detectCMake();
}

void EditorSettingsWindow::refreshEmsdkStatus() {
    m_emsdkInfo = Exporter::detectEmsdk(m_emsdkOverride);
}

ImGuiTabItemFlags EditorSettingsWindow::tabFlags(Tab tab) const {
    return m_requestedTab == tab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
}

// Adds buffers for new rows and drops gone ones, keeping typed text intact.
void EditorSettingsWindow::syncAiBuffers() {
    std::map<std::string, KeyEntry> keys;
    for (const ai::ProviderAccount& account : ai::listAccounts(m_aiSettings)) {
        auto existing = m_aiKeys.find(account.id);
        keys[account.id] = existing != m_aiKeys.end() ? existing->second : KeyEntry{};
    }
    m_aiKeys = std::move(keys);

    std::map<std::string, EndpointEntry> endpoints;
    for (const ai::CustomEndpoint& endpoint : m_aiSettings.customEndpoints) {
        auto existing = m_aiEndpoints.find(endpoint.id);
        if (existing != m_aiEndpoints.end()) {
            endpoints[endpoint.id] = existing->second;
            continue;
        }
        EndpointEntry entry;
        setBuffer(entry.label.data(), entry.label.size(), endpoint.label);
        setBuffer(entry.url.data(), entry.url.size(), endpoint.url);
        endpoints[endpoint.id] = entry;
    }
    m_aiEndpoints = std::move(endpoints);

    refreshAiKeyState();
}

void EditorSettingsWindow::refreshAiKeyState() {
    for (auto& entry : m_aiKeys) {
        entry.second.configured = ai::SecretStore::hasApiKey(entry.first);
    }
}

void EditorSettingsWindow::addAiEndpoint(const std::string& label, const std::string& url) {
    ai::CustomEndpoint endpoint;
    endpoint.id = ai::makeEndpointId(m_aiSettings, label);
    endpoint.label = label;
    endpoint.url = url;
    m_aiSettings.customEndpoints.push_back(endpoint);
    syncAiBuffers();
}

void EditorSettingsWindow::removeAiEndpoint(const std::string& endpointId) {
    m_removedEndpoints.push_back(endpointId);
    auto& endpoints = m_aiSettings.customEndpoints;
    endpoints.erase(std::remove_if(endpoints.begin(), endpoints.end(),
                                   [&](const ai::CustomEndpoint& endpoint) {
                                       return endpoint.id == endpointId;
                                   }),
                    endpoints.end());
    if (m_editingEndpointId == endpointId) {
        m_editingEndpointId.clear();
    }
    syncAiBuffers();
}

void EditorSettingsWindow::show() {
    if (!m_isOpen) return;

    ImGui::OpenPopup("Editor Settings##EditorSettingsModal");

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 center = viewport->GetWorkCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImVec2 size(
        std::min(Theme::dpi(dialogWidth), viewport->WorkSize.x * 0.9f),
        std::min(Theme::dpi(dialogHeight), viewport->WorkSize.y * 0.9f)
    );
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_Modal |
                             ImGuiWindowFlags_NoResize |
                             noScrollFlags;

    bool popupOpen = ImGui::BeginPopupModal("Editor Settings##EditorSettingsModal", &m_isOpen, flags);

    if (popupOpen) {
        if (!m_isOpen) {
            ImGui::CloseCurrentPopup();
        } else {
            drawSettings();
        }
        ImGui::EndPopup();
    }
}

void EditorSettingsWindow::drawSettings() {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float footerY = ImGui::GetWindowHeight() - style.WindowPadding.y - ImGui::GetFrameHeight();
    float tabRegionHeight = std::max(1.0f, footerY - style.ItemSpacing.y - ImGui::GetCursorPosY());

    ImGui::BeginChild(
        "##EditorSettingsTabRegion",
        ImVec2(0, tabRegionHeight),
        ImGuiChildFlags_None,
        noScrollFlags
    );

    if (ImGui::BeginTabBar("##EditorSettingsTabs", ImGuiTabBarFlags_FittingPolicyShrink)) {
        // A requested tab only shows from the next frame. Until then the last one
        // is still drawn, and the toolchain tabs would probe for nothing.
        const bool switchingTab = m_requestedTab.has_value();

        if (ImGui::BeginTabItem("General", nullptr, tabFlags(Tab::General))) {
            drawGeneralSettings();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Desktop", nullptr, tabFlags(Tab::Desktop))) {
            if (!switchingTab) drawCMakeSettings();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Shortcuts", nullptr, tabFlags(Tab::Shortcuts))) {
            drawShortcutSettings();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Graphics", nullptr, tabFlags(Tab::Graphics))) {
            drawGraphicsSettings();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Engine API", nullptr, tabFlags(Tab::Api))) {
            drawApiSettings();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Web", nullptr, tabFlags(Tab::Web))) {
            if (!switchingTab) drawWebSettings();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("AI", nullptr, tabFlags(Tab::AI))) {
            drawAiSettings();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Advanced", nullptr, tabFlags(Tab::Advanced))) {
            drawAdvancedSettings();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
        m_requestedTab.reset();
    }

    ImGui::EndChild();

    ImGui::SetCursorPos(ImVec2(style.WindowPadding.x, footerY - style.ItemSpacing.y));
    ImGui::Separator();

    float footerWidth = std::max(1.0f, ImGui::GetWindowWidth() - style.WindowPadding.x * 2.0f);
    float buttonWidth = std::clamp((footerWidth - style.ItemSpacing.x) * 0.5f, 1.0f, Theme::dpi(settingsButtonWidth));
    float buttonsWidth = buttonWidth * 2.0f + style.ItemSpacing.x;
    float buttonX = style.WindowPadding.x + std::max(0.0f, (footerWidth - buttonsWidth) * 0.5f);
    ImGui::SetCursorPos(ImVec2(buttonX, footerY));

    if (ImGui::Button("OK", ImVec2(buttonWidth, 0))) {
        if (applySettings()) {
            m_isOpen = false;
            ImGui::CloseCurrentPopup();
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0))) {
        m_isOpen = false;
        ImGui::CloseCurrentPopup();
    }
}

void EditorSettingsWindow::drawGeneralSettings() {
    drawSettingsPanel("##EditorGeneralSettingsPanel", [this]() {
        if (beginSettingsRow("Default Export Directory",
                             "Used by Export Project when the current project has no saved export directory.",
                             !m_defaultExportDirectory.empty())) {
            m_defaultExportDirectory.clear();
        }
        fs::path exportDisplay = m_defaultExportDirectory.empty()
            ? fs::path("<Not set>")
            : m_defaultExportDirectory;
        drawPathPicker(
            "##EditorDefaultExportDirectory", "Browse##editor_default_export_dir", "Clear##editor_default_export_dir",
            exportDisplay, !m_defaultExportDirectory.empty(), true,
            [this](const std::string& selectedPath) {
                m_defaultExportDirectory = selectedPath;
            },
            [this]() {
                m_defaultExportDirectory.clear();
            }
        );

        if (beginSettingsRow("Editor VSync",
                             "VSync for the editor UI. Play mode and exported builds use the project VSync setting.",
                             m_editorVSyncEnabled != true)) {
            m_editorVSyncEnabled = true;
        }
        ImGui::Checkbox("##EditorVSync", &m_editorVSyncEnabled);

        if (beginSettingsRow("UI Scale",
                             "Size of the editor text and panels, on top of the display scale the system reports.",
                             m_uiScalePercent != 100)) {
            m_uiScalePercent = 100;
        }
        static constexpr int uiScalePercents[] = { 50, 75, 90, 100, 110, 125, 150, 175, 200, 250, 300 };
        ImGui::SetNextItemWidth(Theme::dpi(100.0f));
        if (ImGui::BeginCombo("##EditorUiScale", (std::to_string(m_uiScalePercent) + "%").c_str(), ImGuiComboFlags_HeightLarge)) {
            for (int percent : uiScalePercents) {
                const bool selected = percent == m_uiScalePercent;
                if (ImGui::Selectable((std::to_string(percent) + "%").c_str(), selected)) {
                    m_uiScalePercent = percent;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        const bool extensionChanged = std::string(m_projectFileExtension.data()) != ProjectFile::DEFAULT_EXTENSION;
        if (beginSettingsRow("Project File Extension",
                             "Extension of the marker file written next to project.yaml. Double-clicking that file\n"
                             "opens the project once the file type is registered in the Project menu.",
                             extensionChanged)) {
            setBuffer(m_projectFileExtension.data(), m_projectFileExtension.size(),
                      ProjectFile::DEFAULT_EXTENSION);
        }
        ImGui::SetNextItemWidth(Theme::dpi(120.0f));
        ImGui::InputText("##ProjectFileExtension", m_projectFileExtension.data(), m_projectFileExtension.size());
    });
}

void EditorSettingsWindow::drawCMakeSettings() {
    if (!m_kitsDetected) {
        detectKits();
    }
    drawSettingsPanel("##EditorCMakeSettingsPanel", [this]() {
        beginSettingsRow("CMake", "Path to cmake executable. Empty means auto-detect from PATH.", !m_cmakeOverride.empty());
        fs::path cmakeDisplay = m_cmakeOverride.empty()
            ? fs::path(m_cmakeInfo.found ? m_cmakeInfo.path : "<Not found on PATH>")
            : fs::path(m_cmakeOverride);
        drawPathPicker(
            "##EditorCMakePath", "Browse##editor_cmake", "Auto##editor_cmake",
            cmakeDisplay, !m_cmakeOverride.empty(), false,
            [this](const std::string& selectedPath) {
                const std::string resolved = Generator::resolveCMakePath(selectedPath);
                const std::string version = resolved.empty() ? std::string() : Generator::probeCMakeVersion(resolved);

                if (version.empty()) {
                    m_cmakePickError = resolved.empty()
                        ? "No CMake executable in: " + selectedPath
                        : "Not a working CMake: " + resolved;
                } else {
                    m_cmakeOverride = resolved;
                    m_cmakePickError.clear();
                }
                refreshCMakeStatus();
            },
            [this]() {
                m_cmakeOverride.clear();
                m_cmakePickError.clear();
                refreshCMakeStatus();
            }
        );

        if (m_cmakeInfo.found) {
            ImGui::TextDisabled("Detected: %s", m_cmakeInfo.version.empty() ? m_cmakeInfo.path.c_str() : m_cmakeInfo.version.c_str());
        }
        if (!m_cmakePickError.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.0f, 1.0f));
            ImGui::TextWrapped("%s", m_cmakePickError.c_str());
            ImGui::PopStyleColor();
        }

        if (beginSettingsRow("Compiler", "Compiler kit used to build C++ scripts, both when playing a scene and when exporting. "
                "Set the language standard in Project Settings > Build > C++ Standard.", m_cmakeKitIndex != 0)) {
            m_cmakeKitIndex = 0;
        }
        if (m_cmakeKitIndex < 0 || m_cmakeKitIndex > static_cast<int>(m_availableKits.size())) {
            m_cmakeKitIndex = 0;
        }

        const char* currentLabel = m_cmakeKitIndex == 0
            ? "Default"
            : m_availableKits[m_cmakeKitIndex - 1].displayName.c_str();

        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##EditorCMakeKit", currentLabel)) {
            bool selected = m_cmakeKitIndex == 0;
            if (ImGui::Selectable("Default", selected)) m_cmakeKitIndex = 0;
            if (selected) ImGui::SetItemDefaultFocus();

            for (size_t i = 0; i < m_availableKits.size(); i++) {
                const auto& kit = m_availableKits[i];
                if (!kit.available) {
                    ImGui::BeginDisabled();
                    ImGui::Selectable((kit.displayName + "  (unavailable)").c_str(), false);
                    ImGui::EndDisabled();
                    showDisabledItemTooltip(kit.unavailableReason);
                    continue;
                }

                selected = m_cmakeKitIndex == static_cast<int>(i + 1);
                if (ImGui::Selectable(kit.displayName.c_str(), selected)) {
                    m_cmakeKitIndex = static_cast<int>(i + 1);
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        if (m_cmakeKitIndex > 0) {
            const auto& kit = m_availableKits[m_cmakeKitIndex - 1];
            if (!kit.cCompiler.empty() || !kit.cxxCompiler.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped("C: %s\nCXX: %s", kit.cCompiler.c_str(), kit.cxxCompiler.c_str());
                ImGui::PopStyleColor();
            }
        } else {
            // The combo fell back to "Default" because the stored kit was not detected
            const LocalBuildSettings stored = projectBuildSettings();
            if (!stored.cxxCompiler.empty() || !stored.generator.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.0f, 1.0f));
                ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION " Builds still use \"%s\", which is no longer detected. Apply to switch to the default toolchain.",
                    stored.cxxCompiler.empty() ? stored.generator.c_str() : stored.cxxCompiler.c_str());
                ImGui::PopStyleColor();
            }
        }

    });
}

void EditorSettingsWindow::drawWebSettings() {
    if (!m_emsdkDetected) {
        refreshEmsdkStatus();
        m_emsdkDetected = true;
    }
    drawSettingsPanel("##EditorWebSettingsPanel", [this]() {
        beginSettingsRow("Emscripten SDK", "Path to emsdk root. Empty means EMSDK/PATH auto-detect.", !m_emsdkOverride.empty());

        fs::path emsdkDisplay = m_emsdkOverride.empty()
            ? fs::path("<Auto-detect>")
            : fs::path(m_emsdkOverride);
        drawPathPicker(
            "##EditorEmsdkPath", "Browse##editor_emsdk", "Auto##editor_emsdk",
            emsdkDisplay, !m_emsdkOverride.empty(), true,
            [this](const std::string& selectedPath) {
                m_emsdkOverride = selectedPath;
                refreshEmsdkStatus();
            },
            [this]() {
                m_emsdkOverride.clear();
                refreshEmsdkStatus();
            }
        );

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Status");
        ImGui::TableNextColumn();
        if (m_emsdkInfo.found) {
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), ICON_FA_CIRCLE_CHECK " Found %s", m_emsdkInfo.description.c_str());
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION " Not found");
        }
    });
}

void EditorSettingsWindow::drawAiSettings() {
    if (beginSettingsPanel("##EditorAiSettingsPanel")) {
        if (m_expandAiChat) {
            ImGui::SetScrollY(0.0f);
            ImGui::SetNextItemOpen(true);
            m_expandAiChat = false;
        }
        if (ImGui::CollapsingHeader("AI Chat")) {
            drawAiChatSettings();
        }
        if (ImGui::CollapsingHeader("MCP Server", ImGuiTreeNodeFlags_DefaultOpen)) {
            drawMcpSettings();
        }
    }
    endSettingsPanel();
}

void EditorSettingsWindow::drawAiChatSettings() {
    drawSectionNote("Add a key for any provider the AI chat should use. Keys are stored obfuscated in "
                    "your user config folder, and the chat's model picker lists the configured providers.");

    if (beginSettingsTable("##AiProviderKeys")) {
        for (const ai::ProviderAccount& account : ai::listAccounts(m_aiSettings)) {
            if (account.provider == ai::ProviderId::OpenAICompatible) continue;
            drawAiKeyRow(account.id, account.label);
        }
        ImGui::EndTable();
    }

    drawSectionTitle("Custom endpoints");
    ImGui::SetItemTooltip("OpenAI-compatible Chat Completions URLs. Each keeps its own key and "
                          "model list, so several can be configured at the same time.");

    // Skipped when empty: a zero-row table still costs its padding.
    if (!m_aiSettings.customEndpoints.empty() && beginSettingsTable("##AiEndpoints")) {
        drawAiEndpointRows();
        ImGui::EndTable();
    }
    drawAddEndpointButton();

    drawSectionTitle("Limits");

    if (beginSettingsTable("##AiLimits")) {
        const ai::Settings defaults;

        if (beginSettingsRow("Request Timeout (s)",
                             "Maximum time to wait for each AI model response. Raise this for slow local models.",
                             m_aiSettings.requestTimeoutSeconds != defaults.requestTimeoutSeconds)) {
            m_aiSettings.requestTimeoutSeconds = defaults.requestTimeoutSeconds;
        }
        ImGui::SetNextItemWidth(-1);
        ImGui::InputInt("##AiRequestTimeout", &m_aiSettings.requestTimeoutSeconds, 30, 60);
        m_aiSettings.requestTimeoutSeconds = std::clamp(m_aiSettings.requestTimeoutSeconds, 1, 3600);

        if (beginSettingsRow("Max Output Tokens", nullptr,
                             m_aiSettings.maxOutputTokens != defaults.maxOutputTokens)) {
            m_aiSettings.maxOutputTokens = defaults.maxOutputTokens;
        }
        ImGui::SetNextItemWidth(-1);
        ImGui::InputInt("##AiMaxOutput", &m_aiSettings.maxOutputTokens, 256, 1024);
        m_aiSettings.maxOutputTokens = std::clamp(m_aiSettings.maxOutputTokens, 256, 16000);

        if (beginSettingsRow("Max Tool Steps",
                             "How many model turns that request tools are allowed per user message. "
                             "Raise this if you see \"Reached the tool-step limit\" on long tasks.",
                             m_aiSettings.maxToolRounds != defaults.maxToolRounds)) {
            m_aiSettings.maxToolRounds = defaults.maxToolRounds;
        }
        ImGui::SetNextItemWidth(-1);
        ImGui::InputInt("##AiMaxToolRounds", &m_aiSettings.maxToolRounds, 1, 4);
        m_aiSettings.maxToolRounds = std::clamp(m_aiSettings.maxToolRounds, 1, 100);

        ImGui::EndTable();
    }
}

void EditorSettingsWindow::drawAiKeyRow(const std::string& accountId, const std::string& label, bool dimLabel) {
    KeyEntry& entry = m_aiKeys[accountId];
    float clearWidth = ImGui::GetFrameHeight();
    float spacing = ImGui::GetStyle().ItemSpacing.x;

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    if (dimLabel) {
        ImGui::TextDisabled("%s", label.c_str());
    } else {
        ImGui::TextUnformatted(label.c_str());
    }
    if (entry.configured) {
        ImGui::SameLine();
        ImGui::TextColored(keySetColor, ICON_FA_CIRCLE_CHECK);
        ImGui::SetItemTooltip("Key configured");
    }

    ImGui::TableNextColumn();
    ImGui::PushID(accountId.c_str());
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - clearWidth - spacing);
    ImGui::InputTextWithHint("##Key", entry.configured ? "key set - type to replace" : "paste API key",
                             entry.buffer.data(), entry.buffer.size(),
                             ImGuiInputTextFlags_Password);
    if (ImGui::BeginPopupContextItem("##KeyContext")) {
        if (ImGui::MenuItem(ICON_FA_CLIPBOARD " Paste")) {
            const char* clipboard = ImGui::GetClipboardText();
            if (clipboard) {
                setBuffer(entry.buffer.data(), entry.buffer.size(), clipboard);
            }
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!entry.configured && entry.buffer[0] == '\0');
    if (Widgets::iconButton("##ClearKey", ICON_FA_TRASH, ImVec2(clearWidth, ImGui::GetFrameHeight()))) {
        ai::SecretStore::clearApiKey(accountId);
        entry.buffer.fill('\0');
        refreshAiKeyState();
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Clear this key");
    ImGui::PopID();
}

void EditorSettingsWindow::drawAiEndpointRows() {
    std::string pendingRemoval;
    for (const ai::CustomEndpoint& endpoint : m_aiSettings.customEndpoints) {
        auto it = m_aiEndpoints.find(endpoint.id);
        if (it == m_aiEndpoints.end()) continue;
        EndpointEntry& fields = it->second;

        float clearWidth = ImGui::GetFrameHeight();
        float spacing = ImGui::GetStyle().ItemSpacing.x;

        ImGui::PushID(endpoint.id.c_str());
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        // A plain label until the pencil turns it into a field.
        if (m_editingEndpointId == endpoint.id) {
            ImGui::SetNextItemWidth(-1);
            if (m_focusEndpointLabel) {
                ImGui::SetKeyboardFocusHere();
                m_focusEndpointLabel = false;
            }
            if (ImGui::InputText("##Label", fields.label.data(), fields.label.size(),
                                 ImGuiInputTextFlags_EnterReturnsTrue) ||
                ImGui::IsItemDeactivated()) {
                m_editingEndpointId.clear();
            }
        } else {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(fields.label[0] != '\0' ? fields.label.data() : endpoint.id.c_str());
            if (inlineIconButton(ICON_FA_PENCIL, "EditLabel", "Rename this endpoint")) {
                m_editingEndpointId = endpoint.id;
                m_focusEndpointLabel = true;
            }
        }

        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - clearWidth - spacing);
        ImGui::InputTextWithHint("##Url", "https://host/v1/chat/completions",
                                 fields.url.data(), fields.url.size());
        ImGui::SameLine();
        if (Widgets::iconButton("##RemoveEndpoint", ICON_FA_XMARK,
                                ImVec2(clearWidth, ImGui::GetFrameHeight()))) {
            pendingRemoval = endpoint.id;
        }
        ImGui::SetItemTooltip("Remove this endpoint and its key");
        ImGui::PopID();

        drawAiKeyRow(ai::accountKey(ai::ProviderId::OpenAICompatible, endpoint.id), "API key", true);
    }

    // Deferred so the endpoint list is not mutated mid-iteration.
    if (!pendingRemoval.empty()) {
        removeAiEndpoint(pendingRemoval);
    }
}

// Outside the tables, centered under the section title.
void EditorSettingsWindow::drawAddEndpointButton() {
    const float width = Theme::dpi(160.0f);
    ImGui::SetCursorPosX(std::max(0.0f, (ImGui::GetWindowSize().x - width) * 0.5f));
    if (ImGui::Button(ICON_FA_PLUS "  Add endpoint", ImVec2(width, 0))) {
        ImGui::OpenPopup("##AddEndpoint");
    }
    if (ImGui::BeginPopup("##AddEndpoint")) {
        // The presets below only prefill a URL that could be typed here instead.
        if (ImGui::MenuItem("OpenAI compatible")) {
            addAiEndpoint("OpenAI compatible", "");
        }
        ImGui::Separator();
        for (const ai::EndpointPreset& preset : ai::endpointPresets()) {
            if (ImGui::MenuItem(preset.label.c_str())) {
                addAiEndpoint(preset.label, preset.url);
            }
        }
        ImGui::EndPopup();
    }
}

void EditorSettingsWindow::drawMcpSettings() {
    // Stored at once, so a copied command still works after Cancel
    if (m_mcpToken.empty()) {
        m_mcpToken = ai::McpServer::newToken();
    }
    const std::string url = ai::McpServer::endpointUrl(m_mcpSettings.port);

    drawSectionNote("Lets AI agents outside the editor, such as Claude Code, Codex, Gemini CLI or OpenCode, use the "
                    "tools of the AI chat on the open project. The agent brings its own model.");

    if (beginSettingsTable("##McpSettingsTable")) {
        const ai::McpSettings defaults;
        const ai::McpServer* server = Backend::getApp().getMcpServer();

        beginSettingsRow("Enable Server", "Listen for MCP clients on this computer only (127.0.0.1).");
        ImGui::Checkbox("##McpEnabled", &m_mcpSettings.enabled);

        beginSettingsRow("Status");
        const std::string error = server->getError();
        if (server->isRunning()) {
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), ICON_FA_CIRCLE_CHECK " Running");
        } else if (!error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.0f, 1.0f));
            ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION " %s", error.c_str());
            ImGui::PopStyleColor();
        } else {
            ImGui::TextDisabled("Stopped");
        }

        if (beginSettingsRow("Port", nullptr, m_mcpSettings.port != defaults.port)) {
            m_mcpSettings.port = defaults.port;
        }
        ImGui::SetNextItemWidth(-1);
        ImGui::InputInt("##McpPort", &m_mcpSettings.port, 0, 0);
        m_mcpSettings.port = std::clamp(m_mcpSettings.port, 1024, 65535);

        if (beginSettingsRow("Allow Changes", "Off: agents only get the read-only tools, which inspect and search the project.",
                             m_mcpSettings.allowChanges != defaults.allowChanges)) {
            m_mcpSettings.allowChanges = defaults.allowChanges;
        }
        ImGui::Checkbox("##McpAllowChanges", &m_mcpSettings.allowChanges);

        beginSettingsRow("Token", "Agents send it as \"Authorization: Bearer <token>\". "
                                  "Anyone who has it can change your project, so keep it private.");
        if (ImGui::Button("Copy##McpToken")) {
            ImGui::SetClipboardText(m_mcpToken.c_str());
        }
        ImGui::SameLine();
        if (ImGui::Button("Replace##McpToken")) {
            m_mcpToken = ai::McpServer::newToken();
        }
        ImGui::SetItemTooltip("Connected agents need the new token from the next request on.");

        beginSettingsRow("URL", "The MCP endpoint, for clients set up by hand. They also need the token header.");
        drawCopyField("##McpUrl", url, url, "Copy the URL");

        ImGui::EndTable();
    }

    drawSectionTitle("Clients");
    ImGui::SetItemTooltip("Run one of these in a terminal to add the editor to that agent.");

    if (beginSettingsTable("##McpClientsTable")) {
        const std::string masked = "********";

        const std::string claude = "claude mcp add --transport http quantum " + url + " --header \"Authorization: Bearer ";
        beginSettingsRow("Claude Code");
        drawCopyField("##McpClaudeCommand", claude + masked + "\"", claude + m_mcpToken + "\"",
                      "Copy the command, token included");

        const std::string codex = "codex mcp add quantum --url " + url + " --bearer-token-env-var QUANTUM_MCP_TOKEN";
        beginSettingsRow("Codex", "Codex reads the token from QUANTUM_MCP_TOKEN when it connects, "
                                  "so set that variable to the token where Codex runs.");
        drawCopyField("##McpCodexCommand", codex, codex, "Copy the command");

        const std::string gemini = "gemini mcp add -s user --transport http --header \"Authorization: Bearer ";
        beginSettingsRow("Gemini CLI");
        drawCopyField("##McpGeminiCommand", gemini + masked + "\" quantum " + url, gemini + m_mcpToken + "\" quantum " + url,
                      "Copy the command, token included");

        const std::string opencode = "opencode mcp add quantum --url " + url + " --header \"Authorization=Bearer ";
        beginSettingsRow("OpenCode");
        drawCopyField("##McpOpencodeCommand", opencode + masked + "\"", opencode + m_mcpToken + "\"",
                      "Copy the command, token included");

        ImGui::EndTable();
    }
}

void EditorSettingsWindow::drawAdvancedSettings() {
    drawSettingsPanel("##EditorAdvancedSettingsPanel", [this]() {
        beginSettingsRow("Shader Cache", "Shared by projects for this editor version. Shaders are rebuilt when needed.");
        ImGui::BeginDisabled(m_project && m_project->isAnyScenePlaying());
        if (ImGui::Button("Clear Shader Cache")) {
            std::error_code ec;
            std::filesystem::remove_all(App::getUserShaderCacheDir(), ec);
            m_cacheStatus = ec ? "Could not clear the shader cache: " + ec.message() : "Shader cache cleared.";
        }
        ImGui::EndDisabled();
        if (!m_cacheStatus.empty()) {
            ImGui::TextWrapped("%s", m_cacheStatus.c_str());
        }
    });
}

bool EditorSettingsWindow::applySettings() {
    AppSettings::setEditorVSyncEnabled(m_editorVSyncEnabled);
    AppSettings::setUiScale(m_uiScalePercent / 100.0f);
    AppSettings::setViewportRenderDistance(m_viewportRenderDistance);
    AppSettings::setDefaultExportDirectory(m_defaultExportDirectory);
    AppSettings::setProjectFileExtension(m_projectFileExtension.data());
    AppSettings::setCMakePath(m_cmakeOverride);
    // Unless the Desktop tab ran detection, the stored kit stays
    if (m_kitsDetected) {
        LocalBuildSettings build;
        if (m_cmakeKitIndex > 0) {
            const auto& kit = m_availableKits[m_cmakeKitIndex - 1];
            build.cCompiler = kit.cCompiler;
            build.cxxCompiler = kit.cxxCompiler;
            build.generator = kit.generator;
        }
        if (m_project) {
            const auto file = m_project->getProjectPath() / "project.yaml";
            build.buildJobs = AppSettings::getBuildSettings(file).buildJobs;
            AppSettings::setBuildSettings(file, build);
        }
        // Also the editor-wide default, so a new project starts from it
        AppSettings::setLastCMakeKit(build.cCompiler, build.cxxCompiler, build.generator);
    }
    AppSettings::setEmsdkPath(m_emsdkOverride);
    const bool graphicsSaved = applyGraphicsSettings();
    applyAiSettings();
    AppSettings::setMcpSettings(m_mcpSettings);
    Backend::getApp().getMcpServer()->applySettings(m_mcpSettings);
    if (!AppSettings::saveSettings()) return false;
    return graphicsSaved;
}

void EditorSettingsWindow::applyAiSettings() {
    // The id never changes, so a rename keeps the endpoint's stored key.
    for (ai::CustomEndpoint& endpoint : m_aiSettings.customEndpoints) {
        auto it = m_aiEndpoints.find(endpoint.id);
        if (it == m_aiEndpoints.end()) continue;
        endpoint.label = it->second.label.data();
        endpoint.url = it->second.url.data();
    }

    // The model, the endpoint in use and the approval mode belong to the chat
    ai::Settings settings = AppSettings::getAiSettings();
    settings.customEndpoints = m_aiSettings.customEndpoints;
    if (!ai::findEndpoint(settings, settings.endpointId)) {
        settings.endpointId.clear();
    }
    settings.requestTimeoutSeconds = m_aiSettings.requestTimeoutSeconds;
    settings.maxOutputTokens = m_aiSettings.maxOutputTokens;
    settings.maxToolRounds = m_aiSettings.maxToolRounds;
    AppSettings::setAiSettings(settings);

    // Deleted endpoints lose their key, unless the same id was re-added before OK.
    for (const std::string& endpointId : m_removedEndpoints) {
        if (!ai::findEndpoint(settings, endpointId)) {
            ai::SecretStore::clearApiKey(ai::accountKey(ai::ProviderId::OpenAICompatible, endpointId));
        }
    }
    m_removedEndpoints.clear();

    // Persist any keys the user typed (one per account). Keys are stored
    // obfuscated in the user config dir, never alongside the project.
    for (auto& entry : m_aiKeys) {
        if (entry.second.buffer[0] != '\0') {
            ai::SecretStore::setApiKey(entry.first, entry.second.buffer.data());
            entry.second.buffer.fill('\0');
        }
    }
    refreshAiKeyState();

    Backend::getApp().getAiChatWindow()->reloadSettings();
}

// A chord is captured from the keyboard, not typed: the point of the tab is that a key
// on the real keyboard becomes the binding, so what shows in the row is what was pressed.
// Mouse and gamepad buttons are left out because the click that opens the capture would
// be read back as its own chord.
void EditorSettingsWindow::handleShortcutCapture() {
    if (m_capturedId.empty()) return;

    const ImGuiIO& io = ImGui::GetIO();
    m_captureMods.clear();
    if (io.KeyCtrl) m_captureMods += "Ctrl+";
    if (io.KeyShift) m_captureMods += "Shift+";
    if (io.KeyAlt) m_captureMods += "Alt+";
    if (io.KeySuper) m_captureMods += "Super+";

    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
        const ImGuiKey key = static_cast<ImGuiKey>(k);
        if (key == ImGuiKey_LeftCtrl || key == ImGuiKey_RightCtrl ||
            key == ImGuiKey_LeftShift || key == ImGuiKey_RightShift ||
            key == ImGuiKey_LeftAlt || key == ImGuiKey_RightAlt ||
            key == ImGuiKey_LeftSuper || key == ImGuiKey_RightSuper) continue;
        if ((key >= ImGuiKey_GamepadStart && key <= ImGuiKey_GamepadRStickDown) ||
            (key >= ImGuiKey_MouseLeft && key <= ImGuiKey_MouseWheelY)) continue;
        if (!ImGui::IsKeyPressed(key, false)) continue;

        const std::string id = m_capturedId;
        m_capturedId.clear();

        if (key == ImGuiKey_Escape && io.KeyMods == ImGuiMod_None) {
            m_shortcutError.clear();
            return;  // cancelled, nothing changed
        }
        if ((key == ImGuiKey_Delete || key == ImGuiKey_Backspace) && io.KeyMods == ImGuiMod_None) {
            std::string ignored;
            KeyBindings::bind(id, "", ignored);
            m_shortcutError.clear();
            return;
        }

        // The chord is stored as the text the same function formats, so the row, the
        // menu, the palette and settings.yaml all carry one spelling.
        const std::string chord = KeyBindings::chordText(
            static_cast<ImGuiKeyChord>(key) | static_cast<ImGuiKeyChord>(io.KeyMods & ImGuiMod_Mask_));
        std::string error;
        if (!KeyBindings::bind(id, chord, error)) {
            m_shortcutError = "Could not bind " + chord + ": " + error;
            return;
        }
        m_shortcutError.clear();
        return;
    }
}

void EditorSettingsWindow::applyShortcutClipboard() {
    const char* raw = ImGui::GetClipboardText();
    if (!raw || raw[0] == '\0') {
        m_shortcutError = "The clipboard has no text.";
        return;
    }

    std::vector<std::string> skipped;
    const int applied = cli::applyBindingLines(std::string(raw), skipped);

    if (skipped.empty()) {
        m_shortcutError.clear();
        m_shortcutNote = applied > 0
                             ? "Applied " + std::to_string(applied) + " binding(s) from the clipboard."
                             : "The clipboard had no \"id: chord\" lines.";
    } else {
        std::string problems = "Applied " + std::to_string(applied) + " binding(s), " +
                               std::to_string(skipped.size()) + " skipped:";
        for (const std::string& problem : skipped) problems += "\n  " + problem;
        m_shortcutError = problems;
        m_shortcutNote.clear();
    }
}

void EditorSettingsWindow::drawShortcutRow(const char* id) {
    const KeyBinding* binding = KeyBindings::find(id);
    if (!binding) return;

    const bool capturing = m_capturedId == id;
    const bool overridden = KeyBindings::isOverridden(id);
    const std::string chords = KeyBindings::text(id);
    const auto conflicts = KeyBindings::conflicts();
    bool clashing = false;
    for (const auto& conflict : conflicts) {
        if (conflict.first == id || conflict.second == id) clashing = true;
    }

    ImGui::TableNextRow();

    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    if (clashing) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::WarningText);
        ImGui::TextUnformatted(binding->label);
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Another command answers this same chord, so one of the two "
                              "will not fire. The other is named in the warning above the list.");
    } else {
        ImGui::TextUnformatted(binding->label);
    }

    ImGui::TableNextColumn();
    if (capturing) {
        const std::string hint = m_captureMods + "Press a key...  Esc cancels";
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_CheckMark]);
        ImGui::TextUnformatted(hint.c_str());
        ImGui::PopStyleColor();
    } else if (chords.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::TextUnformatted("(unbound)");
        ImGui::PopStyleColor();
    } else {
        ImGui::TextUnformatted(chords.c_str());
        ImGui::SetItemTooltip("%s\nid: %s", binding->label, binding->id);
    }

    ImGui::TableNextColumn();
    if (overridden) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::TextUnformatted(binding->chords);
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Default. The row above is what fires now.");
    }

    ImGui::TableNextColumn();
    const float buttonSize = ImGui::GetFrameHeight();
    if (Widgets::iconButton((std::string("Record") + id).c_str(),
                            capturing ? ICON_FA_XMARK : ICON_FA_KEYBOARD,
                            ImVec2(buttonSize, buttonSize))) {
        m_capturedId = capturing ? std::string() : id;
        m_shortcutError.clear();
    }
    ImGui::SetItemTooltip(capturing ? "Stop capturing"
                                    : "Click, then press the shortcut. Del on a row clears it.");
    if (overridden) {
        ImGui::SameLine(0.0f, 2.0f);
        if (Widgets::iconButton((std::string("Reset") + id).c_str(), ICON_FA_ROTATE_LEFT,
                                ImVec2(buttonSize, buttonSize))) {
            KeyBindings::reset(id);
            m_shortcutError.clear();
        }
        ImGui::SetItemTooltip("Restore the default chord");
    }
}

void EditorSettingsWindow::drawShortcutSettings() {
    // One capture step per frame, before the list, so a key pressed while the row is
    // scrolled out of view still lands on the row that asked for it.
    handleShortcutCapture();

    // beginSettingsPanel + a table of its own: the shared two-column row table is for
    // label/value settings, and this list needs four columns.
    const bool panel = beginSettingsPanel("##EditorShortcutsPanel");
    if (panel) {
        ImGui::PushItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - 210.0f));
        ImGui::InputTextWithHint("##ShortcutSearch", "Search by command, id or chord",
                                 m_shortcutSearch.data(), m_shortcutSearch.size());
        ImGui::PopItemWidth();
        if (m_shortcutSearch[0] != '\0') {
            ImGui::SameLine();
            if (inlineIconButton(ICON_FA_XMARK, "ClearShortcutSearch", "Clear the filter")) {
                m_shortcutSearch[0] = '\0';
            }
        }
        ImGui::SameLine();
        if (inlineIconButton(ICON_FA_ROTATE_LEFT, "ResetAllShortcuts",
                             "Restore every shortcut to its default chord")) {
            KeyBindings::resetAll();
            m_capturedId.clear();
            m_shortcutError.clear();
        }

        ImGui::SameLine();
        if (inlineIconButton(ICON_FA_COPY, "CopyShortcuts",
                             "Copy the overrides as settings.yaml lines, on one line each:\n"
                             "  shortcuts:\n    file.save: Ctrl+S")) {
            const std::string text = cli::bindingOverridesYaml();
            ImGui::SetClipboardText(text.c_str());
        }
        ImGui::SameLine();
        if (inlineIconButton(ICON_FA_PASTE, "PasteShortcuts",
                             "Apply every \"id: chord\" line on the clipboard, so a saved\n"
                             "binding list can be moved between machines. Unknown ids and\n"
                             "unreadable chords are named instead of silently dropped.")) {
            applyShortcutClipboard();
        }

        const auto conflicts = KeyBindings::conflicts();
        if (!conflicts.empty()) {
            std::string warning;
            for (const auto& conflict : conflicts) {
                if (!warning.empty()) warning += ", ";
                warning += conflict.first + " = " + conflict.second;
            }
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::WarningText);
            ImGui::TextWrapped("%d chord(s) shared by two commands: %s",
                               static_cast<int>(conflicts.size()), warning.c_str());
            ImGui::PopStyleColor();
        }
        if (!m_shortcutError.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::DangerText);
            ImGui::TextWrapped("%s", m_shortcutError.c_str());
            ImGui::PopStyleColor();
        }
        if (!m_shortcutNote.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            ImGui::TextWrapped("%s", m_shortcutNote.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::Spacing();
        // Everything applies as it is clicked - the menu labels, the palette rows and the
        // input pass all read the same registry - so Cancel cannot undo a rebind.
        drawSectionNote("Applied immediately; OK and Cancel do not undo a rebind. Use Reset "
                        "all (the round arrow above) to return to the defaults.");

        ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("##ShortcutTable", 4, flags)) {
            ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthStretch, 0.34f);
            ImGui::TableSetupColumn("Shortcut", ImGuiTableColumnFlags_WidthStretch, 0.30f);
            ImGui::TableSetupColumn("Default", ImGuiTableColumnFlags_WidthStretch, 0.20f);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableHeadersRow();

            const std::string needle = lowerCopy(m_shortcutSearch.data());
            const char* category = nullptr;
            int shown = 0;
            for (const KeyBinding& binding : KeyBindings::all()) {
                const std::string chords = KeyBindings::text(binding.id);
                const bool matches = needle.empty() ||
                                     lowerCopy(binding.label).find(needle) != std::string::npos ||
                                     std::string(binding.id).find(needle) != std::string::npos ||
                                     lowerCopy(binding.category).find(needle) != std::string::npos ||
                                     lowerCopy(chords).find(needle) != std::string::npos;
                if (!matches) continue;

                if (!category || std::strcmp(category, binding.category) != 0) {
                    category = binding.category;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                    ImGui::TextUnformatted(category);
                    ImGui::PopStyleColor();
                    for (int column = 1; column < 4; ++column) ImGui::TableNextColumn();
                    ++shown;
                }
                drawShortcutRow(binding.id);
                ++shown;
            }
            ImGui::EndTable();
            ImGui::TextDisabled("%d of %d commands", shown, static_cast<int>(KeyBindings::all().size()));
        }
        endSettingsPanel();
    } else {
        ImGui::PopStyleVar(3);
    }
}

// ---------------------------------------------------------------------------
// Graphics tab
// ---------------------------------------------------------------------------

namespace {

    // Order matters: each list is indexed by the enum's ordinal, which is also what
    // project.yaml stores as a lowercase name through Stream::encodeProject.
    const char* scalingModeNames[] = { "Fit width", "Fit height", "Letterbox", "Crop",
                                       "Stretch", "Native" };
    const char* textureStrategyNames[] = { "Fit", "Resize to power of two", "None" };
    const char* windowModeNames[] = { "Windowed", "Maximized", "Fullscreen" };
    const char* shadowQualityNames[] = { "None", "Low", "Medium", "High" };
    const char* graphicsPresetNames[] = { "Low", "Medium", "High", "Ultra" };
    const char* textureFilterNames[] = { "Nearest", "Linear" };
    // must stay in the order tonemap.frag reads from tonemap.params.y
    const char* tonemapCurveNames[] = { "ACES", "Reinhard", "Reinhard Extended", "Uncharted 2", "AgX" };

    struct SizePreset {
        const char* name;
        int width;
        int height;
    };

    // 4K is here because the canvas is what an exported build renders at; the desktop
    // window can be set to the same size, and the presets fill both.
    const SizePreset sizePresets[] = {
        { "1280x720", 1280, 720 }, { "1920x1080", 1920, 1080 },
        { "2560x1440", 2560, 1440 }, { "3840x2160 4K", 3840, 2160 },
    };

    int parseSize(const char* text, int fallback) {
        if (!text || text[0] == '\0') return fallback;
        const long value = std::strtol(text, nullptr, 10);
        // 16384 is the engine's texture cap, so anything above it is a typo.
        if (value <= 0 || value > 16384) return fallback;
        return static_cast<int>(value);
    }

    // One row that reads a scene property through Catalog and writes it back with the same
    // undo command the Inspector uses, so a change made here is undoable with Ctrl+Z and
    // marks the scene modified exactly like a change made there.
    template <typename T, typename Draw>
    void scenePropertyRow(const char* label, const char* help, Project* project,
                          uint32_t sceneId, Scene* scene, const char* propertyName, Draw draw) {
        if (!project || !scene) return;
        beginSettingsRow(label, help, false);
        T value = Catalog::getSceneProperty<T>(scene, propertyName);
        if (draw(value)) {
            CommandHandle::get(sceneId)->addCommand(
                new ScenePropertyCmd<T>(project, sceneId, propertyName, value));
        }
    }

    // Three properties at once, so one Ctrl+Z undoes a whole preset rather than leaving the
    // scene rendering at the new width with the old height.
    void applyResolutionPreset(Project* project, uint32_t sceneId, int width, int height) {
        if (!project) return;
        auto preset = std::make_unique<MultiPropertyCmd>();
        preset->addCommand(std::make_unique<ScenePropertyCmd<int>>(
            project, sceneId, "fixed_resolution_width", width));
        preset->addCommand(std::make_unique<ScenePropertyCmd<int>>(
            project, sceneId, "fixed_resolution_height", height));
        preset->addCommand(std::make_unique<ScenePropertyCmd<bool>>(
            project, sceneId, "fixed_resolution_enabled", true));
        CommandHandle::get(sceneId)->addCommand(preset.release());
    }

    // Keep the preset deliberately focused on quality and cost. Artistic controls such as
    // exposure, DOF, grading and render distance belong to the scene author and must not be
    // silently changed when a user chooses a hardware-oriented quality level.
    void applyGraphicsPreset(Project* project, uint32_t sceneId, int presetIndex) {
        if (!project) return;
        const int preset = std::clamp(presetIndex, 0, 3);
        const ShadowQuality shadows = preset == 0 ? ShadowQuality::LOW
            : preset == 1 ? ShadowQuality::MEDIUM : ShadowQuality::HIGH;
        const bool mediumOrBetter = preset >= 1;
        const bool highOrBetter = preset >= 2;
        auto command = std::make_unique<MultiPropertyCmd>();

        command->addCommand(std::make_unique<ScenePropertyCmd<ShadowQuality>>(
            project, sceneId, "shadows_quality", shadows));
        command->addCommand(std::make_unique<ScenePropertyCmd<ShadowQuality>>(
            project, sceneId, "shadows_2d_quality", shadows));
        command->addCommand(std::make_unique<ScenePropertyCmd<bool>>(
            project, sceneId, "depth_prepass_enabled", mediumOrBetter));
        command->addCommand(std::make_unique<ScenePropertyCmd<bool>>(
            project, sceneId, "mesh_lod_enabled", true));

        command->addCommand(std::make_unique<ScenePropertyCmd<bool>>(
            project, sceneId, "ssao_enabled", mediumOrBetter));
        command->addCommand(std::make_unique<ScenePropertyCmd<float>>(
            project, sceneId, "ssao_radius", preset == 0 ? 0.35f : preset == 1 ? 0.50f : preset == 2 ? 0.65f : 0.80f));
        command->addCommand(std::make_unique<ScenePropertyCmd<float>>(
            project, sceneId, "ssao_intensity", preset == 0 ? 0.80f : preset == 1 ? 1.00f : preset == 2 ? 1.15f : 1.30f));
        command->addCommand(std::make_unique<ScenePropertyCmd<float>>(
            project, sceneId, "ssao_bias", preset == 0 ? 0.035f : preset == 1 ? 0.025f : preset == 2 ? 0.020f : 0.015f));
        command->addCommand(std::make_unique<ScenePropertyCmd<bool>>(
            project, sceneId, "contact_shadows_enabled", mediumOrBetter));
        command->addCommand(std::make_unique<ScenePropertyCmd<float>>(
            project, sceneId, "contact_shadow_length", preset == 0 ? 0.25f : preset == 1 ? 0.35f : preset == 2 ? 0.50f : 0.70f));
        command->addCommand(std::make_unique<ScenePropertyCmd<int>>(
            project, sceneId, "contact_shadow_steps", preset == 0 ? 8 : preset == 1 ? 12 : preset == 2 ? 20 : 28));
        command->addCommand(std::make_unique<ScenePropertyCmd<float>>(
            project, sceneId, "contact_shadow_thickness", preset == 0 ? 0.04f : preset == 1 ? 0.025f : 0.018f));
        command->addCommand(std::make_unique<ScenePropertyCmd<float>>(
            project, sceneId, "contact_shadow_intensity", preset == 0 ? 0.45f : preset == 1 ? 0.60f : preset == 2 ? 0.70f : 0.80f));
        command->addCommand(std::make_unique<ScenePropertyCmd<bool>>(
            project, sceneId, "ssgi_enabled", highOrBetter));
        command->addCommand(std::make_unique<ScenePropertyCmd<float>>(
            project, sceneId, "ssgi_radius", preset == 0 ? 1.5f : preset == 1 ? 2.5f : preset == 2 ? 3.5f : 5.0f));
        command->addCommand(std::make_unique<ScenePropertyCmd<float>>(
            project, sceneId, "ssgi_intensity", preset == 0 ? 0.20f : preset == 1 ? 0.30f : preset == 2 ? 0.40f : 0.50f));
        command->addCommand(std::make_unique<ScenePropertyCmd<int>>(
            project, sceneId, "ssgi_steps", preset == 0 ? 8 : preset == 1 ? 12 : preset == 2 ? 20 : 28));
        command->addCommand(std::make_unique<ScenePropertyCmd<float>>(
            project, sceneId, "ssgi_thickness", preset == 0 ? 0.35f : preset == 1 ? 0.25f : preset == 2 ? 0.18f : 0.12f));

        command->addCommand(std::make_unique<ScenePropertyCmd<bool>>(
            project, sceneId, "ssr_enabled", highOrBetter));
        command->addCommand(std::make_unique<ScenePropertyCmd<float>>(
            project, sceneId, "ssr_max_distance", preset == 0 ? 6.0f : preset == 1 ? 10.0f : preset == 2 ? 18.0f : 28.0f));
        command->addCommand(std::make_unique<ScenePropertyCmd<int>>(
            project, sceneId, "ssr_max_steps", preset == 0 ? 24 : preset == 1 ? 40 : preset == 2 ? 64 : 96));
        command->addCommand(std::make_unique<ScenePropertyCmd<float>>(
            project, sceneId, "ssr_intensity", preset == 0 ? 0.75f : preset == 1 ? 0.90f : preset == 2 ? 1.00f : 1.10f));
        command->addCommand(std::make_unique<ScenePropertyCmd<float>>(
            project, sceneId, "ssr_blur", preset == 0 ? 0.20f : preset == 1 ? 0.12f : preset == 2 ? 0.06f : 0.02f));

        command->addCommand(std::make_unique<ScenePropertyCmd<bool>>(
            project, sceneId, "taa_enabled", mediumOrBetter));
        command->addCommand(std::make_unique<ScenePropertyCmd<float>>(
            project, sceneId, "taa_blend", preset == 0 ? 0.85f : preset == 1 ? 0.90f : preset == 2 ? 0.92f : 0.94f));

        command->addCommand(std::make_unique<ScenePropertyCmd<bool>>(
            project, sceneId, "bloom_enabled", mediumOrBetter));
        command->addCommand(std::make_unique<ScenePropertyCmd<float>>(
            project, sceneId, "bloom_threshold", preset == 0 ? 0.95f : preset == 1 ? 0.85f : preset == 2 ? 0.75f : 0.65f));
        command->addCommand(std::make_unique<ScenePropertyCmd<float>>(
            project, sceneId, "bloom_intensity", preset == 0 ? 0.50f : preset == 1 ? 0.75f : preset == 2 ? 1.00f : 1.15f));
        command->addCommand(std::make_unique<ScenePropertyCmd<float>>(
            project, sceneId, "bloom_soft_knee", 0.50f));
        command->addCommand(std::make_unique<ScenePropertyCmd<int>>(
            project, sceneId, "bloom_mip_levels", preset == 0 ? 2 : preset == 1 ? 3 : preset == 2 ? 4 : 5));

        command->addCommand(std::make_unique<ScenePropertyCmd<bool>>(
            project, sceneId, "tonemap_enabled", true));
        command->addCommand(std::make_unique<ScenePropertyCmd<int>>(
            project, sceneId, "tonemap_mode", highOrBetter ? 4 : 0));
        command->addCommand(std::make_unique<ScenePropertyCmd<bool>>(
            project, sceneId, "fxaa_enabled", !highOrBetter));
        command->addCommand(std::make_unique<ScenePropertyCmd<float>>(
            project, sceneId, "fxaa_intensity", 0.85f));

        CommandHandle::get(sceneId)->addCommand(command.release());
    }

    // A heading inside the two-column row table. SeparatorText cannot be used there: the
    // table owns the row, and a separator between cells lands in the wrong column.
    void tableSectionHeader(const char* title) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();
    }

}

void EditorSettingsWindow::syncGraphicsBuffers() {
    m_graphicsLoaded = true;
    m_graphicsError.clear();
    m_viewportRenderDistance = AppSettings::getViewportRenderDistance();
    if (!m_project) return;

    setBuffer(m_canvasWidth.data(), m_canvasWidth.size(), std::to_string(m_project->getCanvasWidth()));
    setBuffer(m_canvasHeight.data(), m_canvasHeight.size(), std::to_string(m_project->getCanvasHeight()));
    setBuffer(m_windowWidth.data(), m_windowWidth.size(), std::to_string(m_project->getWindowWidth()));
    setBuffer(m_windowHeight.data(), m_windowHeight.size(), std::to_string(m_project->getWindowHeight()));
    m_scalingMode = static_cast<int>(m_project->getScalingMode());
    m_textureStrategy = static_cast<int>(m_project->getTextureStrategy());
    m_windowMode = static_cast<int>(m_project->getWindowMode());
    m_projectVSync = m_project->isVSyncEnabled();

    std::vector<SceneProject>& scenes = m_project->getScenes();
    if (scenes.empty()) {
        m_graphicsScene = 0;
        return;
    }
    // Open on the scene the user has selected: the quality block edits one scene, and the
    // wrong default would silently tune the wrong file.
    const uint32_t selected = m_project->getSelectedSceneId();
    m_graphicsScene = 0;
    for (size_t index = 0; index < scenes.size(); ++index) {
        if (scenes[index].id != selected) continue;
        m_graphicsScene = static_cast<int>(index);
        break;
    }
}

void EditorSettingsWindow::drawProjectOutputRows() {
    if (!m_project) return;
    if (!beginSettingsTable("##ProjectGraphicsTable")) return;

    const bool canvasChanged =
        parseSize(m_canvasWidth.data(), -1) != static_cast<int>(m_project->getCanvasWidth()) ||
        parseSize(m_canvasHeight.data(), -1) != static_cast<int>(m_project->getCanvasHeight());
    if (beginSettingsRow("Canvas Size",
                         "The resolution the game renders at and the size an exported window is\n"
                         "created with. Applied on Play and to generated builds; the editor\n"
                         "viewport keeps its own size.", canvasChanged)) {
        setBuffer(m_canvasWidth.data(), m_canvasWidth.size(), std::to_string(m_project->getCanvasWidth()));
        setBuffer(m_canvasHeight.data(), m_canvasHeight.size(), std::to_string(m_project->getCanvasHeight()));
    }
    ImGui::SetNextItemWidth(Theme::dpi(86.0f));
    ImGui::InputText("##CanvasWidth", m_canvasWidth.data(), m_canvasWidth.size(),
                     ImGuiInputTextFlags_CharsDecimal);
    ImGui::SameLine();
    ImGui::TextUnformatted("x");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(86.0f));
    ImGui::InputText("##CanvasHeight", m_canvasHeight.data(), m_canvasHeight.size(),
                     ImGuiInputTextFlags_CharsDecimal);
    const int canvasW = parseSize(m_canvasWidth.data(), 0);
    const int canvasH = parseSize(m_canvasHeight.data(), 0);
    if (canvasW > 0 && canvasH > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("%d MP", (canvasW * canvasH) / 1000000);
    }

    ImGui::SameLine();
    for (size_t index = 0; index < std::size(sizePresets); ++index) {
        if (index > 0) ImGui::SameLine();
        if (ImGui::SmallButton(sizePresets[index].name)) {
            setBuffer(m_canvasWidth.data(), m_canvasWidth.size(), std::to_string(sizePresets[index].width));
            setBuffer(m_canvasHeight.data(), m_canvasHeight.size(), std::to_string(sizePresets[index].height));
        }
    }

    const bool windowChanged =
        parseSize(m_windowWidth.data(), -1) != static_cast<int>(m_project->getWindowWidth()) ||
        parseSize(m_windowHeight.data(), -1) != static_cast<int>(m_project->getWindowHeight());
    if (beginSettingsRow("Window Size", "Initial desktop window size. Ignored by Web and mobile.",
                         windowChanged)) {
        setBuffer(m_windowWidth.data(), m_windowWidth.size(), std::to_string(m_project->getWindowWidth()));
        setBuffer(m_windowHeight.data(), m_windowHeight.size(), std::to_string(m_project->getWindowHeight()));
    }
    ImGui::SetNextItemWidth(Theme::dpi(86.0f));
    ImGui::InputText("##WindowWidth", m_windowWidth.data(), m_windowWidth.size(),
                     ImGuiInputTextFlags_CharsDecimal);
    ImGui::SameLine();
    ImGui::TextUnformatted("x");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(86.0f));
    ImGui::InputText("##WindowHeight", m_windowHeight.data(), m_windowHeight.size(),
                     ImGuiInputTextFlags_CharsDecimal);

    if (beginSettingsRow("Window Mode",
                         "How the desktop window opens. Fullscreen uses the canvas size.",
                         m_windowMode != static_cast<int>(m_project->getWindowMode()))) {
        m_windowMode = static_cast<int>(m_project->getWindowMode());
    }
    ImGui::SetNextItemWidth(Theme::dpi(160.0f));
    ImGui::Combo("##WindowMode", &m_windowMode, windowModeNames,
                 static_cast<int>(std::size(windowModeNames)));

    if (beginSettingsRow("Scaling Mode",
                         "How the canvas fits the real window when the player resizes it.\n"
                         "Letterbox keeps the whole canvas with bars; Crop fills the screen\n"
                         "and cuts what does not fit; Native renders at the window size.",
                         m_scalingMode != static_cast<int>(m_project->getScalingMode()))) {
        m_scalingMode = static_cast<int>(m_project->getScalingMode());
    }
    ImGui::SetNextItemWidth(Theme::dpi(160.0f));
    ImGui::Combo("##ScalingMode", &m_scalingMode, scalingModeNames,
                 static_cast<int>(std::size(scalingModeNames)));

    if (beginSettingsRow("Texture Strategy",
                         "What happens to a texture whose size is not a power of two.\n"
                         "Resize re-samples it on import, Fit keeps it as it is.",
                         m_textureStrategy != static_cast<int>(m_project->getTextureStrategy()))) {
        m_textureStrategy = static_cast<int>(m_project->getTextureStrategy());
    }
    ImGui::SetNextItemWidth(Theme::dpi(180.0f));
    ImGui::Combo("##TextureStrategy", &m_textureStrategy, textureStrategyNames,
                 static_cast<int>(std::size(textureStrategyNames)));

    if (beginSettingsRow("Project VSync",
                         "Vertical sync during Play and in exported builds. The editor UI has\n"
                         "its own switch on the General tab.",
                         m_projectVSync != m_project->isVSyncEnabled())) {
        m_projectVSync = m_project->isVSyncEnabled();
    }
    ImGui::Checkbox("##ProjectVSync", &m_projectVSync);

    ImGui::EndTable();
}

void EditorSettingsWindow::drawSceneQualityRows() {
    if (!m_project) return;
    std::vector<SceneProject>& scenes = m_project->getScenes();
    if (scenes.empty()) {
        ImGui::TextDisabled("The project has no scene yet, so there is nothing to tune.");
        return;
    }
    if (m_graphicsScene < 0 || m_graphicsScene >= static_cast<int>(scenes.size())) m_graphicsScene = 0;

    ImGui::SetNextItemWidth(Theme::dpi(240.0f));
    if (ImGui::BeginCombo("##QualityScene", scenes[m_graphicsScene].name.c_str(),
                          ImGuiComboFlags_HeightLarge)) {
        for (size_t index = 0; index < scenes.size(); ++index) {
            const bool selected = static_cast<int>(index) == m_graphicsScene;
            if (ImGui::Selectable(scenes[index].name.c_str(), selected)) {
                m_graphicsScene = static_cast<int>(index);
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("quality of this scene");

    ImGui::Spacing();
    ImGui::SetNextItemWidth(Theme::dpi(160.0f));
    ImGui::Combo("Graphics preset", &m_graphicsPreset, graphicsPresetNames,
                 static_cast<int>(std::size(graphicsPresetNames)));
    ImGui::SameLine();
    if (ImGui::Button("Apply preset")) {
        applyGraphicsPreset(m_project, scenes[m_graphicsScene].id, m_graphicsPreset);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("quality/performance only; artistic settings are preserved");

    SceneProject& sceneProject = scenes[m_graphicsScene];
    Scene* scene = sceneProject.scene;
    if (!scene) {
        ImGui::TextDisabled("The scene is not loaded yet. Open it once and come back.");
        return;
    }
    const uint32_t sceneId = sceneProject.id;

    if (!beginSettingsTable("##SceneQualityTable")) return;

    tableSectionHeader("Lighting");
    scenePropertyRow<Vector4>("Background Color", "The clear colour of the scene.", m_project,
                              sceneId, scene, "background_color", [](Vector4& value) {
                                  float rgba[4] = { value.x, value.y, value.z, value.w };
                                  if (!ImGui::ColorEdit4("##background_color", rgba)) return false;
                                  value.x = rgba[0]; value.y = rgba[1]; value.z = rgba[2];
                                  value.w = rgba[3];
                                  return true;
                              });
    scenePropertyRow<ShadowQuality>("Shadow Quality",
                                    "Shadow map filtering. High costs the most fill rate; None\n"
                                    "still draws lights, without shadows.",
                                    m_project, sceneId, scene, "shadows_quality", [](ShadowQuality& value) {
                                        int item = static_cast<int>(value);
                                        if (!ImGui::Combo("##shadows_quality", &item, shadowQualityNames,
                                                          static_cast<int>(std::size(shadowQualityNames)))) {
                                            return false;
                                        }
                                        value = static_cast<ShadowQuality>(item);
                                        return true;
                                    });
    scenePropertyRow<float>("Global Illumination Intensity",
                            "Flat ambient term added to every surface.", m_project, sceneId, scene,
                            "global_illumination_intensity", [](float& value) {
                                return ImGui::DragFloat("##global_illumination_intensity", &value, 0.01f,
                                                         0.0f, 8.0f, "%.2f");
                            });
    scenePropertyRow<Vector3>("Global Illumination Color", "Tint of the ambient term.", m_project,
                              sceneId, scene, "global_illumination_color", [](Vector3& value) {
                                  float rgb[3] = { value.x, value.y, value.z };
                                  if (!ImGui::ColorEdit3("##global_illumination_color", rgb)) return false;
                                  value.x = rgb[0]; value.y = rgb[1]; value.z = rgb[2];
                                  return true;
                              });
    scenePropertyRow<ShadowQuality>("2D Shadow Quality",
                                    "Penumbra taps on 2D lights: more taps, softer edges.",
                                    m_project, sceneId, scene, "shadows_2d_quality", [](ShadowQuality& value) {
                                        int item = static_cast<int>(value);
                                        if (!ImGui::Combo("##shadows_2d_quality", &item, shadowQualityNames,
                                                          static_cast<int>(std::size(shadowQualityNames)))) {
                                            return false;
                                        }
                                        value = static_cast<ShadowQuality>(item);
                                        return true;
                                    });
    scenePropertyRow<float>("2D Ambient Intensity", "Ambient light for the 2D pass.", m_project,
                            sceneId, scene, "ambient_light_2d_intensity", [](float& value) {
                                return ImGui::DragFloat("##ambient_light_2d_intensity", &value, 0.01f,
                                                         0.0f, 8.0f, "%.2f");
                            });

    tableSectionHeader("Geometry");
    scenePropertyRow<bool>("Depth Pre-pass",
                           "Draws depth before colour. Faster with heavy overdraw, slower on\n"
                           "small scenes. Reloads the meshes when it changes.",
                           m_project, sceneId, scene, "depth_prepass_enabled", [](bool& value) {
                               bool on = value;
                               if (!ImGui::Checkbox("##depth_prepass_enabled", &on)) return false;
                               value = on;
                               return true;
                           });
    scenePropertyRow<bool>("Mesh LOD", "Swap meshes by distance.", m_project, sceneId, scene,
                           "mesh_lod_enabled", [](bool& value) {
                               bool on = value;
                               if (!ImGui::Checkbox("##mesh_lod_enabled", &on)) return false;
                               value = on;
                               return true;
                           });
    scenePropertyRow<float>("Mesh LOD Threshold",
                            "How much larger the next level has to be before it is used.",
                            m_project, sceneId, scene, "mesh_lod_threshold", [](float& value) {
                                return ImGui::DragFloat("##mesh_lod_threshold", &value, 0.01f, 0.0f, 0.0f,
                                                         "%.3f");
                            });
    scenePropertyRow<float>("Render Distance",
                            "How far the scene is drawn, in metres. 0 removes the limit. It caps\n"
                            "the camera far plane, so geometry past it is culled instead of shaded,\n"
                            "shadows are packed into the range that shows, and terrain and foliage\n"
                            "stop generating further out. Perspective cameras only.",
                            m_project, sceneId, scene, "render_distance", [](float& value) {
                                return ImGui::DragFloat("##render_distance", &value, 1.0f, 0.0f, 100000.0f,
                                                         "%.0f m");
                            });
    beginSettingsRow("Distance presets",
                     "Sets the scene render distance above. Nothing is culled by the distance\n"
                     "you can still see through, so pick the shortest that keeps the horizon.");
    for (const float metres : { 60.0f, 150.0f, 400.0f, 0.0f }) {
        char preset[16];
        if (metres > 0.0f)
            std::snprintf(preset, sizeof(preset), "%.0f m", metres);
        else
            std::snprintf(preset, sizeof(preset), "Unlimited");
        ImGui::SameLine();
        if (ImGui::SmallButton(preset)) {
            CommandHandle::get(sceneId)->addCommand(
                new ScenePropertyCmd<float>(m_project, sceneId, "render_distance", metres));
        }
    }

    tableSectionHeader("Screen space");
    scenePropertyRow<bool>("SSAO",
                           "Ambient occlusion. Turning it on rebuilds the mesh shaders, which\n"
                           "takes a moment in a big scene.",
                           m_project, sceneId, scene, "ssao_enabled", [](bool& value) {
                               bool on = value;
                               if (!ImGui::Checkbox("##ssao_enabled", &on)) return false;
                               value = on;
                               return true;
                           });
    scenePropertyRow<float>("SSAO Radius", "How wide a occluded area reaches.", m_project, sceneId,
                            scene, "ssao_radius", [](float& value) {
                                return ImGui::DragFloat("##ssao_radius", &value, 0.01f, 0.0f, 16.0f, "%.2f");
                            });
    scenePropertyRow<float>("SSAO Intensity", "Darkening applied where it occludes.", m_project,
                            sceneId, scene, "ssao_intensity", [](float& value) {
                                return ImGui::DragFloat("##ssao_intensity", &value, 0.01f, 0.0f, 8.0f, "%.2f");
                            });
    scenePropertyRow<float>("SSAO Bias",
                            "Lifts the test off the surface. Too low stripes flat walls; too\n"
                            "high removes the effect.",
                            m_project, sceneId, scene, "ssao_bias", [](float& value) {
                                return ImGui::DragFloat("##ssao_bias", &value, 0.001f, 0.0f, 1.0f, "%.3f");
                            });
    scenePropertyRow<bool>("Contact Shadows",
                           "Short screen-space shadows under direct light. Uses the existing depth\n"
                           "pre-pass and is especially useful where the shadow atlas is too coarse.",
                           m_project, sceneId, scene, "contact_shadows_enabled", [](bool& value) {
                               bool on = value;
                               if (!ImGui::Checkbox("##contact_shadows_enabled", &on)) return false;
                               value = on;
                               return true;
                           });
    scenePropertyRow<float>("Contact Shadow Length", "Maximum light-space march distance in world units.",
                            m_project, sceneId, scene, "contact_shadow_length", [](float& value) {
                                return ImGui::DragFloat("##contact_shadow_length", &value, 0.01f, 0.0f, 8.0f, "%.2f");
                            });
    scenePropertyRow<int>("Contact Shadow Steps", "Samples marched toward the strongest directional light.",
                          m_project, sceneId, scene, "contact_shadow_steps", [](int& value) {
                              return ImGui::DragInt("##contact_shadow_steps", &value, 1.0f, 4, 64);
                          });
    scenePropertyRow<float>("Contact Shadow Thickness", "Depth tolerance that prevents surface self-shadowing.",
                            m_project, sceneId, scene, "contact_shadow_thickness", [](float& value) {
                                return ImGui::DragFloat("##contact_shadow_thickness", &value, 0.001f, 0.0f, 1.0f, "%.3f");
                            });
    scenePropertyRow<float>("Contact Shadow Intensity", "How strongly the screen-space occlusion darkens direct light.",
                            m_project, sceneId, scene, "contact_shadow_intensity", [](float& value) {
                                return ImGui::DragFloat("##contact_shadow_intensity", &value, 0.01f, 0.0f, 1.0f, "%.2f");
                            });
    scenePropertyRow<bool>("SSGI",
                           "Screen-space diffuse bounce lighting. It uses the scene color and depth,\n"
                           "so it is strongest in creases and near-field surfaces.",
                           m_project, sceneId, scene, "ssgi_enabled", [](bool& value) {
                               bool on = value;
                               if (!ImGui::Checkbox("##ssgi_enabled", &on)) return false;
                               value = on;
                               return true;
                           });
    scenePropertyRow<float>("SSGI Radius", "Maximum screen-space bounce distance in world units.",
                            m_project, sceneId, scene, "ssgi_radius", [](float& value) {
                                return ImGui::DragFloat("##ssgi_radius", &value, 0.05f, 0.1f, 32.0f, "%.2f");
                            });
    scenePropertyRow<float>("SSGI Intensity", "Strength of the diffuse bounce added to scene color.",
                            m_project, sceneId, scene, "ssgi_intensity", [](float& value) {
                                return ImGui::DragFloat("##ssgi_intensity", &value, 0.01f, 0.0f, 4.0f, "%.2f");
                            });
    scenePropertyRow<int>("SSGI Steps", "Screen-space ray-march samples per bounce direction.",
                          m_project, sceneId, scene, "ssgi_steps", [](int& value) {
                              return ImGui::DragInt("##ssgi_steps", &value, 1.0f, 4, 32);
                          });
    scenePropertyRow<float>("SSGI Thickness", "Depth tolerance used to reject false screen-space hits.",
                            m_project, sceneId, scene, "ssgi_thickness", [](float& value) {
                                return ImGui::DragFloat("##ssgi_thickness", &value, 0.01f, 0.0f, 4.0f, "%.2f");
                            });
    scenePropertyRow<bool>("DOF",
                           "Depth of field. Blurs what is not at the focus distance using the\n"
                           "scene depth; it rebuilds no mesh shaders.",
                           m_project, sceneId, scene, "dof_enabled", [](bool& value) {
                               bool on = value;
                               if (!ImGui::Checkbox("##dof_enabled", &on)) return false;
                               value = on;
                               return true;
                           });
    scenePropertyRow<float>("DOF Focus Distance",
                            "How far from the camera the image stays sharp, in world units.",
                            m_project, sceneId, scene, "dof_focus_distance", [](float& value) {
                                return ImGui::DragFloat("##dof_focus_distance", &value, 0.5f, 0.0f, 10000.0f,
                                                         "%.1f");
                            });
    scenePropertyRow<float>("DOF Aperture",
                            "How strongly out-of-focus areas blur. 0 keeps everything sharp.",
                            m_project, sceneId, scene, "dof_aperture", [](float& value) {
                                return ImGui::DragFloat("##dof_aperture", &value, 0.1f, 0.0f, 16.0f, "%.2f");
                            });
    scenePropertyRow<float>("DOF Focus Range",
                            "Depth band around the focus distance that stays fully sharp.",
                            m_project, sceneId, scene, "dof_focus_range", [](float& value) {
                                return ImGui::DragFloat("##dof_focus_range", &value, 0.1f, 0.0f, 10000.0f,
                                                         "%.2f");
                            });
    scenePropertyRow<float>("DOF Max Blur",
                            "Ceiling of the blur radius, as a fraction of the screen height.",
                            m_project, sceneId, scene, "dof_max_blur", [](float& value) {
                                return ImGui::DragFloat("##dof_max_blur", &value, 0.001f, 0.0f, 0.25f, "%.3f");
                            });
    scenePropertyRow<bool>("SSR",
                           "Screen-space reflections. Also rebuilds the mesh shaders.",
                           m_project, sceneId, scene, "ssr_enabled", [](bool& value) {
                               bool on = value;
                               if (!ImGui::Checkbox("##ssr_enabled", &on)) return false;
                               value = on;
                               return true;
                           });
    scenePropertyRow<float>("SSR Max Distance", "How far a reflection may reach in world units.",
                            m_project, sceneId, scene, "ssr_max_distance", [](float& value) {
                                return ImGui::DragFloat("##ssr_max_distance", &value, 0.5f, 0.0f, 5000.0f,
                                                         "%.1f");
                            });
    scenePropertyRow<int>("SSR Max Steps",
                          "Rays marched per pixel. The main cost of the pass.", m_project, sceneId,
                          scene, "ssr_max_steps", [](int& value) {
                              return ImGui::DragInt("##ssr_max_steps", &value, 1.0f, 1, 512);
                          });
    scenePropertyRow<float>("SSR Intensity", "How much of the reflection is kept.", m_project,
                            sceneId, scene, "ssr_intensity", [](float& value) {
                                return ImGui::DragFloat("##ssr_intensity", &value, 0.01f, 0.0f, 4.0f, "%.2f");
                            });
    scenePropertyRow<float>("SSR Blur", "Softening of the reflected sample.", m_project, sceneId,
                            scene, "ssr_blur", [](float& value) {
                                return ImGui::DragFloat("##ssr_blur", &value, 0.01f, 0.0f, 4.0f, "%.2f");
                            });
    scenePropertyRow<bool>("TAA",
                           "Temporal anti-aliasing with depth reprojection and a ping-ponged history buffer.\n"
                           "When Fixed Resolution is enabled, it becomes temporal upscaling (TAAU).",
                           m_project, sceneId, scene, "taa_enabled", [](bool& value) {
                               bool on = value;
                               if (!ImGui::Checkbox("##taa_enabled", &on)) return false;
                               value = on;
                               return true;
                           });
    scenePropertyRow<float>("TAA History Blend", "History weight at zero motion.", m_project, sceneId,
                            scene, "taa_blend", [](float& value) {
                                return ImGui::DragFloat("##taa_blend", &value, 0.01f, 0.0f, 0.99f, "%.2f");
                            });
    scenePropertyRow<bool>("Motion Blur", "Directional blur driven by the camera motion-vector buffer.",
                           m_project, sceneId, scene, "motion_blur_enabled", [](bool& value) {
                               bool on = value;
                               if (!ImGui::Checkbox("##motion_blur_enabled", &on)) return false;
                               value = on;
                               return true;
                           });
    scenePropertyRow<float>("Motion Blur Intensity", "Multiplier for the reprojection vector.", m_project,
                            sceneId, scene, "motion_blur_intensity", [](float& value) {
                                return ImGui::DragFloat("##motion_blur_intensity", &value, 0.01f, 0.0f, 8.0f, "%.2f");
                            });
    scenePropertyRow<int>("Motion Blur Samples", "Maximum samples marched along the velocity vector.",
                          m_project, sceneId, scene, "motion_blur_samples", [](int& value) {
                              return ImGui::DragInt("##motion_blur_samples", &value, 1.0f, 1, 32);
                          });

    tableSectionHeader("High dynamic range");
    scenePropertyRow<bool>("Bloom",
                           "Light bleeding. Extracts the bright pixels, blurs them across\n"
                           "the mip levels and adds the glow back before the tone map.",
                           m_project, sceneId, scene, "bloom_enabled", [](bool& value) {
                               bool on = value;
                               if (!ImGui::Checkbox("##bloom_enabled", &on)) return false;
                               value = on;
                               return true;
                           });
    scenePropertyRow<float>("Bloom Threshold",
                            "Luminance a pixel must pass to glow. The scene is 0..1, so\n"
                            "below 1.0 is what makes lamps and the sky bloom.",
                            m_project, sceneId, scene, "bloom_threshold", [](float& value) {
                                return ImGui::DragFloat("##bloom_threshold", &value, 0.01f, 0.0f, 4.0f, "%.2f");
                            });
    scenePropertyRow<float>("Bloom Soft Knee",
                            "How gradually a light starts to bloom. 0 pops as the camera moves.",
                            m_project, sceneId, scene, "bloom_soft_knee", [](float& value) {
                                return ImGui::DragFloat("##bloom_soft_knee", &value, 0.01f, 0.0f, 1.0f, "%.2f");
                            });
    scenePropertyRow<float>("Bloom Intensity", "How much of the glow is added back.", m_project,
                            sceneId, scene, "bloom_intensity", [](float& value) {
                                return ImGui::DragFloat("##bloom_intensity", &value, 0.01f, 0.0f, 8.0f, "%.2f");
                            });
    scenePropertyRow<float>("Lens Dirt",
                            "How much of the glow is carried by smudges fixed to the screen\n"
                            "instead of spreading evenly. The mask is procedural: the engine\n"
                            "ships no dirt texture.",
                            m_project, sceneId, scene, "bloom_dirt", [](float& value) {
                                return ImGui::DragFloat("##bloom_dirt", &value, 0.01f, 0.0f, 1.0f, "%.2f");
                            });
    scenePropertyRow<int>("Bloom Levels",
                          "Mip levels of the blur chain. More levels reach wider; each one\n"
                          "halves the target.",
                          m_project, sceneId, scene, "bloom_mip_levels", [](int& value) {
                              return ImGui::DragInt("##bloom_mip_levels", &value, 1.0f, 1, MAX_BLOOM_LEVELS);
                          });
    scenePropertyRow<bool>("Tone Map",
                           "Filmic curve from the HDR target to the display. Without it the\n"
                           "bright areas clip to flat white.",
                           m_project, sceneId, scene, "tonemap_enabled", [](bool& value) {
                               bool on = value;
                               if (!ImGui::Checkbox("##tonemap_enabled", &on)) return false;
                               value = on;
                               return true;
                           });
    scenePropertyRow<int>("Tone Map Curve",
                          "Response of the highlight roll-off. AgX keeps saturated lights\n"
                          "closest to film; ACES is the usual default.",
                          m_project, sceneId, scene, "tonemap_mode", [](int& value) {
                              if (!ImGui::Combo("##tonemap_mode", &value, tonemapCurveNames,
                                                static_cast<int>(std::size(tonemapCurveNames)))) {
                                  return false;
                              }
                              return true;
                          });
    scenePropertyRow<float>("Exposure",
                            "Multiplies the HDR color before the curve. 1.0 is the authored\n"
                            "brightness; it becomes the starting point when auto exposure is on.",
                            m_project, sceneId, scene, "exposure", [](float& value) {
                                return ImGui::DragFloat("##exposure", &value, 0.01f, 0.01f, 16.0f, "%.2f");
                            });
    scenePropertyRow<bool>("Auto Exposure",
                           "Eye adaptation. A 1x1 meter reads the frame luminance each pass and\n"
                           "drives the exposure, so a door from a dark room into daylight blows\n"
                           "out for a moment and settles. Needs the tone map.",
                           m_project, sceneId, scene, "auto_exposure", [](bool& value) {
                               bool on = value;
                               if (!ImGui::Checkbox("##auto_exposure", &on)) return false;
                               value = on;
                               return true;
                           });
    scenePropertyRow<float>("Auto Exposure Speed",
                            "How fast the eye adapts, in stops per second. Slow is cinematic,\n"
                            "fast reads as a broken camera.",
                            m_project, sceneId, scene, "auto_exposure_speed", [](float& value) {
                                return ImGui::DragFloat("##auto_exposure_speed", &value, 0.05f, 0.05f, 10.0f,
                                                        "%.2f");
                            });
    scenePropertyRow<float>("Auto Exposure Min EV",
                            "Darkest the meter may make the image.", m_project, sceneId, scene,
                            "auto_exposure_min_ev", [](float& value) {
                                return ImGui::DragFloat("##auto_exposure_min_ev", &value, 0.1f, -10.0f, 10.0f,
                                                        "%.1f");
                            });
    scenePropertyRow<float>("Auto Exposure Max EV",
                            "Brightest the meter may make the image.", m_project, sceneId, scene,
                            "auto_exposure_max_ev", [](float& value) {
                                return ImGui::DragFloat("##auto_exposure_max_ev", &value, 0.1f, -10.0f, 10.0f,
                                                        "%.1f");
                            });
    scenePropertyRow<float>("Exposure Compensation",
                            "Offsets the target grey. Positive is a brighter world: snow and\n"
                            "sand read as grey to the meter and want +0.5 to +1.0.",
                            m_project, sceneId, scene, "exposure_compensation", [](float& value) {
                                return ImGui::DragFloat("##exposure_compensation", &value, 0.05f, -5.0f, 5.0f,
                                                        "%.2f");
                            });

    tableSectionHeader("Color grading");
    scenePropertyRow<bool>("Color Grading",
                           "Applied by the tone map, after the curve and before the encode, so\n"
                           "it behaves like a print. Needs bloom or the tone map enabled.",
                           m_project, sceneId, scene, "color_grading_enabled", [](bool& value) {
                               bool on = value;
                               if (!ImGui::Checkbox("##color_grading_enabled", &on)) return false;
                               value = on;
                               return true;
                           });
    scenePropertyRow<float>("Saturation",
                            "0 is monochrome, 1 authored, above 1 pushes towards clipping.",
                            m_project, sceneId, scene, "color_grading_saturation", [](float& value) {
                                return ImGui::DragFloat("##color_grading_saturation", &value, 0.01f, 0.0f, 4.0f,
                                                        "%.2f");
                            });
    scenePropertyRow<float>("Contrast",
                            "Pivots around mid grey. 1 keeps the curve's own response.",
                            m_project, sceneId, scene, "color_grading_contrast", [](float& value) {
                                return ImGui::DragFloat("##color_grading_contrast", &value, 0.01f, 0.0f, 4.0f,
                                                        "%.2f");
                            });
    scenePropertyRow<float>("Brightness",
                            "Exposure-style offset in stops after the curve, so it lifts the\n"
                            "shadows too. Use it for night or a faded look.",
                            m_project, sceneId, scene, "color_grading_brightness", [](float& value) {
                                return ImGui::DragFloat("##color_grading_brightness", &value, 0.01f, -1.0f, 1.0f,
                                                        "%.2f");
                            });
    scenePropertyRow<float>("Hue Shift",
                            "Rotates every hue around the colour wheel, in degrees. A little is\n"
                            "a grade; a lot is a bug.", m_project, sceneId, scene,
                            "color_grading_hue_shift", [](float& value) {
                                return ImGui::DragFloat("##color_grading_hue_shift", &value, 1.0f, -180.0f,
                                                        180.0f, "%.1f");
                            });
    scenePropertyRow<float>("Temperature",
                            "Towards amber (positive) or cyan (negative) along the warm axis.",
                            m_project, sceneId, scene, "color_grading_temperature", [](float& value) {
                                return ImGui::DragFloat("##color_grading_temperature", &value, 0.01f, -1.0f, 1.0f,
                                                        "%.2f");
                            });
    scenePropertyRow<float>("Tint",
                            "Towards green (positive) or magenta (negative). Pairs with\n"
                            "temperature the way a gel does.", m_project, sceneId, scene,
                            "color_grading_tint", [](float& value) {
                                return ImGui::DragFloat("##color_grading_tint", &value, 0.01f, -1.0f, 1.0f,
                                                        "%.2f");
                            });
    scenePropertyRow<std::string>("3D LUT",
                                  "Optional project-relative RGBA strip with dimensions N*N by N.",
                                  m_project, sceneId, scene, "color_grading_lut", [](std::string& value) {
                                      return ImGui::InputText("##color_grading_lut", &value);
                                  });

    tableSectionHeader("Anti-aliasing");
    scenePropertyRow<bool>("FXAA",
                           "Fast approximate anti-aliasing over the finished image. It runs\n"
                           "after the tone map and after the internal-resolution upscale, so it\n"
                           "smooths what the viewer actually sees. Cheap, and it softens detail\n"
                           "as it removes the stair-steps.",
                           m_project, sceneId, scene, "fxaa_enabled", [](bool& value) {
                               bool on = value;
                               if (!ImGui::Checkbox("##fxaa_enabled", &on)) return false;
                               value = on;
                               return true;
                           });
    scenePropertyRow<float>("FXAA Strength",
                            "Blend of the anti-aliased result with its own input. Lower keeps\n"
                            "sharpness and lets the jaggies back through.",
                            m_project, sceneId, scene, "fxaa_intensity", [](float& value) {
                                return ImGui::DragFloat("##fxaa_intensity", &value, 0.01f, 0.0f, 1.0f, "%.2f");
                            });

    tableSectionHeader("Internal resolution");
    scenePropertyRow<bool>("Fixed Resolution",
                           "Renders the scene at a fixed size and upscales it to the window.\n"
                           "A cheap way to keep a frame rate on a 4K display, and the reload\n"
                           "costs a moment. With TAA enabled, the temporal upscaler reconstructs\n"
                           "the native view before FXAA. Upscale filter below.",
                           m_project, sceneId, scene, "fixed_resolution_enabled", [](bool& value) {
                               bool on = value;
                               if (!ImGui::Checkbox("##fixed_resolution_enabled", &on)) return false;
                               value = on;
                               return true;
                           });
    scenePropertyRow<int>("Fixed Resolution Width", "Render width in pixels.", m_project, sceneId,
                          scene, "fixed_resolution_width", [](int& value) {
                              return ImGui::InputInt("##fixed_resolution_width", &value, 0, 0);
                          });
    scenePropertyRow<int>("Fixed Resolution Height", "Render height in pixels.", m_project, sceneId,
                          scene, "fixed_resolution_height", [](int& value) {
                              return ImGui::InputInt("##fixed_resolution_height", &value, 0, 0);
                          });
    scenePropertyRow<TextureFilter>("Upscale Filter",
                                    "Nearest keeps pixels sharp (retro look); Linear blends.",
                                    m_project, sceneId, scene, "fixed_resolution_filter",
                                    [](TextureFilter& value) {
                                        int item = value == TextureFilter::LINEAR ? 1 : 0;
                                        if (!ImGui::Combo("##fixed_resolution_filter", &item, textureFilterNames,
                                                          static_cast<int>(std::size(textureFilterNames)))) {
                                            return false;
                                        }
                                        value = item == 1 ? TextureFilter::LINEAR : TextureFilter::NEAREST;
                                        return true;
                                    });

    // Above the window size the same path supersamples: the scene renders bigger and the
    // blit downscales it, which is the cheapest sharpness the fixed-resolution pass buys.
    beginSettingsRow("Resolution preset",
                     "Sets the two sizes above and turns Fixed Resolution on.\n"
                     "Native clears it: the scene then renders at the window size.");
    if (m_project && scene) {
        static constexpr struct { const char* label; int width; int height; } kResolutionPresets[] = {
            { "720p", 1280, 720 }, { "1080p", 1920, 1080 },
            { "1440p", 2560, 1440 }, { "4K", 3840, 2160 },
        };
        for (const auto& preset : kResolutionPresets) {
            ImGui::SameLine();
            if (ImGui::SmallButton(preset.label)) {
                applyResolutionPreset(m_project, sceneId, preset.width, preset.height);
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Native")) {
            CommandHandle::get(sceneId)->addCommand(
                new ScenePropertyCmd<bool>(m_project, sceneId, "fixed_resolution_enabled", false));
        }
    } else {
        ImGui::TextDisabled("no scene");
    }

    ImGui::EndTable();
}

// The editor's own view cap, kept out of the scene so tuning it never changes what the
// project saves or ships. It reads and writes AppSettings, not the Scene.
void EditorSettingsWindow::drawEditorViewportRows() {
    if (!beginSettingsTable("##EditorViewportTable")) return;

    const bool changed = m_viewportRenderDistance != AppSettings::getViewportRenderDistance();
    if (beginSettingsRow("Viewport Render Distance",
                         "How far the scene viewport draws while you edit, in metres. 0 follows\n"
                         "each scene's own render distance. Play mode and exported builds ignore\n"
                         "it, so a short value is a navigation aid, not a project change.",
                         changed)) {
        m_viewportRenderDistance = AppSettings::getViewportRenderDistance();
    }
    ImGui::SetNextItemWidth(Theme::dpi(120.0f));
    ImGui::DragFloat("##ViewportRenderDistance", &m_viewportRenderDistance, 1.0f, 0.0f, 100000.0f, "%.0f m");
    for (const float metres : { 100.0f, 300.0f, 1000.0f, 0.0f }) {
        ImGui::SameLine();
        char preset[16];
        if (metres > 0.0f) std::snprintf(preset, sizeof(preset), "%.0f m", metres);
        else std::snprintf(preset, sizeof(preset), "Scene");
        if (ImGui::SmallButton(preset)) m_viewportRenderDistance = metres;
    }

    ImGui::EndTable();
}

void EditorSettingsWindow::drawGraphicsSettings() {
    if (!m_graphicsLoaded) syncGraphicsBuffers();

    const bool panel = beginSettingsPanel("##EditorGraphicsPanel");
    if (!panel) {
        ImGui::PopStyleVar(3);
        return;
    }
    drawSectionTitle("Scene viewport");
    drawEditorViewportRows();
    ImGui::Spacing();

    if (!m_project) {
        ImGui::TextDisabled("Open a project to edit its graphics settings.");
        endSettingsPanel();
        return;
    }

    drawSectionTitle("Project output");
    drawProjectOutputRows();
    ImGui::Spacing();

    drawSectionTitle("Scene render quality");
    drawSceneQualityRows();

    if (!m_graphicsError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::DangerText);
        ImGui::TextWrapped("%s", m_graphicsError.c_str());
        ImGui::PopStyleColor();
    }

    drawSectionNote("The scene block applies as you click and is undoable with Ctrl+Z; the\n"
                    "project block is written to project.yaml when you press OK. Canvas, scaling\n"
                    "and texture strategy take effect on Play. SSAO, SSR, the depth pre-pass and\n"
                    "fixed resolution rebuild the affected shaders, so a big scene pauses for a\n"
                    "frame or two. Bloom, the mip chain, tonemapping, manual and automatic\n"
                    "exposure, colour grading and lens dirt all draw. The viewport render\n"
                    "distance is the editor's own view cap and is not saved with the scene.");
    endSettingsPanel();
}

bool EditorSettingsWindow::applyGraphicsSettings() {
    if (!m_project || !m_graphicsLoaded) return true;
    m_graphicsError.clear();

    const int canvasWidth = parseSize(m_canvasWidth.data(), static_cast<int>(m_project->getCanvasWidth()));
    const int canvasHeight = parseSize(m_canvasHeight.data(), static_cast<int>(m_project->getCanvasHeight()));
    const int windowWidth = parseSize(m_windowWidth.data(), static_cast<int>(m_project->getWindowWidth()));
    const int windowHeight = parseSize(m_windowHeight.data(), static_cast<int>(m_project->getWindowHeight()));

    bool changed = false;
    if (static_cast<unsigned int>(canvasWidth) != m_project->getCanvasWidth() ||
        static_cast<unsigned int>(canvasHeight) != m_project->getCanvasHeight()) {
        m_project->setCanvasSize(canvasWidth, canvasHeight);
        changed = true;
    }
    const int scalingIndex = std::clamp(m_scalingMode, 0, static_cast<int>(std::size(scalingModeNames)) - 1);
    const Scaling scaling = static_cast<Scaling>(scalingIndex);
    if (scaling != m_project->getScalingMode()) {
        m_project->setScalingMode(scaling);
        changed = true;
    }
    const int strategyIndex = std::clamp(m_textureStrategy, 0,
                                         static_cast<int>(std::size(textureStrategyNames)) - 1);
    const TextureStrategy strategy = static_cast<TextureStrategy>(strategyIndex);
    if (strategy != m_project->getTextureStrategy()) {
        m_project->setTextureStrategy(strategy);
        changed = true;
    }
    const int modeIndex = std::clamp(m_windowMode, 0, static_cast<int>(std::size(windowModeNames)) - 1);
    const WindowMode mode = static_cast<WindowMode>(modeIndex);
    if (mode != m_project->getWindowMode()) {
        m_project->setWindowMode(mode);
        changed = true;
    }
    if (static_cast<unsigned int>(windowWidth) != m_project->getWindowWidth() ||
        static_cast<unsigned int>(windowHeight) != m_project->getWindowHeight()) {
        m_project->setWindowSize(windowWidth, windowHeight);
        changed = true;
    }
    if (m_projectVSync != m_project->isVSyncEnabled()) {
        m_project->setVSyncEnabled(m_projectVSync);
        changed = true;
    }

    if (!changed) return true;
    if (!m_project->saveProjectFile()) {
        m_graphicsError = "project.yaml could not be written. Check its permissions and press OK again.";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Engine API tab
// ---------------------------------------------------------------------------

namespace {

    // Colour comes from the style, so both presets stay legible: what is callable reads
    // different from what is a value.
    ImGuiCol kindColor(const std::string& kind) {
        if (kind == "Property" || kind == "Constant" || kind == "EnumMember") return ImGuiCol_TextDisabled;
        if (kind == "Class" || kind == "Enum") return ImGuiCol_Text;
        return ImGuiCol_CheckMark;
    }

}

void EditorSettingsWindow::rebuildApiRows() {
    const std::vector<std::string>& kinds = EngineApiIndex::kinds();
    const std::string kind = m_apiKind > 0 && m_apiKind - 1 < static_cast<int>(kinds.size())
                                 ? kinds[m_apiKind - 1] : std::string();
    const std::string owner = m_apiOwner > 0 && m_apiOwner - 1 < static_cast<int>(m_apiOwners.size())
                                  ? m_apiOwners[m_apiOwner - 1].first : std::string();
    m_apiRows = EngineApiIndex::filter(m_apiSearch.data(), kind, owner);
    m_apiRowsKey = std::string(m_apiSearch.data()) + "|" + std::to_string(m_apiKind) + "|" +
                   std::to_string(m_apiOwner);
}

void EditorSettingsWindow::drawApiSettings() {
    const bool panel = beginSettingsPanel("##EditorApiPanel");
    if (!panel) {
        ImGui::PopStyleVar(3);
        return;
    }

    const std::vector<EngineAPISymbol>& symbols = EngineApiIndex::symbols();
    const std::vector<std::string>& kinds = EngineApiIndex::kinds();
    const std::vector<int> kindCounts = EngineApiIndex::kindCounts();
    if (m_apiOwners.empty()) m_apiOwners = EngineApiIndex::owners();

    ImGui::PushItemWidth(std::max(140.0f, ImGui::GetContentRegionAvail().x * 0.4f));
    ImGui::InputTextWithHint("##ApiSearch", "Search names, owners and signatures",
                             m_apiSearch.data(), m_apiSearch.size());
    ImGui::PopItemWidth();
    if (m_apiSearch[0] != '\0') {
        ImGui::SameLine();
        if (inlineIconButton(ICON_FA_XMARK, "ClearApiSearch", "Clear the search")) {
            m_apiSearch[0] = '\0';
        }
    }

    ImGui::SameLine();
    ImGui::PushItemWidth(Theme::dpi(150.0f));
    if (ImGui::BeginCombo("##ApiKind", m_apiKind == 0 ? "All kinds" : kinds[m_apiKind - 1].c_str())) {
        if (ImGui::Selectable("All kinds", m_apiKind == 0)) m_apiKind = 0;
        for (size_t index = 0; index < kinds.size(); ++index) {
            const int number = static_cast<int>(index) + 1;
            const bool selected = m_apiKind == number;
            if (ImGui::Selectable((kinds[index] + "  (" + std::to_string(kindCounts[index]) + ")").c_str(),
                                  selected)) {
                m_apiKind = number;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::PopItemWidth();

    ImGui::SameLine();
    ImGui::PushItemWidth(Theme::dpi(180.0f));
    if (ImGui::BeginCombo("##ApiOwner",
                          m_apiOwner == 0 ? "All owners" : m_apiOwners[m_apiOwner - 1].first.c_str(),
                          ImGuiComboFlags_HeightLargest)) {
        if (ImGui::Selectable("All owners", m_apiOwner == 0)) m_apiOwner = 0;
        for (size_t index = 0; index < m_apiOwners.size(); ++index) {
            const int number = static_cast<int>(index) + 1;
            const bool selected = m_apiOwner == number;
            if (ImGui::Selectable((m_apiOwners[index].first + "  (" +
                                   std::to_string(m_apiOwners[index].second) + ")").c_str(), selected)) {
                m_apiOwner = number;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::PopItemWidth();

    // Rebuilt before the buttons that read it, so a copy never sends a stale list.
    const std::string key = std::string(m_apiSearch.data()) + "|" + std::to_string(m_apiKind) + "|" +
                            std::to_string(m_apiOwner);
    if (key != m_apiRowsKey) rebuildApiRows();

    ImGui::SameLine();
    if (inlineIconButton(ICON_FA_COPY, "CopyApiRows",
                         "Copy the filtered rows as one Lua-visible name per line, so the list\n"
                         "can be pasted into a script or compared with another engine build.")) {
        std::string text;
        for (const int index : m_apiRows) {
            const EngineAPISymbol& symbol = symbols[index];
            text += symbol.parent[0] ? std::string(symbol.parent) + "." + symbol.name
                                     : std::string(symbol.name);
            text += "\n";
            if (text.size() > 200000) break;
        }
        ImGui::SetClipboardText(text.c_str());
        m_apiNote = "Copied " + std::to_string(m_apiRows.size()) + " name(s) to the clipboard.";
    }
    ImGui::SameLine();
    if (inlineIconButton(ICON_FA_ROTATE_LEFT, "ResetApiFilters", "Clear the search and both filters")) {
        m_apiSearch[0] = '\0';
        m_apiKind = 0;
        m_apiOwner = 0;
        m_apiNote.clear();
    }

    if (symbols.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::WarningText);
        ImGui::TextWrapped("The generated API header is empty in this build.");
        ImGui::PopStyleColor();
        ImGui::TextWrapped("Rebuild it with: cmake --build build --target generate_api_suggestions");
        endSettingsPanel();
        return;
    }

    if (!m_apiNote.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::TextUnformatted(m_apiNote.c_str());
        ImGui::PopStyleColor();
    }

    const float listHeight = std::max(Theme::dpi(160.0f), ImGui::GetContentRegionAvail().y - Theme::dpi(30.0f));
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##ApiTable", 5, flags, ImVec2(0, listHeight))) {
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(34.0f));
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.26f);
        ImGui::TableSetupColumn("Owner", ImGuiTableColumnFlags_WidthStretch, 0.20f);
        ImGui::TableSetupColumn("Signature", ImGuiTableColumnFlags_WidthStretch, 0.44f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize(ICON_FA_COPY).x +
                                                                         ImGui::GetStyle().FramePadding.x * 2.0f);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_apiRows.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const EngineAPISymbol& symbol = symbols[m_apiRows[row]];
                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                const std::string kind(symbol.kind);
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImGui::GetStyle().Colors[kindColor(kind)]);
                ImGui::TextUnformatted(EngineApiIndex::kindLabel(kind));
                ImGui::PopStyleColor();
                ImGui::SetItemTooltip("%s", kind.c_str());

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(symbol.name);

                ImGui::TableNextColumn();
                if (symbol.parent[0] != '\0') {
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                    ImGui::TextUnformatted(symbol.parent);
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemClicked()) {
                        // Clicking an owner is the shortest route to "what else does it have".
                        for (size_t index = 0; index < m_apiOwners.size(); ++index) {
                            if (m_apiOwners[index].first != symbol.parent) continue;
                            m_apiOwner = static_cast<int>(index) + 1;
                            break;
                        }
                    }
                }

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(symbol.detail);

                ImGui::TableNextColumn();
                if (inlineIconButton(ICON_FA_COPY, "CopyApiRow" + std::to_string(row),
                                     "Copy the signature")) {
                    ImGui::SetClipboardText(symbol.detail);
                    m_apiNote = std::string("Copied: ") + symbol.detail;
                }
            }
        }
        clipper.End();
        ImGui::EndTable();
    }

    ImGui::TextDisabled("%d of %d symbols | %d owners | header: build/generated/engine_api_suggestions.h",
                        static_cast<int>(m_apiRows.size()), static_cast<int>(symbols.size()),
                        static_cast<int>(m_apiOwners.size()));
    endSettingsPanel();
}

} // namespace quantum::editor
