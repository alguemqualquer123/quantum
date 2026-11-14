// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include "ai/AiTypes.h"
#include "yaml-cpp/yaml.h"

namespace quantum::editor {

struct PanelVisibilitySettings {
    bool structure = true;
    bool properties = true;
    bool resources = true;
    bool output = true;
    bool animation = true;
    bool terrain = false;
    bool aiChat = true;
    bool notifications = true;
    bool resourceMonitor = false;
    bool graphEditor = false;
    bool terminal = false;
    bool materialBrowser = false;
    bool modelExport = false;
    bool pluginBrowser = false;
    bool worldCreator = false;

    bool operator==(const PanelVisibilitySettings& other) const {
        return structure == other.structure
            && properties == other.properties
            && resources == other.resources
            && output == other.output
            && animation == other.animation
            && terrain == other.terrain
            && aiChat == other.aiChat
            && notifications == other.notifications
            && resourceMonitor == other.resourceMonitor
            && graphEditor == other.graphEditor
            && terminal == other.terminal
            && materialBrowser == other.materialBrowser
            && modelExport == other.modelExport
            && pluginBrowser == other.pluginBrowser
            && worldCreator == other.worldCreator;    }

    bool operator!=(const PanelVisibilitySettings& other) const {
        return !(*this == other);
    }
};

// Compiler paths and job count are machine-specific, so they live in the project's
// own .quantum/user/build.yaml rather than in project.yaml, and travel with the
// project when it moves.
struct LocalBuildSettings {
    std::string cCompiler;
    std::string cxxCompiler;
    std::string generator;
    unsigned int buildJobs = 0; // 0 = automatic
    bool configured = false;    // set on read: false = no entry, so default toolchain
};

class AppSettings {
private:
    static std::filesystem::path configFilePath;
    static YAML::Node settingsData;
    
    // Recent projects list (max 10)
    static std::vector<std::filesystem::path> recentProjects;
    static std::filesystem::path lastProjectPath;

    // Last compiler kit chosen in Editor Settings, so new projects inherit it
    // instead of silently defaulting each time.
    static std::string lastCMakeCCompiler;
    static std::string lastCMakeCxxCompiler;
    static std::string lastCMakeGenerator;

    // Emscripten SDK root override for Web exports ("" = auto-detect).
    // Machine-specific, so stored editor-wide rather than per-project.
    static std::string emsdkPath;

    // cmake executable override ("" = look it up on PATH). Machine-specific,
    // stored editor-wide like emsdkPath.
    static std::string cmakePath;
    static std::filesystem::path defaultExportDirectory;

    // Shell the Terminal panel keeps alive ("" = the platform default). Which shell a
    // machine has is machine-specific, so it is editor-wide like cmakePath.
    static std::string terminalShell;
    
    // Window settings. The size is physical pixels, only meaningful again at the
    // scale it was captured at; 0 marks a file from before that was tracked.
    static int windowWidth;
    static int windowHeight;
    static bool isMaximized;
    static float windowUiScale;

    // The dock layout is pixels too, but keeps the scale it was built at even if
    // the window later moves to a monitor that changes windowUiScale.
    static float layoutUiScale;

    // Resources window settings
    static int resourcesIconSize;
    static int resourcesLayout;      // 0=AUTO, 1=GRID, 2=SPLIT_FILES_ONLY, 3=SPLIT
    static int resourcesItemViewStyle; // 0=CARD, 1=CLASSIC
    static float resourcesLeftPanelWidth;

    // Code editor settings
    static float codeEditorFontSize;

    // Editor viewport settings
    static bool multiViewportEnabled;
    static bool editorVSyncEnabled;
    static float uiScale;
    // How far the scene viewport draws while editing; 0 follows the scene's own render
    // distance. Editor-only, so it never reaches the scene file or a build.
    static float viewportRenderDistance;
    static PanelVisibilitySettings panelVisibility;

    // Theme preset (0=gray, 1=light, 2=dark) and the accent color as "#RRGGBB" (or
    // "#RRGGBBAA"). Empty accent keeps whatever Theme's preset already picked.
    static int themePreset;
    static std::string accentColor;

    // Extension of the marker file Explorer binds a project to ("" = ".dproj"). The
    // association is per-user and machine-wide in effect, so it belongs to the editor
    // rather than to one project.
    static std::string projectFileExtension;

    // Remapped keyboard shortcuts: KeyBindings id -> chord text ("Ctrl+Shift+S"). An
    // empty value means the user unbound that command; a missing key means its default.
    // Which keys a hand wants is a habit, not a property of a project, so it is stored
    // editor-wide like the theme.
    static std::map<std::string, std::string> shortcutOverrides;

    // Plugin windows the user left open, by registry id rather than by title: two plugins may name a
    // window the same and the id is what PluginRegistry keys on. Which plugins are even installed
    // changes between runs, so an id here that nothing registers is kept rather than dropped - a
    // project's own plugins only load on the next editor start.
    static std::vector<std::string> openPluginWindows;

    // AI assistant settings (API keys are intentionally not stored here)
    static ai::Settings aiSettings;
    static ai::McpSettings mcpSettings;

    // A marker survives an abnormal termination. The next boot can avoid
    // restoring the project and dock layout that were active when the process
    // died, without deleting either user's file.
    static bool previousSessionUnclean;
    static bool sessionMarkerWritten;

