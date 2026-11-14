// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "AppSettings.h"
#include "Out.h"
#include "ai/SecretStore.h"
#include "util/FileUtils.h"
#include "util/ProjectFile.h"
#include <fstream>
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace quantum::editor {

namespace {

std::filesystem::path userConfigDirectory() {
#if defined(_WIN32)
    if (const char* appData = std::getenv("APPDATA"); appData && *appData) {
        return std::filesystem::path(appData) / "Quantum";
    }
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / "Library" / "Application Support" / "Quantum";
    }
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg) / "quantum";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".config" / "quantum";
    }
#endif
    return std::filesystem::current_path();
}

// Where the same files lived before the fork took the Quantum name.
std::filesystem::path legacyUserConfigDirectory() {
#if defined(_WIN32)
    if (const char* appData = std::getenv("APPDATA"); appData && *appData) {
        return std::filesystem::path(appData) / "Quantum";
    }
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / "Library" / "Application Support" / "Quantum";
    }
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg) / "quantum";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".config" / "quantum";
    }
#endif
    return {};
}

bool isWritableDirectory(const std::filesystem::path& dir) {
    const std::filesystem::path probe = dir / ".quantum_write_test";
    std::error_code ec;
    {
        std::ofstream f(probe);
        if (!f.is_open()) return false;
    }
    std::filesystem::remove(probe, ec);
    return true;
}

// Upgrades the single "custom_endpoint" URL to the named endpoint list. An empty
// URL used to mean OpenRouter.
void migrateLegacyEndpoint(ai::Settings& settings, const std::string& url) {
    const std::string legacyAccount = ai::toString(ai::ProviderId::OpenAICompatible);
    if (url.empty() && settings.provider != ai::ProviderId::OpenAICompatible &&
        !ai::SecretStore::hasApiKey(legacyAccount)) {
        return; // never configured; do not invent an entry
    }

    ai::CustomEndpoint endpoint;
    endpoint.id = "custom";
    endpoint.label = "Custom endpoint";
    endpoint.url = url;
    for (const ai::EndpointPreset& preset : ai::endpointPresets()) {
        if (url.empty() ? preset.id == "openrouter" : preset.url == url) {
            endpoint = {preset.id, preset.label, preset.url};
            break;
        }
    }

    settings.customEndpoints.push_back(endpoint);
    if (settings.provider == ai::ProviderId::OpenAICompatible) {
        settings.endpointId = endpoint.id;
    }
    ai::SecretStore::renameAccount(legacyAccount,
                                   ai::accountKey(ai::ProviderId::OpenAICompatible, endpoint.id));
}

} // namespace

// Initialize static members
std::filesystem::path AppSettings::configFilePath;
YAML::Node AppSettings::settingsData;
std::vector<std::filesystem::path> AppSettings::recentProjects;
std::filesystem::path AppSettings::lastProjectPath;
std::string AppSettings::lastCMakeCCompiler;
std::string AppSettings::lastCMakeCxxCompiler;
std::string AppSettings::lastCMakeGenerator;
std::string AppSettings::emsdkPath;
std::string AppSettings::cmakePath;
std::filesystem::path AppSettings::defaultExportDirectory;
std::string AppSettings::terminalShell;
int AppSettings::windowWidth = 1280;
int AppSettings::windowHeight = 720;
bool AppSettings::isMaximized = false;
float AppSettings::windowUiScale = 0.0f;
float AppSettings::layoutUiScale = 0.0f;
int AppSettings::resourcesIconSize = 32;
int AppSettings::resourcesLayout = 0;
int AppSettings::resourcesItemViewStyle = 1;
float AppSettings::resourcesLeftPanelWidth = 200.0f;
float AppSettings::codeEditorFontSize = AppSettings::defaultCodeEditorFontSize;
bool AppSettings::multiViewportEnabled = false;
bool AppSettings::editorVSyncEnabled = true;
float AppSettings::uiScale = 1.0f;
bool AppSettings::previousSessionUnclean = false;
bool AppSettings::sessionMarkerWritten = false;
float AppSettings::viewportRenderDistance = 0.0f;
PanelVisibilitySettings AppSettings::panelVisibility;
int AppSettings::themePreset = 0;
std::string AppSettings::accentColor;
std::string AppSettings::projectFileExtension;
std::map<std::string, std::string> AppSettings::shortcutOverrides;
std::vector<std::string> AppSettings::openPluginWindows;
ai::Settings AppSettings::aiSettings;
ai::McpSettings AppSettings::mcpSettings;

