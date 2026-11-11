// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "AppSettings.h"
#include "Exporter.h"
#include "Generator.h"
#include "ai/AiTypes.h"
#include "imgui.h"

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace quantum::editor {

    class Project;

    class EditorSettingsWindow {
    public:
        enum class Tab {
            General,
            Desktop,
            Shortcuts,
            Graphics,
            Api,
            Web,
            AI,
            Advanced
        };

    private:
        // std::map keeps buffer addresses stable for ImGui as endpoints come and go.
        struct KeyEntry {
            std::array<char, 512> buffer{};
            bool configured = false;
        };
        struct EndpointEntry {
            std::array<char, 128> label{};
            std::array<char, 512> url{};
        };

        bool m_isOpen = false;
        std::optional<Tab> m_requestedTab;
        // Kept until the AI tab draws, which can be a frame after the tab request
        bool m_expandAiChat = false;

        // Build settings are per project, so the dialog edits the open project's
        Project* m_project = nullptr;

        std::vector<CMakeKit> m_availableKits;
        int m_cmakeKitIndex = 0;
        std::string m_cmakeOverride;
        CMakeInfo m_cmakeInfo;
        std::string m_cmakePickError;
        // Probing runs processes, so it waits for the tab that shows the result
        bool m_kitsDetected = false;
        bool m_emsdkDetected = false;

        std::string m_emsdkOverride;
        EmsdkInfo m_emsdkInfo;
        bool m_editorVSyncEnabled = true;
        int m_uiScalePercent = 100;
        std::filesystem::path m_defaultExportDirectory;
        std::string m_cacheStatus;
        // Typed as a buffer because ImGui edits it in place; the extension is a short
        // ASCII name no matter what the user types.
        std::array<char, 32> m_projectFileExtension{};

        ai::Settings m_aiSettings;
        std::map<std::string, KeyEntry> m_aiKeys;           // account id -> typed key
        std::map<std::string, EndpointEntry> m_aiEndpoints;  // endpoint id -> typed fields
        // Endpoints removed here. Wiped only on OK, so Cancel restores their keys.
        std::vector<std::string> m_removedEndpoints;
        // Endpoint being renamed in place, and its one-frame focus request.
        std::string m_editingEndpointId;
        bool m_focusEndpointLabel = false;

        ai::McpSettings m_mcpSettings;
        std::string m_mcpToken;

        // Shortcuts tab. The capture state is here rather than in the row so one frame's
        // keypress lands on the row the user clicked, even if the filter hides it.
        std::array<char, 128> m_shortcutSearch{};
        std::string m_capturedId;      // binding waiting for a key, "" when not capturing
        std::string m_captureMods;     // modifiers held so far, shown as the hint
        std::string m_shortcutError;   // last rejected chord, shown once
        std::string m_shortcutNote;    // last bulk import result, shown once

        // Graphics tab. The project rows mirror project.yaml and are written only on OK,
        // like the build settings; the scene rows apply as they are clicked because they go
        // through the same undo command the Inspector uses.
        std::array<char, 16> m_canvasWidth{};
        std::array<char, 16> m_canvasHeight{};
        std::array<char, 16> m_windowWidth{};
        std::array<char, 16> m_windowHeight{};
        int m_scalingMode = 0;
        int m_textureStrategy = 0;
        int m_windowMode = 0;
        bool m_projectVSync = true;
        // Editor-only view cap, in metres (0 = follow each scene). Not project data: it lives
        // in settings.yaml so tuning it does not touch the scene the game ships with.
        float m_viewportRenderDistance = 0.0f;
        bool m_graphicsLoaded = false;   // buffers filled from the project since open()
        int m_graphicsScene = 0;         // index into the open project's scenes
        int m_graphicsPreset = 2;        // Low, Medium, High, Ultra; applied to the selected scene
        std::string m_graphicsError;     // project.yaml refused to be written, shown once

        // Engine API tab. The filtered row list is cached: the table holds every symbol the
        // reflector emitted, so walking it again per frame is wasted work.
        std::array<char, 128> m_apiSearch{};
        int m_apiKind = 0;               // 0 = all kinds, else 1 + index into kinds()
        int m_apiOwner = 0;              // 0 = all owners, else 1 + index into m_apiOwners
        std::vector<std::pair<std::string, int>> m_apiOwners;
        std::vector<int> m_apiRows;
        std::string m_apiRowsKey;        // search + kind + owner the rows were built for
        std::string m_apiNote;           // copy feedback, shown once

        LocalBuildSettings projectBuildSettings() const;
        void detectKits();
        void refreshCMakeStatus();
        void refreshEmsdkStatus();
        ImGuiTabItemFlags tabFlags(Tab tab) const;
        void drawSettings();
        void drawGeneralSettings();
        void drawCMakeSettings();
        void drawWebSettings();
        void drawAiSettings();
        void drawAiChatSettings();
        void drawAiKeyRow(const std::string& accountId, const std::string& label, bool dimLabel = false);
        void drawAiEndpointRows();
        void drawAddEndpointButton();
        void drawMcpSettings();
        void drawAdvancedSettings();
        void drawShortcutSettings();
        void drawShortcutRow(const char* id);
        void handleShortcutCapture();
        // Applies every "id: chord" line found on the clipboard as a rebind.
        void applyShortcutClipboard();
        void drawGraphicsSettings();
        void drawProjectOutputRows();
        void drawSceneQualityRows();
        void drawEditorViewportRows();
        void syncGraphicsBuffers();
        // False when project.yaml refused to be written, which keeps the dialog open
        bool applyGraphicsSettings();
        void drawApiSettings();
        void rebuildApiRows();
        void syncAiBuffers();
        void refreshAiKeyState();
        void addAiEndpoint(const std::string& label, const std::string& url);
        void removeAiEndpoint(const std::string& endpointId);
        void applyAiSettings();
        bool applySettings();

    public:
        EditorSettingsWindow() = default;
        ~EditorSettingsWindow() = default;

        // Opens on the given tab, or on the one shown last
        void open(Project* project, std::optional<Tab> tab = std::nullopt, bool expandAiChat = false);
        void show();
        bool isOpen() const { return m_isOpen; }
    };

}