    // Private methods
    static void ensureConfigDirectory();

public:
    static std::filesystem::path getExportTargetDir(const std::filesystem::path& projectFile, const std::string& mode);
    static bool setExportTargetDir(const std::filesystem::path& projectFile, const std::string& mode, const std::filesystem::path& targetDir);

    static LocalBuildSettings getBuildSettings(const std::filesystem::path& projectFile);
    static bool setBuildSettings(const std::filesystem::path& projectFile, const LocalBuildSettings& value);

    // Initialization
    static bool initialize();
    static bool wasPreviousSessionUnclean();
    static bool beginSession();
    static void endSession();

    // Directory where editor config files live (settings.yaml, layout ini, ...)
    static std::filesystem::path getConfigDirectory();

    // Project settings
    static std::filesystem::path getLastProjectPath();
    static void setLastProjectPath(const std::filesystem::path& path);
    
    static std::vector<std::filesystem::path> getRecentProjects();
    static void addToRecentProjects(const std::filesystem::path& path, bool needSave = true);
    static void clearRecentProjects();

    // Last compiler kit (empty strings = Default toolchain)
    static std::string getLastCMakeCCompiler();
    static std::string getLastCMakeCxxCompiler();
    static std::string getLastCMakeGenerator();
    static void setLastCMakeKit(const std::string& cCompiler, const std::string& cxxCompiler, const std::string& generator);

    // Emscripten SDK root override for Web exports ("" = auto-detect)
    static std::string getEmsdkPath();
    static void setEmsdkPath(const std::string& path);

    // cmake executable override ("" = look it up on PATH)
    static std::string getCMakePath();
    static void setCMakePath(const std::string& path);

    // Shell for the Terminal panel ("" = platform default); persist on set.
    static std::string getTerminalShell();
    static void setTerminalShell(const std::string& shell);

    // Project marker extension ("" = the default). Always read through
    // ProjectFile::normalizeExtension, which turns "" into the default form.
    static std::string getProjectFileExtension();
    static void setProjectFileExtension(const std::string& extension);

    static std::filesystem::path getDefaultExportDirectory();
    static void setDefaultExportDirectory(const std::filesystem::path& path);
    
    // Window settings
    static int getWindowWidth();
    static int getWindowHeight();
    static bool getIsMaximized();
    
    static float getWindowUiScale();
    static float getLayoutUiScale();

    static void setWindowWidth(int width);
    static void setWindowHeight(int height);
    static void setIsMaximized(bool maximized);
    static void setWindowUiScale(float scale);
    static void setLayoutUiScale(float scale);
    
    // Resources window settings
    static int getResourcesIconSize();
    static int getResourcesLayout();
    static int getResourcesItemViewStyle();
    static float getResourcesLeftPanelWidth();
    
    static void setResourcesIconSize(int size);
    static void setResourcesLayout(int layout);
    static void setResourcesItemViewStyle(int style);
    static void setResourcesLeftPanelWidth(float width);

    // Code editor settings
    static constexpr float defaultCodeEditorFontSize = 14.0f; // VSCode default
    static constexpr float minCodeEditorFontSize = 8.0f;
    static constexpr float maxCodeEditorFontSize = 40.0f;

    static float getCodeEditorFontSize();
    static void setCodeEditorFontSize(float size);

    // Multi-viewport: allow dockable windows to be dragged out into their own OS window
    static bool getMultiViewportEnabled();
    static void setMultiViewportEnabled(bool enabled);

    // VSync for the editor's own frames; Play mode and builds use the project setting
    static bool getEditorVSyncEnabled();
    static void setEditorVSyncEnabled(bool enabled);

    // Multiplies the display scale the system reports
    static float getUiScale();
    static void setUiScale(float scale);

    // Render-distance cap of the scene viewport while editing (0 = follow the scene). Play
    // mode ignores it: what you tune is the editor's view, not what the game ships.
    static float getViewportRenderDistance();
    static void setViewportRenderDistance(float metres);

    // Panels toggled from the View menu
    static PanelVisibilitySettings getPanelVisibility();
    static void setPanelVisibility(const PanelVisibilitySettings& visibility);

    // Plugin windows left open, by registry id. Persist on set, like the panels above.
    static std::vector<std::string> getOpenPluginWindows();
    static void setOpenPluginWindows(const std::vector<std::string>& windowIds);

    // Theme preset (0=dark, 1=light) and accent color ("#RRGGBB"); persist on set.
    static int getThemePreset();
    static void setThemePreset(int preset);
    static std::string getAccentColor();
    static void setAccentColor(const std::string& color);

    // Keyboard shortcuts (empty chord text = deliberately unbound)
    static std::map<std::string, std::string> getShortcutOverrides();
    static void setShortcutOverride(const std::string& id, const std::string& chords);
    static void removeShortcutOverride(const std::string& id);
    static void clearShortcutOverrides();

    // AI assistant settings
    static ai::Settings getAiSettings();
    static void setAiSettings(const ai::Settings& settings);
    static ai::McpSettings getMcpSettings();
    static void setMcpSettings(const ai::McpSettings& settings);

    // Load and save settings
    static bool loadSettings();
    static bool saveSettings();
};

} // namespace quantum::editor