bool AppSettings::initialize() {
    FileUtils::adoptLegacyDirectory(legacyUserConfigDirectory(), userConfigDirectory());

    // Settings live in the working directory, where a run from a build tree
    // keeps them. A desktop launcher starts the app in "/" instead, so there
    // they go to the per-user config directory: nothing could be saved
    // otherwise.
    const std::filesystem::path workingDir = std::filesystem::current_path();
    const std::filesystem::path workingDirSettings = workingDir / "settings.yaml";

    std::error_code ec;
    if (std::filesystem::exists(workingDirSettings, ec)) {
        configFilePath = workingDirSettings;
    } else {
        const std::filesystem::path userDir = userConfigDirectory();
        // A project folder is content, not a home for editor config: double-clicking a
        // project marker starts the editor with the project as the working directory,
        // and settings.yaml, editor.log and the layout ini would appear inside it.
        const bool projectWorkingDir = !ProjectFile::resolve(workingDir).empty();
        if (std::filesystem::exists(userDir / "settings.yaml", ec) || !isWritableDirectory(workingDir)
            || projectWorkingDir) {
            configFilePath = userDir / "settings.yaml";
        } else {
            configFilePath = workingDirSettings;
        }
    }

    ensureConfigDirectory();
    const std::filesystem::path sessionMarker = configFilePath.parent_path() / ".editor-session-active";
    previousSessionUnclean = std::filesystem::exists(sessionMarker);
    sessionMarkerWritten = false;
    return loadSettings();
}

bool AppSettings::wasPreviousSessionUnclean() {
    return previousSessionUnclean;
}

bool AppSettings::beginSession() {
    const std::filesystem::path marker = getConfigDirectory() / ".editor-session-active";
    try {
        std::ofstream file(marker, std::ios::out | std::ios::trunc);
        if (!file.is_open()) {
            Out::warning("Could not create editor recovery marker: %s", marker.string().c_str());
            return false;
        }
        file << "editor session active\n";
        file.flush();
        sessionMarkerWritten = file.good();
    } catch (const std::exception& e) {
        Out::warning("Could not create editor recovery marker: %s", e.what());
        sessionMarkerWritten = false;
    }
    return sessionMarkerWritten;
}

void AppSettings::endSession() {
    if (!sessionMarkerWritten) return;
    std::error_code ec;
    std::filesystem::remove(getConfigDirectory() / ".editor-session-active", ec);
    if (ec) {
        Out::warning("Could not clear editor recovery marker: %s", ec.message().c_str());
    }
    sessionMarkerWritten = false;
}

std::filesystem::path AppSettings::getConfigDirectory() {
    if (configFilePath.empty()) {
        return std::filesystem::current_path();
    }
    return configFilePath.parent_path();
}

void AppSettings::ensureConfigDirectory() {
    std::filesystem::path configDir = configFilePath.parent_path();
    if (!std::filesystem::exists(configDir)) {
        try {
            std::filesystem::create_directories(configDir);
        } catch (const std::exception& e) {
            Out::error("Failed to create config directory: %s", e.what());
        }
    }
}

bool AppSettings::loadSettings() {
    try {
        if (!std::filesystem::exists(configFilePath)) {
            settingsData = YAML::Node();
            return false;
        }
        
        settingsData = YAML::LoadFile(configFilePath.string());
        
        // Load last project path
        if (settingsData["last_project_path"]) {
            std::string path = settingsData["last_project_path"].as<std::string>();
            if (!path.empty() && std::filesystem::exists(path)) {
                lastProjectPath = std::filesystem::path(path);
            }
        }
        
        // Load last compiler kit
        if (settingsData["cmake_kit"]) {
            auto kitNode = settingsData["cmake_kit"];
            if (kitNode["c_compiler"]) lastCMakeCCompiler = kitNode["c_compiler"].as<std::string>();
            if (kitNode["cxx_compiler"]) lastCMakeCxxCompiler = kitNode["cxx_compiler"].as<std::string>();
            if (kitNode["generator"]) lastCMakeGenerator = kitNode["generator"].as<std::string>();
        }

        // Load Emscripten SDK path override
        if (settingsData["emsdk"] && settingsData["emsdk"]["path"]) {
            emsdkPath = settingsData["emsdk"]["path"].as<std::string>();
        }

        // Load cmake executable override
        if (settingsData["cmake"] && settingsData["cmake"]["path"]) {
            cmakePath = settingsData["cmake"]["path"].as<std::string>();
        }
        if (settingsData["export"] && settingsData["export"]["default_dir"]) {
            defaultExportDirectory = settingsData["export"]["default_dir"].as<std::string>();
        }

        // Load recent projects
        recentProjects.clear();
        if (settingsData["recent_projects"]) {
            for (const auto& path : settingsData["recent_projects"]) {
                std::string pathStr = path.as<std::string>();
                if (!pathStr.empty() && std::filesystem::exists(pathStr)) {
                    recentProjects.push_back(std::filesystem::path(pathStr));
                }
            }
        }
        
        // Load window settings
        if (settingsData["window"]) {
            auto windowNode = settingsData["window"];
            if (windowNode["width"]) {
                windowWidth = windowNode["width"].as<int>();
            }
            if (windowNode["height"]) {
                windowHeight = windowNode["height"].as<int>();
            }
            if (windowNode["maximized"]) {
                isMaximized = windowNode["maximized"].as<bool>();
            }
            if (windowNode["ui_scale"]) {
                windowUiScale = windowNode["ui_scale"].as<float>();
            }
            if (windowNode["layout_ui_scale"]) {
                layoutUiScale = windowNode["layout_ui_scale"].as<float>();
            }
        }
        
        // Load resources window settings
        if (settingsData["resources_window"]) {
            auto resNode = settingsData["resources_window"];
            if (resNode["icon_size"]) {
                resourcesIconSize = resNode["icon_size"].as<int>();
            }
            if (resNode["layout"]) {
                resourcesLayout = resNode["layout"].as<int>();
            }
            if (resNode["item_view_style"]) {
                resourcesItemViewStyle = resNode["item_view_style"].as<int>();
            }
            if (resNode["left_panel_width"]) {
                resourcesLeftPanelWidth = resNode["left_panel_width"].as<float>();
            }
        }

        // Load code editor settings
        if (settingsData["code_editor"]) {
            auto codeNode = settingsData["code_editor"];
            if (codeNode["font_size"]) {
                setCodeEditorFontSize(codeNode["font_size"].as<float>());
            }
        }

        // Load editor viewport settings
        if (settingsData["editor"]) {
            auto editorNode = settingsData["editor"];
            if (editorNode["multi_viewport"]) {
                multiViewportEnabled = editorNode["multi_viewport"].as<bool>();
            }
            if (editorNode["vsync"]) {
                editorVSyncEnabled = editorNode["vsync"].as<bool>();
            }
            if (editorNode["ui_scale"]) {
                setUiScale(editorNode["ui_scale"].as<float>());
            }
            if (editorNode["render_distance"]) {
                viewportRenderDistance = std::max(0.0f, editorNode["render_distance"].as<float>());
            }
            if (editorNode["panels"]) {
                auto panelsNode = editorNode["panels"];
                if (panelsNode["structure"]) panelVisibility.structure = panelsNode["structure"].as<bool>();
                if (panelsNode["properties"]) panelVisibility.properties = panelsNode["properties"].as<bool>();
                if (panelsNode["resources"]) panelVisibility.resources = panelsNode["resources"].as<bool>();
                if (panelsNode["output"]) panelVisibility.output = panelsNode["output"].as<bool>();
                if (panelsNode["animation"]) panelVisibility.animation = panelsNode["animation"].as<bool>();
                if (panelsNode["terrain"]) panelVisibility.terrain = panelsNode["terrain"].as<bool>();
                if (panelsNode["ai_chat"]) panelVisibility.aiChat = panelsNode["ai_chat"].as<bool>();
                if (panelsNode["notifications"]) panelVisibility.notifications = panelsNode["notifications"].as<bool>();
                if (panelsNode["resource_monitor"]) panelVisibility.resourceMonitor = panelsNode["resource_monitor"].as<bool>();
                if (panelsNode["graph_editor"]) panelVisibility.graphEditor = panelsNode["graph_editor"].as<bool>();
                if (panelsNode["terminal"]) panelVisibility.terminal = panelsNode["terminal"].as<bool>();
                if (panelsNode["material_browser"]) panelVisibility.materialBrowser = panelsNode["material_browser"].as<bool>();
                if (panelsNode["model_export"]) panelVisibility.modelExport = panelsNode["model_export"].as<bool>();
                if (panelsNode["plugin_browser"]) panelVisibility.pluginBrowser = panelsNode["plugin_browser"].as<bool>();
                if (panelsNode["world_creator"]) panelVisibility.worldCreator = panelsNode["world_creator"].as<bool>();
            }
            if (editorNode["terminal_shell"]) {
                terminalShell = editorNode["terminal_shell"].as<std::string>();
            }
            if (editorNode["theme_preset"]) {
                themePreset = editorNode["theme_preset"].as<int>();
            }
            if (editorNode["accent_color"]) {
                accentColor = editorNode["accent_color"].as<std::string>();
            }
            if (editorNode["project_file_extension"]) {
                projectFileExtension = editorNode["project_file_extension"].as<std::string>();
            }
            if (editorNode["shortcuts"]) {
                // Replace, not merge: initialize() can run twice in one process, and a
                // stale chord from an earlier load would outrank the file.
                shortcutOverrides.clear();
                for (const auto& entry : editorNode["shortcuts"]) {
                    shortcutOverrides[entry.first.as<std::string>()] =
                        entry.second.IsScalar() ? entry.second.as<std::string>() : std::string();
                }
            }
            if (editorNode["plugin_windows"] && editorNode["plugin_windows"].IsSequence()) {
                // Replace, not append, for the same reason as the shortcuts above.
                openPluginWindows.clear();
                for (const auto& windowNode : editorNode["plugin_windows"]) {
                    if (!windowNode.IsScalar()) continue;
                    const std::string windowId = windowNode.as<std::string>();
                    // The View menu and the host both key on the id, so a repeat in the file would be
                    // one window with two checkboxes.
                    if (std::find(openPluginWindows.begin(), openPluginWindows.end(), windowId)
                        == openPluginWindows.end()) {
                        openPluginWindows.push_back(windowId);
                    }
                }
            }
        }

        // Load AI assistant settings (no API keys)
        if (settingsData["ai_assistant"]) {
            auto aiNode = settingsData["ai_assistant"];
            if (aiNode["provider"]) aiSettings.provider = ai::providerFromString(aiNode["provider"].as<std::string>());
            if (aiNode["model"]) aiSettings.model = aiNode["model"].as<std::string>();
            if (aiNode["endpoint_id"]) aiSettings.endpointId = aiNode["endpoint_id"].as<std::string>();
            if (aiNode["custom_endpoints"] && aiNode["custom_endpoints"].IsSequence()) {
                // Replace, not append: initialize() runs on the CLI and App paths.
                aiSettings.customEndpoints.clear();
                for (const auto& endpointNode : aiNode["custom_endpoints"]) {
                    ai::CustomEndpoint endpoint;
                    if (endpointNode["id"]) endpoint.id = endpointNode["id"].as<std::string>();
                    if (endpointNode["label"]) endpoint.label = endpointNode["label"].as<std::string>();
                    if (endpointNode["url"]) endpoint.url = endpointNode["url"].as<std::string>();
                    // Duplicate ids would share one key and one model list.
                    if (endpoint.id.empty() || ai::findEndpoint(aiSettings, endpoint.id)) continue;
                    aiSettings.customEndpoints.push_back(endpoint);
                }
            }
            if (aiNode["custom_endpoint"] && aiSettings.customEndpoints.empty()) {
                migrateLegacyEndpoint(aiSettings, aiNode["custom_endpoint"].as<std::string>());
            }
            if (aiNode["approval_mode"]) aiSettings.approvalMode = ai::approvalModeFromString(aiNode["approval_mode"].as<std::string>());
            if (aiNode["request_timeout_seconds"]) aiSettings.requestTimeoutSeconds = aiNode["request_timeout_seconds"].as<int>();
            if (aiNode["max_output_tokens"]) aiSettings.maxOutputTokens = aiNode["max_output_tokens"].as<int>();
            if (aiNode["max_tool_rounds"]) aiSettings.maxToolRounds = aiNode["max_tool_rounds"].as<int>();
            if (aiSettings.model.empty()) {
                aiSettings.model = ai::defaultModelForProvider(aiSettings.provider);
            }
        }

        // Load MCP server settings (no token)
        if (settingsData["mcp_server"]) {
            auto mcpNode = settingsData["mcp_server"];
            if (mcpNode["enabled"]) mcpSettings.enabled = mcpNode["enabled"].as<bool>();
            if (mcpNode["port"]) mcpSettings.port = std::clamp(mcpNode["port"].as<int>(), 1024, 65535);
            if (mcpNode["allow_changes"]) mcpSettings.allowChanges = mcpNode["allow_changes"].as<bool>();
        }

        auto finiteOr = [](float value, float fallback) {
            return std::isfinite(value) ? value : fallback;
        };
        windowWidth = std::clamp(windowWidth, 320, 16384);
        windowHeight = std::clamp(windowHeight, 200, 16384);
        windowUiScale = std::clamp(finiteOr(windowUiScale, 0.0f), 0.0f, 3.0f);
        layoutUiScale = std::clamp(finiteOr(layoutUiScale, 0.0f), 0.0f, 3.0f);
        resourcesIconSize = std::clamp(resourcesIconSize, 16, 256);
        resourcesLayout = std::clamp(resourcesLayout, 0, 3);
        resourcesItemViewStyle = std::clamp(resourcesItemViewStyle, 0, 1);
        resourcesLeftPanelWidth = std::clamp(finiteOr(resourcesLeftPanelWidth, 200.0f), 120.0f, 2048.0f);
        setCodeEditorFontSize(finiteOr(codeEditorFontSize, defaultCodeEditorFontSize));
        setUiScale(finiteOr(uiScale, 1.0f));
        if (recentProjects.size() > 32) recentProjects.resize(32);
        return true;
    } catch (const std::exception& e) {
        settingsData = YAML::Node();
        Out::error("Failed to load settings: %s", e.what());
        return false;
    }
}

bool AppSettings::saveSettings() {
    try {
        // Update settings data
        
        // Project settings
        if (!lastProjectPath.empty() && std::filesystem::exists(lastProjectPath)) {
            settingsData["last_project_path"] = lastProjectPath.string();
        } else {
            settingsData.remove("last_project_path");
        }
        
        YAML::Node recentProjectsNode;
        for (const auto& path : recentProjects) {
            recentProjectsNode.push_back(path.string());
        }
        settingsData["recent_projects"] = recentProjectsNode;

        // Last compiler kit
        if (!lastCMakeCCompiler.empty() || !lastCMakeCxxCompiler.empty() || !lastCMakeGenerator.empty()) {
            YAML::Node kitNode;
            kitNode["c_compiler"] = lastCMakeCCompiler;
            kitNode["cxx_compiler"] = lastCMakeCxxCompiler;
            kitNode["generator"] = lastCMakeGenerator;
            settingsData["cmake_kit"] = kitNode;
        } else {
            settingsData.remove("cmake_kit");
        }

        // Emscripten SDK path override
        if (!emsdkPath.empty()) {
            YAML::Node emsdkNode;
            emsdkNode["path"] = emsdkPath;
            settingsData["emsdk"] = emsdkNode;
        } else {
            settingsData.remove("emsdk");
        }

        // cmake executable override
        if (!cmakePath.empty()) {
            YAML::Node cmakeNode;
            cmakeNode["path"] = cmakePath;
            settingsData["cmake"] = cmakeNode;
        } else {
            settingsData.remove("cmake");
        }

        if (!defaultExportDirectory.empty()) {
            YAML::Node exportNode;
            exportNode["default_dir"] = defaultExportDirectory.string();
            settingsData["export"] = exportNode;
        } else {
            settingsData.remove("export");
        }
        
        // Window settings
        YAML::Node windowNode;
        windowNode["width"] = windowWidth;
        windowNode["height"] = windowHeight;
        windowNode["maximized"] = isMaximized;
        // Omitted at 0 so "never recorded" stays distinguishable from a scale.
        if (windowUiScale > 0.0f) {
            windowNode["ui_scale"] = windowUiScale;
        }
        if (layoutUiScale > 0.0f) {
            windowNode["layout_ui_scale"] = layoutUiScale;
        }
        settingsData["window"] = windowNode;
        
        // Resources window settings
        YAML::Node resNode;
        resNode["icon_size"] = resourcesIconSize;
        resNode["layout"] = resourcesLayout;
        resNode["item_view_style"] = resourcesItemViewStyle;
        resNode["left_panel_width"] = resourcesLeftPanelWidth;
        settingsData["resources_window"] = resNode;

        // Code editor settings
        YAML::Node codeNode;
        codeNode["font_size"] = codeEditorFontSize;
        settingsData["code_editor"] = codeNode;

        // Editor viewport settings
        YAML::Node editorNode;
        editorNode["multi_viewport"] = multiViewportEnabled;
        editorNode["vsync"] = editorVSyncEnabled;
        editorNode["ui_scale"] = uiScale;
        // Omitted at 0 so "the viewport follows the scene" stays the readable default.
        if (viewportRenderDistance > 0.0f) {
            editorNode["render_distance"] = viewportRenderDistance;
        }
        YAML::Node panelsNode;
        panelsNode["structure"] = panelVisibility.structure;
        panelsNode["properties"] = panelVisibility.properties;
        panelsNode["resources"] = panelVisibility.resources;
        panelsNode["output"] = panelVisibility.output;
        panelsNode["animation"] = panelVisibility.animation;
        panelsNode["terrain"] = panelVisibility.terrain;
        panelsNode["ai_chat"] = panelVisibility.aiChat;
        panelsNode["notifications"] = panelVisibility.notifications;
        panelsNode["resource_monitor"] = panelVisibility.resourceMonitor;
        panelsNode["graph_editor"] = panelVisibility.graphEditor;
        panelsNode["terminal"] = panelVisibility.terminal;
        panelsNode["material_browser"] = panelVisibility.materialBrowser;
        panelsNode["model_export"] = panelVisibility.modelExport;
        panelsNode["plugin_browser"] = panelVisibility.pluginBrowser;
        panelsNode["world_creator"] = panelVisibility.worldCreator;
        editorNode["panels"] = panelsNode;
        if (terminalShell.empty())
            editorNode.remove("terminal_shell");
        else
            editorNode["terminal_shell"] = terminalShell;
        editorNode["theme_preset"] = themePreset;
        editorNode["accent_color"] = accentColor;
        if (projectFileExtension.empty())
            editorNode.remove("project_file_extension");
        else
            editorNode["project_file_extension"] = projectFileExtension;
        if (shortcutOverrides.empty()) {
            editorNode.remove("shortcuts");
        } else {
            YAML::Node shortcutsNode(YAML::NodeType::Map);
            for (const auto& overrideEntry : shortcutOverrides) {
                shortcutsNode[overrideEntry.first] = overrideEntry.second;
            }
            editorNode["shortcuts"] = shortcutsNode;
        }
        if (openPluginWindows.empty()) {
            editorNode.remove("plugin_windows");
        } else {
            YAML::Node windowsNode(YAML::NodeType::Sequence);
            for (const std::string& windowId : openPluginWindows) {
                windowsNode.push_back(windowId);
            }
            editorNode["plugin_windows"] = windowsNode;
        }
        settingsData["editor"] = editorNode;

        // AI assistant settings. Secrets must never be serialized here.
        YAML::Node aiNode;
        aiNode["provider"] = ai::toString(aiSettings.provider);
        aiNode["model"] = aiSettings.model;
        aiNode["endpoint_id"] = aiSettings.endpointId;
        YAML::Node endpointsNode(YAML::NodeType::Sequence);
        for (const ai::CustomEndpoint& endpoint : aiSettings.customEndpoints) {
            YAML::Node endpointNode;
            endpointNode["id"] = endpoint.id;
            endpointNode["label"] = endpoint.label;
            endpointNode["url"] = endpoint.url;
            endpointsNode.push_back(endpointNode);
        }
        aiNode["custom_endpoints"] = endpointsNode;
        aiNode["approval_mode"] = ai::toString(aiSettings.approvalMode);
        aiNode["request_timeout_seconds"] = aiSettings.requestTimeoutSeconds;
        aiNode["max_output_tokens"] = aiSettings.maxOutputTokens;
        aiNode["max_tool_rounds"] = aiSettings.maxToolRounds;
        settingsData["ai_assistant"] = aiNode;

        YAML::Node mcpNode;
        mcpNode["enabled"] = mcpSettings.enabled;
        mcpNode["port"] = mcpSettings.port;
        mcpNode["allow_changes"] = mcpSettings.allowChanges;
        settingsData["mcp_server"] = mcpNode;

        // Save to file
        std::ofstream fout(configFilePath.string());
        if (!fout) {
            Out::error("Failed to open editor settings for writing: %s", configFilePath.string().c_str());
            return false;
        }
        fout << YAML::Dump(settingsData);
        fout.close();
        if (!fout) {
            Out::error("Failed to write editor settings: %s", configFilePath.string().c_str());
            return false;
        }
        
        return true;
    } catch (const std::exception& e) {
        Out::error("Failed to save settings: %s", e.what());
        return false;
    }
}

std::filesystem::path AppSettings::getLastProjectPath() {
    return lastProjectPath;
}

void AppSettings::setLastProjectPath(const std::filesystem::path& path) {
    lastProjectPath = path;
    // Also add to recent projects
    addToRecentProjects(path, false);

    saveSettings();
}

std::string AppSettings::getLastCMakeCCompiler() {
    return lastCMakeCCompiler;
}

std::string AppSettings::getLastCMakeCxxCompiler() {
    return lastCMakeCxxCompiler;
}

std::string AppSettings::getLastCMakeGenerator() {
    return lastCMakeGenerator;
}

void AppSettings::setLastCMakeKit(const std::string& cCompiler, const std::string& cxxCompiler, const std::string& generator) {
    lastCMakeCCompiler = cCompiler;
    lastCMakeCxxCompiler = cxxCompiler;
    lastCMakeGenerator = generator;
    saveSettings();
}

std::string AppSettings::getEmsdkPath() {
    return emsdkPath;
}

void AppSettings::setEmsdkPath(const std::string& path) {
    emsdkPath = path;
    saveSettings();
}

std::string AppSettings::getCMakePath() {
    return cmakePath;
}

void AppSettings::setCMakePath(const std::string& path) {
    cmakePath = path;
    saveSettings();
}

std::string AppSettings::getTerminalShell() {
    return terminalShell;
}

void AppSettings::setTerminalShell(const std::string& shell) {
    terminalShell = shell;
    saveSettings();
}

std::string AppSettings::getProjectFileExtension() {
    return projectFileExtension;
}

void AppSettings::setProjectFileExtension(const std::string& extension) {
    const std::string normalized = ProjectFile::normalizeExtension(extension);
    // The default is what an unset key already means, so storing it would only make
    // the setting look overridden when nothing is.
    projectFileExtension = normalized == ProjectFile::DEFAULT_EXTENSION ? std::string() : normalized;
    saveSettings();
}

std::filesystem::path AppSettings::getDefaultExportDirectory() {
    return defaultExportDirectory;
}

void AppSettings::setDefaultExportDirectory(const std::filesystem::path& path) {
    defaultExportDirectory = path;
    saveSettings();
}

std::vector<std::filesystem::path> AppSettings::getRecentProjects() {
    return recentProjects;
}

void AppSettings::addToRecentProjects(const std::filesystem::path& path, bool needSave) {
    if (path.empty() || !std::filesystem::exists(path)) {
        return;
    }
    
    // Check if path already exists
    auto it = std::find_if(recentProjects.begin(), recentProjects.end(),
        [&path](const std::filesystem::path& p) {
            return p == path;
        });
    
    // If found, remove it (to add it to the front)
    if (it != recentProjects.end()) {
        recentProjects.erase(it);
    }
    
    // Add to the front
    recentProjects.insert(recentProjects.begin(), path);
    
    // Keep only the most recent 10 projects
    if (recentProjects.size() > 10) {
        recentProjects.resize(10);
    }
    
    // Save changes
    if (needSave)
        saveSettings();
}

void AppSettings::clearRecentProjects() {
    recentProjects.clear();
    saveSettings();
}

int AppSettings::getWindowWidth() {
    return windowWidth;
}

int AppSettings::getWindowHeight() {
    return windowHeight;
}

bool AppSettings::getIsMaximized() {
    return isMaximized;
}

void AppSettings::setWindowWidth(int width) {
    windowWidth = width;
}

void AppSettings::setWindowHeight(int height) {
    windowHeight = height;
}

void AppSettings::setIsMaximized(bool maximized) {
    isMaximized = maximized;
}

float AppSettings::getWindowUiScale() {
    return windowUiScale;
}

void AppSettings::setWindowUiScale(float scale) {
    windowUiScale = scale;
}

float AppSettings::getLayoutUiScale() {
    return layoutUiScale;
}

void AppSettings::setLayoutUiScale(float scale) {
    layoutUiScale = scale;
}

int AppSettings::getResourcesIconSize() {
    return resourcesIconSize;
}

int AppSettings::getResourcesLayout() {
    return resourcesLayout;
}

int AppSettings::getResourcesItemViewStyle() {
    return resourcesItemViewStyle;
}

float AppSettings::getResourcesLeftPanelWidth() {
    return resourcesLeftPanelWidth;
}

void AppSettings::setResourcesIconSize(int size) {
    resourcesIconSize = size;
}

void AppSettings::setResourcesLayout(int layout) {
    resourcesLayout = layout;
}

void AppSettings::setResourcesItemViewStyle(int style) {
    resourcesItemViewStyle = style;
}

void AppSettings::setResourcesLeftPanelWidth(float width) {
    resourcesLeftPanelWidth = width;
}

float AppSettings::getCodeEditorFontSize() {
    return codeEditorFontSize;
}

void AppSettings::setCodeEditorFontSize(float size) {
    codeEditorFontSize = std::clamp(size, minCodeEditorFontSize, maxCodeEditorFontSize);
}

bool AppSettings::getMultiViewportEnabled() {
    return multiViewportEnabled;
}

void AppSettings::setMultiViewportEnabled(bool enabled) {
    multiViewportEnabled = enabled;
}

bool AppSettings::getEditorVSyncEnabled() {
    return editorVSyncEnabled;
}

void AppSettings::setEditorVSyncEnabled(bool enabled) {
    editorVSyncEnabled = enabled;
}

float AppSettings::getViewportRenderDistance() {
    return viewportRenderDistance;
}

void AppSettings::setViewportRenderDistance(float metres) {
    viewportRenderDistance = metres > 0.0f ? metres : 0.0f;
}

float AppSettings::getUiScale() {
    return uiScale;
}

void AppSettings::setUiScale(float scale) {
    // clamp lets NaN through
    uiScale = std::isnan(scale) ? 1.0f : std::clamp(scale, 0.5f, 3.0f);
}

PanelVisibilitySettings AppSettings::getPanelVisibility() {
    return panelVisibility;
}

void AppSettings::setPanelVisibility(const PanelVisibilitySettings& visibility) {
    panelVisibility = visibility;
    saveSettings();
}

std::vector<std::string> AppSettings::getOpenPluginWindows() {
    return openPluginWindows;
}

void AppSettings::setOpenPluginWindows(const std::vector<std::string>& windowIds) {
    // The host calls this on every frame it notices a change, and a change it notices can be the
    // same list it just read back. Writing the file on a no-op would make closing a window a disk
    // hit, and one plugin window closing another would make it a hit per frame.
    if (windowIds == openPluginWindows) return;
    openPluginWindows = windowIds;
    saveSettings();
}

int AppSettings::getThemePreset() {
    return themePreset;
}

void AppSettings::setThemePreset(int preset) {
    themePreset = preset;
    saveSettings();
}

std::string AppSettings::getAccentColor() {
    return accentColor;
}

void AppSettings::setAccentColor(const std::string& color) {
    accentColor = color;
    saveSettings();
}

std::map<std::string, std::string> AppSettings::getShortcutOverrides() {
    return shortcutOverrides;
}

void AppSettings::setShortcutOverride(const std::string& id, const std::string& chords) {
    shortcutOverrides[id] = chords;
    saveSettings();
}

void AppSettings::removeShortcutOverride(const std::string& id) {
    if (shortcutOverrides.erase(id) == 0) return;
    saveSettings();
}

void AppSettings::clearShortcutOverrides() {
    if (shortcutOverrides.empty()) return;
    shortcutOverrides.clear();
    saveSettings();
}

ai::Settings AppSettings::getAiSettings() {
    return aiSettings;
}

void AppSettings::setAiSettings(const ai::Settings& settings) {
    aiSettings = settings;
    if (aiSettings.model.empty()) {
        aiSettings.model = ai::defaultModelForProvider(aiSettings.provider);
    }
    saveSettings();
}

ai::McpSettings AppSettings::getMcpSettings() {
    return mcpSettings;
}

void AppSettings::setMcpSettings(const ai::McpSettings& settings) {
    mcpSettings = settings;
}

} // namespace quantum::editor

namespace quantum::editor {

namespace {

// Compiler kit, job count and export destinations describe one machine, so they sit
// with the project's other per-checkout state. Keyed by absolute path in the
// editor-wide settings.yaml they broke as soon as the project moved.
std::filesystem::path localSettingsFile(const std::filesystem::path& projectFile) {
    const std::filesystem::path projectDir = projectFile.has_filename() && projectFile.filename() == "project.yaml"
        ? projectFile.parent_path()
        : projectFile;
    return projectDir / ".quantum" / "user" / "build.yaml";
}

std::string legacyProjectKey(const std::filesystem::path& projectFile) {
    return std::filesystem::absolute(projectFile).lexically_normal().generic_string();
}

YAML::Node loadLocalSettings(const std::filesystem::path& projectFile) {
    const std::filesystem::path file = localSettingsFile(projectFile);
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        return YAML::Node();
    }
    try {
        YAML::Node node = YAML::LoadFile(file.string());
        return node.IsMap() ? node : YAML::Node();
    } catch (const std::exception& e) {
        Out::warning("Ignoring unreadable local build settings \"%s\": %s", file.string().c_str(), e.what());
        return YAML::Node();
    }
}

bool storeLocalSettings(const std::filesystem::path& projectFile, const YAML::Node& node) {
    const std::filesystem::path file = localSettingsFile(projectFile);
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (ec) {
        Out::error("Failed to create local settings directory: %s", ec.message().c_str());
        return false;
    }

    std::ofstream fout(file.string());
    if (!fout) {
        Out::error("Failed to open local build settings for writing: %s", file.string().c_str());
        return false;
    }
    fout << "# Build configuration for this machine only. Version control ignores .quantum/.\n";
    fout << YAML::Dump(node) << "\n";
    fout.close();
    if (!fout) {
        Out::error("Failed to write local build settings: %s", file.string().c_str());
        return false;
    }
    return true;
}

// One-shot move of the entries an earlier editor wrote into settings.yaml, which is
// left alone since an older editor sharing this machine still reads it.
YAML::Node migrateLegacySection(const std::filesystem::path& projectFile, YAML::Node& local,
                                const YAML::Node& editorSettings, const char* section, const char* localKey) {
    if (local[localKey]) {
        return local[localKey];
    }
    const YAML::Node root = editorSettings[section];
    if (!root || !root.IsMap()) {
        return YAML::Node();
    }
    const YAML::Node entry = root[legacyProjectKey(projectFile)];
    if (!entry || !entry.IsMap()) {
        return YAML::Node();
    }
    local[localKey] = YAML::Clone(entry);
    storeLocalSettings(projectFile, local);
    return local[localKey];
}

} // namespace

std::filesystem::path AppSettings::getExportTargetDir(const std::filesystem::path& projectFile, const std::string& mode) {
    YAML::Node local = loadLocalSettings(projectFile);
    const YAML::Node exports = migrateLegacySection(projectFile, local, settingsData, "project_exports", "exports");
    if (!exports || !exports.IsMap()) return {};
    const YAML::Node entry = exports[mode];
    if (!entry || !entry.IsMap()) return {};
    if (!entry["targetDir"]) return {};

    // The editor regenerates this file, so a value that will not convert falls back
    // to the default instead of throwing out of whichever dialog asked
    try {
        return std::filesystem::path(entry["targetDir"].as<std::string>());
    } catch (const std::exception& e) {
        Out::warning("Ignoring an export directory in \"%s\": %s", localSettingsFile(projectFile).string().c_str(), e.what());
        return {};
    }
}

bool AppSettings::setExportTargetDir(const std::filesystem::path& projectFile, const std::string& mode, const std::filesystem::path& targetDir) {
    if (getExportTargetDir(projectFile, mode) == targetDir) return true;

    YAML::Node local = loadLocalSettings(projectFile);
    if (!targetDir.empty()) {
        YAML::Node entry(YAML::NodeType::Map);
        entry["targetDir"] = targetDir.generic_string();
        local["exports"][mode] = entry;
    } else if (local["exports"]) {
        local["exports"].remove(mode);
        if (!local["exports"].size()) local.remove("exports");
    }
    return storeLocalSettings(projectFile, local);
}

LocalBuildSettings AppSettings::getBuildSettings(const std::filesystem::path& projectFile) {
    LocalBuildSettings result;
    YAML::Node local = loadLocalSettings(projectFile);
    const YAML::Node entry = migrateLegacySection(projectFile, local, settingsData, "project_builds", "build");
    if (!entry || !entry.IsMap()) return result;

    try {
        if (entry["c_compiler"]) result.cCompiler = entry["c_compiler"].as<std::string>();
        if (entry["cxx_compiler"]) result.cxxCompiler = entry["cxx_compiler"].as<std::string>();
        if (entry["generator"]) result.generator = entry["generator"].as<std::string>();
        if (entry["build_jobs"]) result.buildJobs = entry["build_jobs"].as<unsigned int>();
        result.configured = true;
    } catch (const std::exception& e) {
        // A value that will not convert falls back to the default toolchain
        Out::warning("Ignoring a build setting in \"%s\": %s", localSettingsFile(projectFile).string().c_str(), e.what());
        return LocalBuildSettings();
    }

    return result;
}

bool AppSettings::setBuildSettings(const std::filesystem::path& projectFile, const LocalBuildSettings& value) {
    const auto previous = getBuildSettings(projectFile);
    if (previous.configured && previous.cCompiler == value.cCompiler
        && previous.cxxCompiler == value.cxxCompiler && previous.generator == value.generator
        && previous.buildJobs == value.buildJobs) {
        return true;
    }

    // Written even when empty: that entry pins the project to the default toolchain
    YAML::Node entry(YAML::NodeType::Map);
    if (!value.cCompiler.empty()) entry["c_compiler"] = value.cCompiler;
    if (!value.cxxCompiler.empty()) entry["cxx_compiler"] = value.cxxCompiler;
    if (!value.generator.empty()) entry["generator"] = value.generator;
    if (value.buildJobs) entry["build_jobs"] = value.buildJobs;

    YAML::Node local = loadLocalSettings(projectFile);
    local["build"] = entry;
    return storeLocalSettings(projectFile, local);
}

} // namespace quantum::editor
