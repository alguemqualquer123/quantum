// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "App.h"
#include "UILabHost.h"

#include "imgui_internal.h"
#include "Platform.h"
#include "Backend.h"
#include "Engine.h"
#include "PluginManager.h"
#include "render/SystemRender.h"
#include "util/FrameCapture.h"

#include "external/IconsFontAwesome6.h"
#include "command/CommandHandle.h"
#include "command/type/DeleteEntityCmd.h"
#include "command/type/DuplicateEntityCmd.h"
#include "command/type/RemoveChildSceneCmd.h"

#include "util/CrashHandler.h"
#include "util/FileDialogs.h"
#include "util/FileUtils.h"
#include "util/ProjectUtils.h"
#include "util/ShellAssociation.h"
#include "util/Util.h"
#include "util/FileUtils.h"
#include "util/SystemStats.h"
#include "cli/LaunchOptions.h"
#include "cli/ProjectDoctor.h"
#include "cli/ProjectFileType.h"
#include "cli/ResourceServerClient.h"

#include "window/NotificationsWindow.h"
#include "window/CommandPaletteWindow.h"
#include "window/WelcomeScreenWindow.h"
#include "window/ResourceMonitorWindow.h"
#include "window/TerminalWindow.h"
#include "window/ModelExportWindow.h"
#include "window/PluginBrowserWindow.h"
#include "window/PluginWindowHost.h"
#include "window/WorldCreatorWindow.h"
#include "window/MaterialBrowserWindow.h"
#include "graph/GraphEditorWindow.h"

#include "EditorVersion.h"
#include "Out.h"
#include "AppMenuCommand.h"
#include "AppSettings.h"
#include "KeyBindings.h"
#include "Theme.h"
#include "ai/McpServer.h"
#include "resources/fonts/fa-solid-900_ttf.h"
#include "resources/fonts/jetbrains-mono-regular_ttf.h"
//#include "recources/fonts/roboto-v20-latin-regular_ttf.h"
#include "util/DefaultFont.h"
#include "util/DefaultFontArabic.h"

#include "shader/ShaderBuilder.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/UISystem.h"

#include <filesystem>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <limits>
#include <utility>
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cstdint>
#include <cstring>
#include <cstdio>

#if defined(_WIN32)
  #include <windows.h>
  #include <shlobj.h>
#endif

using namespace quantum;
// The command ids live in a header now because KeyBindings has to name them too.
using editor::AppMenuCommand;

namespace {

// A scale no monitor reports means the value is missing or corrupt.
float sanitizeUiScale(float scale) {
    return (scale >= 0.5f && scale <= 8.0f) ? scale : 1.0f;
}

// Saved sizes are physical pixels, so they only mean the same window again at
// the scale they were captured at. Settings from before the editor tracked DPI
// carry no scale and read as 1.0, which is what a DPI-unaware process reported.
int scaleSavedWindowSize(int saved, float uiScale) {
    const float savedScale = sanitizeUiScale(editor::AppSettings::getWindowUiScale());
    const float ratio = sanitizeUiScale(uiScale) / savedScale;
    return std::max(1, static_cast<int>(std::lround(saved * ratio)));
}

// The dockspace lives on the main viewport, so that is the scale it has to match.
float mainViewportScale() {
    return sanitizeUiScale(ImGui::GetMainViewport()->DpiScale) * editor::Theme::uiScale();
}

// DPI times UI scale, so a wider range than sanitizeUiScale().
float sanitizeLayoutScale(float scale) {
    return (scale >= 0.25f && scale <= 24.0f) ? scale : 1.0f;
}

// Share of the window a panel beside the scene may take.
constexpr float sidePanelMaxShare = 0.3f;
constexpr float bottomPanelMaxShare = 0.4f;

// HasCentralNodeChild is not set yet when a restored layout is rescaled.
bool holdsCentralNode(const ImGuiDockNode* node) {
    if (!node) return false;
    if (node->LocalFlags & ImGuiDockNodeFlags_CentralNode) return true;
    return holdsCentralNode(node->ChildNodes[0]) || holdsCentralNode(node->ChildNodes[1]);
}

// App.h is frozen for these changes, so the newer panels are not members; a
// single editor instance means these TU-local pointers are the whole lifetime.
editor::NotificationsWindow* g_notificationsWindow = nullptr;
editor::CommandPaletteWindow* g_commandPaletteWindow = nullptr;
editor::WelcomeScreenWindow* g_welcomeScreenWindow = nullptr;
editor::ResourceMonitorWindow* g_resourceMonitorWindow = nullptr;
editor::GraphEditorWindow* g_graphEditorWindow = nullptr;
editor::TerminalWindow* g_terminalWindow = nullptr;
editor::ModelExportWindow* g_modelExportWindow = nullptr;
editor::PluginBrowserWindow* g_pluginBrowserWindow = nullptr;
editor::PluginWindowHost* g_pluginWindowHost = nullptr;
editor::WorldCreatorWindow* g_worldCreatorWindow = nullptr;

// Whether the project extension currently opens files with this build. Asking costs a
// registry read and the menu model is rebuilt every frame, so the answer is cached and
// refreshed when a project loads and whenever the command runs.
bool g_projectFileTypeRegistered = false;

}

ImFont* editor::App::codeFont = nullptr;

editor::App::App(){
    mainThreadId = std::this_thread::get_id();
    propertiesWindow = new Properties(&project);
    outputWindow = new OutputWindow();
    sceneWindow = new SceneWindow(&project);
    propertiesWindow->setSceneWindow(sceneWindow);
    structureWindow = new Structure(&project, sceneWindow);
    codeEditor = new CodeEditor(&project, &gitMonitor);
    imageViewerWindow = new ImageViewerWindow(&project);
    modelViewerWindow = new ModelViewerWindow(&project);
    materialBrowserWindow = new MaterialBrowserWindow(&project);
    resourcesWindow = new ResourcesWindow(&project, codeEditor, imageViewerWindow);
    loadingWindow = new LoadingWindow();
    animationWindow = new AnimationWindow(&project);
    terrainEditWindow = new TerrainEditWindow(&project);
    animatorWindow = new AnimatorWindow(&project);
    aiChatWindow = new AiChatWindow(&project, resourcesWindow);
    mcpServer = new ai::McpServer(&project, resourcesWindow);

    g_notificationsWindow = new NotificationsWindow();
    g_commandPaletteWindow = new CommandPaletteWindow();
    g_welcomeScreenWindow = new WelcomeScreenWindow();
    g_resourceMonitorWindow = new ResourceMonitorWindow();
    g_graphEditorWindow = new GraphEditorWindow();
    g_terminalWindow = new TerminalWindow(&project);
    g_modelExportWindow = new ModelExportWindow(&project);
    g_pluginBrowserWindow = new PluginBrowserWindow();
    g_pluginWindowHost = new PluginWindowHost();
    g_worldCreatorWindow = new WorldCreatorWindow(&project);
    setGraphEditorWindow(g_graphEditorWindow);

    isInitialized = false;
    dockspaceNeedsRebuild = false;

    lastFocusedWindow = LastFocusedWindow::None;

    Out::setOutputWindow(outputWindow);

    isDroppedExternalPaths = false;

    resetLastActivatedScene();
}

void editor::App::saveFunc(){
    if (saveDialogInProgress) return;
    if (project.isTempProject()) {
        saveAllFunc();
        return;
    }
    if (lastFocusedWindow == LastFocusedWindow::Code) {
        if (!codeEditor->saveLastFocused()) {
            registerAlert("Save Failed", "The script could not be saved. Check its file permissions and try again.");
        }
    }else if (!project.isAnyScenePlaying()) {
        project.saveLastSelectedScene();
    }
}

void editor::App::saveAllFunc(std::function<void(bool)> callback){
    if (project.isAnyScenePlaying() || saveDialogInProgress) {
        if (callback) callback(false);
        return;
    }
    codeEditor->saveAll();
    if (codeEditor->hasUnsavedChanges()) {
        registerAlert("Save Failed", "Some scripts could not be saved. Check their file permissions and try again.");
        if (callback) callback(false);
        return;
    }
    project.saveAllScenes([this, callback](bool success) {
        if (!success) {
            if (callback) callback(false);
            return;
        }
        // saveAllScenes() skips untouched scenes with no file, like a new project's first one
        if (project.isTempProject()) {
            registerProjectSaveDialog([this, callback](bool saved) {
                if (saved) {
                    project.saveScenesWithoutFile(callback);
                } else if (callback) {
                    callback(false);
                }
            });
        } else if (project.saveProject(true)) {
            project.saveScenesWithoutFile(callback);
        } else {
            registerAlert("Save Failed", "The project could not be saved. Check its directory and try again.");
            if (callback) callback(false);
        }
    });
}

void editor::App::saveAllAndProject(std::function<void()> onSuccess){
    saveAllFunc([onSuccess](bool success) {
        if (success && onSuccess) onSuccess();
    });
}

void editor::App::openProjectFunc(){
    // Reached from the welcome screen (mid-pass) and from Ctrl+O; the project
    // swap behind it must not run while the pass records draw data.
    if (uiPassActive) {
        runWhenUiIdle([this]() { openProjectFunc(); });
        return;
    }
    if (project.isAnyScenePlaying()) {
        registerAlert("Scene Running", "A scene is currently running or stopping. Stop it before opening another project.");
        return;
    }

    auto openProject = [this]() {
        #ifdef _WIN32
        const wchar_t* homeDir = _wgetenv(L"USERPROFILE");
        #else
        const char* homeDir = std::getenv("HOME");
        #endif
        const std::string selectedDir = FileDialogs::openFileDialog(
            homeDir ? FileUtils::pathToUtf8(std::filesystem::path(homeDir)) : "", FILE_DIALOG_ALL, true);
        if (selectedDir.empty()) return;

        const std::filesystem::path projectDir = FileUtils::pathFromUtf8(selectedDir);
        if (!std::filesystem::exists(projectDir / "project.yaml")) {
            registerAlert("Error", "The selected directory is not a valid project. No project.yaml file found!");
            return;
        }

        // The welcome overlay only exists while there is nothing to edit, so a
        // completed pick dismisses it; cancelling the picker leaves it in place.
        requestProjectChange([this, projectDir]() {
            if (project.loadProject(projectDir))
                g_welcomeScreenWindow->setOpen(false);
        });
    };

    if (project.hasScenesUnsavedChanges() || codeEditor->hasUnsavedChanges() || project.isTempUnsavedProject()) {
        registerConfirmAlert(
            "Unsaved Changes",
            "There are unsaved changes. Do you want to save them before opening another project?",
            [this, openProject]() { saveAllAndProject(openProject); },
            openProject);
    } else {
        openProject();
    }
}

namespace {

editor::PlatformMenuItem menuCommand(AppMenuCommand command,
                                     std::string label,
                                     bool enabled = true) {
    editor::PlatformMenuItem item;
    item.type = editor::PlatformMenuItemType::Command;
    item.label = std::move(label);
    item.command.id = static_cast<uint32_t>(command);
    item.enabled = enabled;
    if (command == AppMenuCommand::About) item.role = editor::PlatformMenuRole::About;
    if (command == AppMenuCommand::EditorSettings) item.role = editor::PlatformMenuRole::Settings;
    if (command == AppMenuCommand::Exit) item.role = editor::PlatformMenuRole::Quit;
    return item;
}

// The shortcut column is never written by hand: it mirrors the binding the dispatcher
// actually reads, so remapping a key in Editor Settings lands in the menu, the palette
// and the input pass at once. `binding` names a KeyBindings row, which is not always the
// command's own (Resume Scene displays Run Scene's F5).
editor::PlatformMenuItem menuShortcut(AppMenuCommand command,
                                      std::string label,
                                      const char* binding,
                                      bool enabled = true) {
    editor::PlatformMenuItem item = menuCommand(command, std::move(label), enabled);
    item.shortcut = editor::KeyBindings::primaryText(binding);
    return item;
}

editor::PlatformMenuItem menuToggle(AppMenuCommand command,
                                    std::string label,
                                    bool checked) {
    editor::PlatformMenuItem item = menuCommand(command, std::move(label));
    item.checked = checked;
    return item;
}

editor::PlatformMenuItem menuToggleShortcut(AppMenuCommand command,
                                            std::string label,
                                            const char* binding,
                                            bool checked) {
    editor::PlatformMenuItem item = menuToggle(command, std::move(label), checked);
    item.shortcut = editor::KeyBindings::primaryText(binding);
    return item;
}

editor::PlatformMenuItem menuPayloadCommand(AppMenuCommand command,
                                            std::string label,
                                            std::string payload) {
    editor::PlatformMenuItem item = menuCommand(command, std::move(label));
    item.command.payload = std::move(payload);
    return item;
}

editor::PlatformMenuItem menuSeparator() {
    editor::PlatformMenuItem item;
    item.type = editor::PlatformMenuItemType::Separator;
    return item;
}

editor::PlatformMenuItem menuSubmenu(std::string label,
                                     std::vector<editor::PlatformMenuItem> children,
                                     bool enabled = true) {
    editor::PlatformMenuItem item;
    item.type = editor::PlatformMenuItemType::Submenu;
    item.label = std::move(label);
    item.enabled = enabled;
    item.children = std::move(children);
    return item;
}

// One item per sample project shipped next to the engine, so creating a project
// from a template needs no extra dialog: picking the item copies the template
// over the fresh temp project and loads it.
std::vector<editor::PlatformMenuItem> buildTemplateMenuItems() {
    auto unavailable = []() {
        std::vector<editor::PlatformMenuItem> items;
        items.push_back(menuCommand(AppMenuCommand::NewProjectFromTemplate,
                                    "No templates installed", false));
        return items;
    };

    const std::filesystem::path root = editor::FileUtils::getTemplatesDir();
    std::error_code ec;
    if (root.empty() || !std::filesystem::is_directory(root, ec)) {
        return unavailable();
    }

    std::vector<std::string> names;
    const std::filesystem::directory_iterator end;
    for (std::filesystem::directory_iterator it(root, ec); !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec) && std::filesystem::exists(it->path() / "project.yaml", ec)) {
            names.push_back(it->path().filename().string());
        }
    }
    if (names.empty()) {
        return unavailable();
    }

    std::sort(names.begin(), names.end());
    std::vector<editor::PlatformMenuItem> items;
    for (const std::string& name : names) {
        items.push_back(menuPayloadCommand(AppMenuCommand::NewProjectFromTemplate, name, name));
    }
    return items;
}

editor::PaletteCommand paletteCmd(AppMenuCommand command,
                                  std::string label,
                                  const char* binding = nullptr,
                                  std::string description = {}) {
    editor::PaletteCommand command_;
    command_.id = std::to_string(static_cast<uint32_t>(command));
    command_.label = std::move(label);
    command_.shortcut = binding ? editor::KeyBindings::primaryText(binding) : std::string();
    command_.description = std::move(description);
    return command_;
}

// Same command set the menus expose, so a palette entry behaves exactly like the
// menu item it mirrors (the ids round-trip through the AppMenuCommand switch).
std::vector<editor::PaletteCommand> buildPaletteCommands() {
    return {
        paletteCmd(AppMenuCommand::NewProject, "File: New Project"),
        paletteCmd(AppMenuCommand::ToggleWelcome, "File: Welcome Screen"),
        paletteCmd(AppMenuCommand::NewScene3D, "File: New 3D Scene"),
        paletteCmd(AppMenuCommand::NewScene2D, "File: New 2D Scene"),
        paletteCmd(AppMenuCommand::NewSceneUI, "File: New UI Scene"),
        paletteCmd(AppMenuCommand::OpenProject, "File: Open Project...", "file.openProject"),
        paletteCmd(AppMenuCommand::SaveProject, "File: Save Project"),
        paletteCmd(AppMenuCommand::SaveProjectAs, "File: Save Project As..."),
        paletteCmd(AppMenuCommand::Save, "File: Save", "file.save"),
        paletteCmd(AppMenuCommand::SaveAll, "File: Save All", "file.saveAll"),
        paletteCmd(AppMenuCommand::ExportProject, "File: Export Project..."),
        paletteCmd(AppMenuCommand::Undo, "Edit: Undo", "edit.undo"),
        paletteCmd(AppMenuCommand::Redo, "Edit: Redo", "edit.redo"),
        paletteCmd(AppMenuCommand::ToggleCodeComment, "Edit: Toggle Comment", "code.comment"),
        paletteCmd(AppMenuCommand::ToggleCodeBlockComment, "Edit: Toggle Block Comment", "code.blockComment"),
        paletteCmd(AppMenuCommand::FormatCodeDocument, "Edit: Format Document", "code.format"),
        paletteCmd(AppMenuCommand::SettingsShortcutsTab, "Edit: Keyboard Shortcuts Settings", "settings.shortcuts"),
        paletteCmd(AppMenuCommand::SettingsGraphicsTab, "Edit: Graphics Settings", "settings.graphics"),
        paletteCmd(AppMenuCommand::SettingsEngineApiTab, "Edit: Engine API Reference", "settings.api"),
        paletteCmd(AppMenuCommand::ToggleStructure, "View: Toggle Structure"),
        paletteCmd(AppMenuCommand::ToggleProperties, "View: Toggle Properties"),
        paletteCmd(AppMenuCommand::ToggleResources, "View: Toggle Resources"),
        paletteCmd(AppMenuCommand::ToggleOutput, "View: Toggle Output"),
        paletteCmd(AppMenuCommand::ToggleAnimation, "View: Toggle Animation"),
        paletteCmd(AppMenuCommand::ToggleTerrain, "View: Toggle Terrain"),
        paletteCmd(AppMenuCommand::ToggleAnimator, "View: Toggle Animator"),
        paletteCmd(AppMenuCommand::ToggleAiChat, "View: Toggle AI Chat"),
        paletteCmd(AppMenuCommand::ToggleNotifications, "View: Toggle Notifications"),
        paletteCmd(AppMenuCommand::ToggleResourceMonitor, "View: Toggle Resource Monitor"),
        paletteCmd(AppMenuCommand::ToggleGraphEditor, "View: Toggle Graph Editor"),
        paletteCmd(AppMenuCommand::ToggleTerminal, "View: Toggle Terminal", "view.terminal"),
        paletteCmd(AppMenuCommand::ToggleMaterialBrowser, "View: Toggle Material Library"),
        paletteCmd(AppMenuCommand::ToggleModelExport, "View: Toggle Model Export"),
        paletteCmd(AppMenuCommand::TogglePlugins, "View: Toggle Plugins"),
        paletteCmd(AppMenuCommand::ToggleWorldCreator, "View: Toggle World Creator"),
        paletteCmd(AppMenuCommand::TogglePhysicsDebugDraw, "View: Toggle Physics Debug Draw"),
        paletteCmd(AppMenuCommand::ResetLayout, "View: Reset Layout"),
        paletteCmd(AppMenuCommand::ThemeGray, "View: Theme: Gray"),
        paletteCmd(AppMenuCommand::ThemeLight, "View: Theme: Light"),
        paletteCmd(AppMenuCommand::ThemeDark, "View: Theme: Dark"),
        paletteCmd(AppMenuCommand::ProjectSettings, "Project: Project Settings..."),
        paletteCmd(AppMenuCommand::ProjectScenes, "Project: Scenes..."),
        paletteCmd(AppMenuCommand::ProjectBundles, "Project: Bundles..."),
        paletteCmd(AppMenuCommand::ValidateProject, "Project: Validate Project"),
        paletteCmd(AppMenuCommand::ClearTrash, "Project: Clear Trash"),
        paletteCmd(AppMenuCommand::ClearShaderCache, "Project: Clear Shader Cache"),
        paletteCmd(AppMenuCommand::ToggleProjectFileType, "Project: Register Project File Type"),
        paletteCmd(AppMenuCommand::RunScene, "Scene: Run", "scene.run"),
        paletteCmd(AppMenuCommand::NetworkPlay, "Scene: Network Play..."),
        paletteCmd(AppMenuCommand::ReloadCppScripts, "Scene: Reload C++ Scripts"),
        paletteCmd(AppMenuCommand::ToggleScriptDebugger, "View: Toggle Script Debugger"),
        paletteCmd(AppMenuCommand::PauseScene, "Scene: Pause", "scene.pause"),
        paletteCmd(AppMenuCommand::StopScene, "Scene: Stop", "scene.stop"),
        paletteCmd(AppMenuCommand::RemoveScene, "Scene: Remove"),
        paletteCmd(AppMenuCommand::About, "Help: About Quantum"),
    };
}

const char* accentForCommand(AppMenuCommand command) {
    switch (command) {
        case AppMenuCommand::AccentBlue: return "#4B79F6";
        case AppMenuCommand::AccentTeal: return "#17A398";
        case AppMenuCommand::AccentAmber: return "#D9911F";
        case AppMenuCommand::AccentViolet: return "#8A5CC8";
        default: return nullptr;
    }
}

editor::Theme::Preset presetFromSetting(int setting) {
    switch (setting) {
        case 1: return editor::Theme::Preset::Light;
        case 2: return editor::Theme::Preset::Dark;
        default: return editor::Theme::Preset::Gray;
    }
}

int settingFromPreset(editor::Theme::Preset preset) {
    switch (preset) {
        case editor::Theme::Preset::Light: return 1;
        case editor::Theme::Preset::Dark: return 2;
        default: return 0;
    }
}

const char* imguiMenuLabel(const editor::PlatformMenuItem& item) {
    switch (static_cast<AppMenuCommand>(item.command.id)) {
        case AppMenuCommand::NewScene3D: return ICON_FA_CUBES "  3D Scene";
        case AppMenuCommand::NewScene2D: return ICON_FA_CUBES_STACKED "  2D Scene";
        case AppMenuCommand::NewSceneUI: return ICON_FA_WINDOW_RESTORE "  UI Scene";
        default: return item.label.c_str();
    }
}

// Help > Keyboard Shortcuts is generated from the registry, so the list cannot disagree
// with a remap. The scene-view keys stay hand-written: the viewport reads them itself and
// they are not configurable.
std::string buildShortcutHelp() {
    std::string text = "REMAPPABLE IN EDITOR SETTINGS > SHORTCUTS\n";
    const char* category = nullptr;
    for (const editor::KeyBinding& binding : editor::KeyBindings::all()) {
        if (!category || std::strcmp(category, binding.category) != 0) {
            category = binding.category;
            text += std::string("\n") + category + "\n";
        }
        // An empty chord column is a command the user unbound, which is worth showing.
        const std::string chords = editor::KeyBindings::text(binding.id);
        text += "    " + chords;
        for (size_t pad = chords.size(); pad < 18; ++pad) text += ' ';
        text += "  " + std::string(binding.label) + "\n";
    }
    text += "\nSCENE VIEW (NOT CONFIGURABLE)\n"
            "    W / E / R       Move / Rotate / Scale\n"
            "    T               Toggle local / global transforms\n"
            "    F / Home        Frame selection / all objects (3D)\n";
    return text;
}

} // namespace

editor::PlatformMenuModel editor::App::buildMenuModel(){
    SceneProject* selectedScene = project.getSelectedScene();
    uint32_t selectedSceneId = project.getSelectedSceneId();
    bool hasSelectedScene = selectedScene != nullptr;
    const bool physicsDebugDraw = hasSelectedScene && selectedScene->scene &&
        selectedScene->scene->getSystem<PhysicsSystem>()->isDebugDrawEnabled();
    bool isProjectBusy = project.isAnyScenePlaying();
    bool isPlaying = hasSelectedScene && selectedScene->playState == ScenePlayState::PLAYING;
    bool isPaused = hasSelectedScene && selectedScene->playState == ScenePlayState::PAUSED;
    bool isLoading = hasSelectedScene && selectedScene->playState == ScenePlayState::LOADING;
    bool isSaving = hasSelectedScene && selectedScene->playState == ScenePlayState::SAVING;
    bool canRun = hasSelectedScene && !isProjectBusy;
    bool canPause = hasSelectedScene && isPlaying;
    bool canResume = hasSelectedScene && isPaused;
    bool canStop = hasSelectedScene && !isSaving && (isPlaying || isPaused || isLoading);
    bool canDetachCamera = hasSelectedScene && (isPlaying || isPaused);
    bool canReloadCppScripts = project.canReloadActiveCppScripts();
    bool canRemove = hasSelectedScene && !isProjectBusy && project.getScenes().size() > 1;

    bool canSave = false;
    if (!saveDialogInProgress) {
        if (project.isTempProject()) {
            canSave = !isProjectBusy;
        } else if (lastFocusedWindow == LastFocusedWindow::Code) {
            canSave = codeEditor->hasLastFocusedUnsavedChanges();
        } else {
            canSave = !isProjectBusy && project.hasSelectedSceneUnsavedChanges();
        }
    }
    // Project settings are not tracked by the scene and script dirty flags.
    bool canSaveAll = !isProjectBusy && !saveDialogInProgress;

    bool canUndo = false;
    bool canRedo = false;
    if (!ImGui::GetIO().WantTextInput) {
        if (lastFocusedWindow == LastFocusedWindow::Resources) {
            canUndo = project.getProjectCommandHistory()->canUndo();
            canRedo = project.getProjectCommandHistory()->canRedo();
        } else if (lastFocusedWindow == LastFocusedWindow::Code) {
            canUndo = codeEditor->canUndoLastFocused();
            canRedo = codeEditor->canRedoLastFocused();
        } else if (lastFocusedWindow != LastFocusedWindow::AI && hasSelectedScene) {
            canUndo = CommandHandle::get(selectedSceneId)->canUndo();
            canRedo = CommandHandle::get(selectedSceneId)->canRedo();
        }
    }

    // The comment and format commands act on a script, so they are only live while one
    // holds the caret
    const bool canEditCode = lastFocusedWindow == LastFocusedWindow::Code &&
                             codeEditor->canEditLastFocused();

    std::vector<PlatformMenuItem> recentItems;
    std::vector<std::filesystem::path> recentProjects = AppSettings::getRecentProjects();
    if (recentProjects.empty()) {
        recentItems.push_back(menuCommand(AppMenuCommand::OpenRecentProject,
                                          "No Recent Projects", false));
    } else {
        for (const auto& path : recentProjects) {
            recentItems.push_back(menuPayloadCommand(
                AppMenuCommand::OpenRecentProject,
                path.filename().string() + " (" + path.string() + ")",
                path.string()));
        }
        recentItems.push_back(menuSeparator());
        recentItems.push_back(menuCommand(AppMenuCommand::ClearRecentProjects,
                                          "Clear Recent Projects"));
    }

    PlatformMenuModel menu;
    menu.menus.push_back(menuSubmenu("File", {
        menuCommand(AppMenuCommand::ToggleWelcome, "Welcome Screen"),
        menuCommand(AppMenuCommand::NewProject, "New Project", !isProjectBusy),
        menuSubmenu("New Project From Template", buildTemplateMenuItems(), !isProjectBusy),
        menuSubmenu("New Scene", {
            menuCommand(AppMenuCommand::NewScene3D, "3D Scene"),
            menuCommand(AppMenuCommand::NewScene2D, "2D Scene"),
            menuCommand(AppMenuCommand::NewSceneUI, "UI Scene")
        }, !isProjectBusy),
        menuSeparator(),
        menuShortcut(AppMenuCommand::OpenProject, "Open Project...", "file.openProject",
                     !isProjectBusy),
        menuSubmenu("Recent Projects", std::move(recentItems), !isProjectBusy),
        menuSeparator(),
        menuShortcut(AppMenuCommand::Save, "Save", "file.save", canSave),
        menuShortcut(AppMenuCommand::SaveAll, "Save All", "file.saveAll", canSaveAll),
        menuCommand(AppMenuCommand::SaveProject, "Save Project", canSaveAll),
        menuCommand(AppMenuCommand::SaveProjectAs, "Save Project As...", canSaveAll),
        menuSeparator(),
        menuCommand(AppMenuCommand::ExportProject, "Export Project..."),
        menuSeparator(),
        menuCommand(AppMenuCommand::Exit, "Exit")
    }));

    menu.menus.push_back(menuSubmenu("Edit", {
        menuShortcut(AppMenuCommand::Undo, "Undo", "edit.undo", canUndo),
        menuShortcut(AppMenuCommand::Redo, "Redo", "edit.redo", canRedo),
        menuSeparator(),
        menuShortcut(AppMenuCommand::DuplicateSelection, "Duplicate", "edit.duplicate", canEditSelection(true)),
        menuShortcut(AppMenuCommand::DeleteSelection, "Delete", "edit.delete", canEditSelection(false)),
        menuSeparator(),
        menuShortcut(AppMenuCommand::ToggleCodeComment, "Toggle Comment", "code.comment", canEditCode),
        menuShortcut(AppMenuCommand::ToggleCodeBlockComment, "Toggle Block Comment", "code.blockComment", canEditCode),
        menuShortcut(AppMenuCommand::FormatCodeDocument, "Format Document", "code.format", canEditCode),
        menuSeparator(),
        menuCommand(AppMenuCommand::EditorSettings, "Editor Settings..."),
        menuShortcut(AppMenuCommand::SettingsShortcutsTab, "Keyboard Shortcuts...", "settings.shortcuts"),
        menuShortcut(AppMenuCommand::SettingsGraphicsTab, "Graphics...", "settings.graphics"),
        menuShortcut(AppMenuCommand::SettingsEngineApiTab, "Engine API...", "settings.api")
    }));

    menu.menus.push_back(menuSubmenu("View", {
        menuToggle(AppMenuCommand::ToggleStructure, Structure::WINDOW_NAME,
                   structureWindow->isOpen()),
        menuToggle(AppMenuCommand::ToggleProperties, Properties::WINDOW_NAME,
                   propertiesWindow->isOpen()),
        menuToggle(AppMenuCommand::ToggleResources, ResourcesWindow::WINDOW_NAME,
                   resourcesWindow->isOpen()),
        menuToggle(AppMenuCommand::ToggleOutput, OutputWindow::WINDOW_NAME,
                   outputWindow->isOpen()),
        menuToggle(AppMenuCommand::ToggleAnimation, AnimationWindow::WINDOW_NAME,
                   animationWindow->isOpen()),
        menuToggle(AppMenuCommand::ToggleTerrain, TerrainEditWindow::WINDOW_NAME,
                   terrainEditWindow->isOpen()),
        menuToggle(AppMenuCommand::ToggleAnimator, AnimatorWindow::WINDOW_NAME,
                   animatorWindow->isOpen()),
        menuToggle(AppMenuCommand::ToggleAiChat, AiChatWindow::WINDOW_NAME,
                   aiChatWindow->isOpen()),
        menuToggle(AppMenuCommand::ToggleNotifications, NotificationsWindow::WINDOW_NAME,
                   g_notificationsWindow->isOpen()),
        menuToggle(AppMenuCommand::ToggleResourceMonitor, ResourceMonitorWindow::WINDOW_NAME,
                   g_resourceMonitorWindow->isOpen()),
        menuToggle(AppMenuCommand::ToggleGraphEditor, GraphEditorWindow::WINDOW_NAME,
                   g_graphEditorWindow->isOpen()),
        menuToggleShortcut(AppMenuCommand::ToggleTerminal, TerminalWindow::WINDOW_NAME, "view.terminal",
                           g_terminalWindow->isOpen()),
        menuToggle(AppMenuCommand::ToggleMaterialBrowser, MaterialBrowserWindow::WINDOW_NAME,
                   materialBrowserWindow->isOpen()),
        menuToggle(AppMenuCommand::ToggleModelExport, ModelExportWindow::WINDOW_NAME,
                   g_modelExportWindow->isOpen()),
        menuToggle(AppMenuCommand::TogglePlugins, PluginBrowserWindow::WINDOW_NAME,
                   g_pluginBrowserWindow->isOpen()),
        menuToggle(AppMenuCommand::ToggleWorldCreator, WorldCreatorWindow::WINDOW_NAME,
                   g_worldCreatorWindow->isOpen()),
        menuToggle(AppMenuCommand::ToggleScriptDebugger, ScriptDebuggerWindow::WINDOW_NAME,
                   scriptDebuggerWindow.isOpen()),
        menuToggle(AppMenuCommand::TogglePhysicsDebugDraw, "Physics Debug Draw", physicsDebugDraw),
        menuSeparator(),
        menuShortcut(AppMenuCommand::CommandPalette, "Command Palette...", "view.commandPalette"),
        menuSubmenu("Theme", {
            menuToggle(AppMenuCommand::ThemeGray, "Gray",
                       Theme::getPreset() == Theme::Preset::Gray),
            menuToggle(AppMenuCommand::ThemeLight, "Light",
                       Theme::getPreset() == Theme::Preset::Light),
            menuToggle(AppMenuCommand::ThemeDark, "Dark",
                       Theme::getPreset() == Theme::Preset::Dark)
        }),
        menuSubmenu("Accent Color", {
            menuCommand(AppMenuCommand::AccentBlue, "Blue"),
            menuCommand(AppMenuCommand::AccentTeal, "Teal"),
            menuCommand(AppMenuCommand::AccentAmber, "Amber"),
            menuCommand(AppMenuCommand::AccentViolet, "Violet")
        }),
        menuSeparator(),
        menuToggle(AppMenuCommand::ToggleDetachableWindows, "Detachable Windows",
                   AppSettings::getMultiViewportEnabled()),
        menuToggle(AppMenuCommand::ToggleEditorVSync, "Editor VSync",
                   AppSettings::getEditorVSyncEnabled()),
        menuSeparator(),
        menuCommand(AppMenuCommand::ResetLayout, "Reset Layout")
    }));

    std::vector<editor::PlatformMenuItem> projectMenu = {
        menuCommand(AppMenuCommand::ProjectSettings, "Project Settings..."),
        menuCommand(AppMenuCommand::ProjectScenes, "Manage Scenes...", !isProjectBusy),
        menuCommand(AppMenuCommand::ProjectBundles, "Bundles..."),
        menuCommand(AppMenuCommand::ValidateProject, "Validate Project", !isProjectBusy && !project.getProjectPath().empty()),
        menuSeparator(),
        menuCommand(AppMenuCommand::ClearTrash, "Empty Project Trash...", !isProjectBusy),
        menuCommand(AppMenuCommand::ClearShaderCache, "Clear Shader Cache")
    };
    // The label names the extension in play because it is configurable: ".dproj" is
    // also Delphi's, and a clash is resolved by picking another one.
    if (editor::ShellAssociation::isSupported()) {
        projectMenu.push_back(menuSeparator());
        projectMenu.push_back(menuToggle(AppMenuCommand::ToggleProjectFileType,
                                         "Register " + editor::cli::ProjectFileType::resolveExtension()
                                                   + " File Type",
                                         g_projectFileTypeRegistered));
    }
    menu.menus.push_back(menuSubmenu("Project", std::move(projectMenu)));

    PlatformMenuItem detachCameraItem = menuShortcut(AppMenuCommand::DetachCamera, "Detach Camera",
                                                     "scene.detachCamera", canDetachCamera);
    detachCameraItem.checked = canDetachCamera && selectedScene->sceneRender && selectedScene->sceneRender->isCameraDetached();

    menu.menus.push_back(menuSubmenu("Scene", {
        menuSubmenu("New Scene", {
            menuCommand(AppMenuCommand::NewScene3D, "3D Scene"),
            menuCommand(AppMenuCommand::NewScene2D, "2D Scene"),
            menuCommand(AppMenuCommand::NewSceneUI, "UI Scene")
        }, !isProjectBusy),
        menuSeparator(),
        menuShortcut(AppMenuCommand::RunScene, "Run Current Scene", "scene.run", canRun),
        menuCommand(AppMenuCommand::NetworkPlay, "Network Play...", canRun),
        menuCommand(AppMenuCommand::ReloadCppScripts, "Reload C++ Scripts", canReloadCppScripts),
        menuShortcut(AppMenuCommand::PauseScene, "Pause", "scene.pause", canPause),
        menuShortcut(AppMenuCommand::ResumeScene, "Resume", "scene.run", canResume),
        menuShortcut(AppMenuCommand::StopScene, "Stop", "scene.stop", canStop),
        detachCameraItem,
        menuSeparator(),
        menuCommand(AppMenuCommand::RemoveScene, "Remove Scene from Project", canRemove)
    }));

    // Built from the live registry, so a plugin that installs, unloads or registers a window while
    // the editor runs appears here on the next frame without a restart.
    std::vector<PlatformMenuItem> pluginEntries = pluginMenuEntries();
    menu.menus.push_back(menuSubmenu("Plugins", std::move(pluginEntries)));

    menu.menus.push_back(menuSubmenu("Help", {
        menuShortcut(AppMenuCommand::Documentation, "Documentation", "help.documentation"),
        menuCommand(AppMenuCommand::KeyboardShortcuts, "Keyboard Shortcuts"),
        menuCommand(AppMenuCommand::ReportIssue, "Report an Issue..."),
        menuSeparator(),
        menuCommand(AppMenuCommand::About, "About Quantum")
    }));
    return menu;
}

std::vector<editor::PlatformMenuItem> editor::App::pluginMenuEntries() {
    const plugin::PluginRegistry& registry = plugin::PluginManager::instance().registry();

    auto windowItem = [this](const plugin::PluginWindowInfo& window) {
        PlatformMenuItem entry = menuPayloadCommand(AppMenuCommand::TogglePluginWindow,
                                                   window.title, window.id);
        entry.checked = g_pluginWindowHost && g_pluginWindowHost->isWindowOpen(window.id);
        return entry;
    };

    auto itemEntry = [](const plugin::PluginMenuItemInfo& item) {
        return menuPayloadCommand(AppMenuCommand::RunPluginCommand, item.label, item.command);
    };

    // Menu items and windows share one grouping key: a plugin that files both under "World" expects
    // to find them together. An empty category is the plugin not caring, and those sit at the top of
    // the menu rather than in a submenu of their own.
    std::vector<std::string> categories;
    auto noteCategory = [&categories](const std::string& category) {
        if (category.empty()) return;
        if (std::find(categories.begin(), categories.end(), category) == categories.end()) {
            categories.push_back(category);
        }
    };
    for (const plugin::PluginMenuItemInfo& item : registry.menuItems()) noteCategory(item.category);
    for (const plugin::PluginWindowInfo& window : registry.windows()) noteCategory(window.category);

    std::vector<PlatformMenuItem> entries;
    for (const plugin::PluginMenuItemInfo& item : registry.menuItems()) {
        if (!item.category.empty()) continue;
        entries.push_back(itemEntry(item));
    }
    for (const plugin::PluginWindowInfo& window : registry.windows()) {
        if (!window.category.empty()) continue;
        entries.push_back(windowItem(window));
    }

    for (const std::string& category : categories) {
        std::vector<PlatformMenuItem> group;
        for (const plugin::PluginMenuItemInfo& item : registry.menuItems()) {
            if (item.category != category) continue;
            group.push_back(itemEntry(item));
        }
        for (const plugin::PluginWindowInfo& window : registry.windows()) {
            if (window.category != category) continue;
            group.push_back(windowItem(window));
        }
        entries.push_back(menuSubmenu(category, std::move(group)));
    }

    if (!registry.tools().empty()) {
        // registerTool() names an action instead of a callback, so a tool is a command by name. One
        // that names no registered command is shown disabled rather than hidden: the plugin's own
        // documentation promises the button, and a missing row gives the user nothing to ask about.
        std::vector<PlatformMenuItem> tools;
        for (const plugin::PluginToolInfo& tool : registry.tools()) {
            plugin::PluginCommandInfo target;
            PlatformMenuItem entry = menuPayloadCommand(AppMenuCommand::RunPluginCommand,
                                                        tool.label.empty() ? tool.name : tool.label,
                                                        tool.action);
            entry.enabled = registry.findCommand(tool.action, target);
            tools.push_back(std::move(entry));
        }
        entries.push_back(menuSeparator());
        entries.push_back(menuSubmenu("Plugin Tools", std::move(tools)));
    }

    if (entries.empty()) {
        // A disabled row with no command behind it, which is why PlatformMenuItem is built by hand
        // instead of through menuCommand: there is nothing to run, and 0 is not an AppMenuCommand.
        PlatformMenuItem none;
        none.label = "No plugin has registered anything yet";
        none.enabled = false;
        entries.push_back(std::move(none));
    }
    return entries;
}

void editor::App::executeMenuCommand(const PlatformMenuCommand& command){
    // The command palette, the welcome screen and confirm alerts fire while
    // the pass is recording draw data; a project swap there would delete
    // textures whose names are already in this pass's draw commands (see
    // uiPassActive). Do the work at the frame boundary instead.
    if (uiPassActive) {
        runWhenUiIdle([this, command]() { executeMenuCommand(command); });
        return;
    }
    if (projectLoading || pendingProjectChange) return;

    const auto action = static_cast<AppMenuCommand>(command.id);
    switch (action) {
        case AppMenuCommand::NewProject: {
            if (project.isAnyScenePlaying()) return;
            std::string projectName = "MyQuantumProject";
            auto startFreshProject = [this, projectName]() {
                requestProjectChange([this, projectName]() {
                    if (project.createTempProject(projectName, true)) aiChatWindow->startNewChat();
                });
            };
            if (project.hasScenesUnsavedChanges() || codeEditor->hasUnsavedChanges() ||
                project.isTempUnsavedProject()) {
                registerConfirmAlert(
                    "Unsaved Changes",
                    "There are unsaved changes. Do you want to save them before creating a new project?",
                    [this, startFreshProject]() { saveAllAndProject(startFreshProject); },
                    startFreshProject);
            } else {
                startFreshProject();
            }
            break;
        }
        case AppMenuCommand::NewProjectFromTemplate: {
            if (project.isAnyScenePlaying()) return;
            const std::filesystem::path templateName(command.payload);
            auto startFromTemplate = [this, templateName]() {
                if (project.createFromTemplate(templateName)) aiChatWindow->startNewChat();
            };
            if (project.hasScenesUnsavedChanges() || codeEditor->hasUnsavedChanges() ||
                project.isTempUnsavedProject()) {
                registerConfirmAlert(
                    "Unsaved Changes",
                    "There are unsaved changes. Do you want to save them before creating a project from a template?",
                    [this, startFromTemplate]() { saveAllAndProject(startFromTemplate); },
                    startFromTemplate);
            } else {
                startFromTemplate();
            }
            break;
        }
        case AppMenuCommand::NewScene3D:
            if (!project.isAnyScenePlaying())
                project.createNewScene("New Scene", SceneType::SCENE_3D);
            break;
        case AppMenuCommand::NewScene2D:
            if (!project.isAnyScenePlaying())
                project.createNewScene("New Scene", SceneType::SCENE_2D);
            break;
        case AppMenuCommand::NewSceneUI:
            if (!project.isAnyScenePlaying())
                project.createNewScene("New Scene", SceneType::SCENE_UI);
            break;
        case AppMenuCommand::OpenProject:
            openProjectFunc();
            break;
        case AppMenuCommand::OpenRecentProject: {
            if (command.payload.empty() || project.isAnyScenePlaying()) return;
            const std::filesystem::path path(command.payload);
            auto openRecentProject = [this, path]() {
                requestProjectChange([this, path]() { project.loadProject(path); });
            };
            if (project.hasScenesUnsavedChanges() || codeEditor->hasUnsavedChanges() ||
                project.isTempUnsavedProject()) {
                registerConfirmAlert(
                    "Unsaved Changes",
                    "There are unsaved changes. Do you want to save them before opening another project?",
                    [this, openRecentProject]() { saveAllAndProject(openRecentProject); },
                    openRecentProject);
            } else {
                openRecentProject();
            }
            break;
        }
        case AppMenuCommand::ClearRecentProjects:
            AppSettings::clearRecentProjects();
            break;
        case AppMenuCommand::SaveProject:
            project.saveProject(true);
            break;
        case AppMenuCommand::SaveProjectAs:
            if (project.isAnyScenePlaying() || saveDialogInProgress) return;
            registerProjectSaveDialog();
            break;
        case AppMenuCommand::Save:
            saveFunc();
            break;
        case AppMenuCommand::SaveAll:
            saveAllFunc();
            break;
        case AppMenuCommand::ExportProject:
            exportWindow.open(&project);
            break;
        case AppMenuCommand::Exit:
            exit();
            break;
        case AppMenuCommand::Undo:
            if (lastFocusedWindow == LastFocusedWindow::Resources) {
                project.getProjectCommandHistory()->undo();
                resourcesWindow->refreshCurrentDirectory();
            } else if (lastFocusedWindow == LastFocusedWindow::Code) {
                codeEditor->undoLastFocused();
            } else if (lastFocusedWindow != LastFocusedWindow::AI && project.getSelectedScene()) {
                CommandHandle::get(project.getSelectedSceneId())->undo();
            }
            break;
        case AppMenuCommand::Redo:
            if (lastFocusedWindow == LastFocusedWindow::Resources) {
                project.getProjectCommandHistory()->redo();
                resourcesWindow->refreshCurrentDirectory();
            } else if (lastFocusedWindow == LastFocusedWindow::Code) {
                codeEditor->redoLastFocused();
            } else if (lastFocusedWindow != LastFocusedWindow::AI && project.getSelectedScene()) {
                CommandHandle::get(project.getSelectedSceneId())->redo();
            }
            break;
        case AppMenuCommand::ToggleStructure:
            structureWindow->setOpen(!structureWindow->isOpen());
            break;
        case AppMenuCommand::ToggleProperties:
            propertiesWindow->setOpen(!propertiesWindow->isOpen());
            break;
        case AppMenuCommand::ToggleResources:
            resourcesWindow->setOpen(!resourcesWindow->isOpen());
            break;
        case AppMenuCommand::ToggleOutput:
            outputWindow->setOpen(!outputWindow->isOpen());
            break;
        case AppMenuCommand::ToggleAnimation:
            animationWindow->setOpen(!animationWindow->isOpen());
            break;
        case AppMenuCommand::ToggleTerrain:
            terrainEditWindow->setOpen(!terrainEditWindow->isOpen());
            break;
        case AppMenuCommand::ToggleAiChat:
            aiChatWindow->setOpen(!aiChatWindow->isOpen());
            break;
        case AppMenuCommand::ToggleDetachableWindows: {
            const bool multiViewport = !AppSettings::getMultiViewportEnabled();
            AppSettings::setMultiViewportEnabled(multiViewport);
            AppSettings::saveSettings();
            if (multiViewport && !Backend::isRunningOnWayland())
                ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
            else
                ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
#ifdef __linux__
            if (multiViewport) {
                if (Backend::isRunningOnWayland())
                    registerAlert("Restart Required",
                        "Detachable windows will take effect after you restart the editor.",
                        "Note: This feature may encounter issues on Linux.");
                else
                    registerAlert("Experimental Feature",
                        "Detachable windows are now enabled.",
                        "Note: This feature may encounter issues on Linux.");
            }
#endif
            break;
        }
        case AppMenuCommand::ToggleEditorVSync:
            AppSettings::setEditorVSyncEnabled(!AppSettings::getEditorVSyncEnabled());
            AppSettings::saveSettings();
            break;
        case AppMenuCommand::ResetLayout:
            structureWindow->setOpen(true);
            propertiesWindow->setOpen(true);
            resourcesWindow->setOpen(true);
            outputWindow->setOpen(true);
            animationWindow->setOpen(true);
            terrainEditWindow->setOpen(false);
            aiChatWindow->setOpen(true);
            g_notificationsWindow->setOpen(true);
            g_resourceMonitorWindow->setOpen(false);
            g_graphEditorWindow->setOpen(false);
            g_terminalWindow->setOpen(false);
            buildDockspace(true);
            break;
        case AppMenuCommand::ProjectSettings:
            projectSettingsWindow.open(&project);
            break;
        case AppMenuCommand::EditorSettings:
            editorSettingsWindow.open(&project);
            break;
        case AppMenuCommand::SettingsShortcutsTab:
            openEditorSettings(EditorSettingsWindow::Tab::Shortcuts);
            break;
        case AppMenuCommand::SettingsGraphicsTab:
            openEditorSettings(EditorSettingsWindow::Tab::Graphics);
            break;
        case AppMenuCommand::SettingsEngineApiTab:
            openEditorSettings(EditorSettingsWindow::Tab::Api);
            break;
        case AppMenuCommand::ProjectScenes:
            if (!project.isAnyScenePlaying())
                scenesWindow.open(&project);
            break;
        case AppMenuCommand::ProjectBundles:
            bundlesWindow.open(&project);
            break;
        case AppMenuCommand::ValidateProject: {
            const std::filesystem::path projectPath = project.getProjectPath();
            if (projectPath.empty()) {
                Out::warning("Project validation skipped: no project is open");
                break;
            }

            const editor::cli::DoctorResult result = editor::cli::auditProject(projectPath);
            if (!result.completed) {
                Out::error("Project validation failed: %s", result.fatal.c_str());
                break;
            }

            for (const editor::cli::DoctorIssue& issue : result.issues) {
                const char* location = issue.file.c_str();
                if (issue.line > 0) {
                    switch (issue.severity) {
                        case editor::cli::DoctorSeverity::Error:
                            Out::error("Project doctor [%s:%d] %s: %s", location, issue.line,
                                       issue.check.c_str(), issue.message.c_str());
                            break;
                        case editor::cli::DoctorSeverity::Warning:
                            Out::warning("Project doctor [%s:%d] %s: %s", location, issue.line,
                                         issue.check.c_str(), issue.message.c_str());
                            break;
                        case editor::cli::DoctorSeverity::Info:
                            Out::info("Project doctor [%s:%d] %s: %s", location, issue.line,
                                      issue.check.c_str(), issue.message.c_str());
                            break;
                    }
                } else {
                    switch (issue.severity) {
                        case editor::cli::DoctorSeverity::Error:
                            Out::error("Project doctor [%s] %s: %s", location,
                                       issue.check.c_str(), issue.message.c_str());
                            break;
                        case editor::cli::DoctorSeverity::Warning:
                            Out::warning("Project doctor [%s] %s: %s", location,
                                         issue.check.c_str(), issue.message.c_str());
                            break;
                        case editor::cli::DoctorSeverity::Info:
                            Out::info("Project doctor [%s] %s: %s", location,
                                      issue.check.c_str(), issue.message.c_str());
                            break;
                    }
                }
            }

            if (result.errorCount == 0 && result.warningCount == 0) {
                Out::success("Project validation passed: %d scenes and %d entities audited",
                             result.scenesAudited, result.entitiesAudited);
            } else {
                Out::warning("Project validation found %d error(s) and %d warning(s) across %d scenes",
                             result.errorCount, result.warningCount, result.scenesAudited);
            }
            break;
        }
        case AppMenuCommand::ClearTrash: {
            if (project.isAnyScenePlaying()) return;
            const auto projectPath = project.getProjectPath();
            registerConfirmAlert("Empty Project Trash",
                "Permanently delete all files in this project's trash?\nThis cannot be undone.",
                [this, projectPath]() {
                    if (project.getProjectPath() == projectPath && !project.isAnyScenePlaying()) project.clearTrash();
                });
            break;
        }
        case AppMenuCommand::ClearShaderCache: {
            const std::filesystem::path cacheDir = getUserShaderCacheDir();
            if (std::filesystem::exists(cacheDir)) std::filesystem::remove_all(cacheDir);
            break;
        }
        case AppMenuCommand::ToggleProjectFileType: {
            std::string error;
            const bool wasRegistered = g_projectFileTypeRegistered;
            const bool done = wasRegistered
                ? editor::cli::ProjectFileType::unregisterFileType({}, error)
                : editor::cli::ProjectFileType::registerFileType({}, error);
            if (done) {
                Out::success("%s. %s", wasRegistered ? "Project file type unregistered"
                                                     : "Project file type registered",
                             editor::cli::ProjectFileType::describe().c_str());
            } else {
                Out::error("Could not %s the project file type: %s",
                           wasRegistered ? "unregister" : "register", error.c_str());
            }
            g_projectFileTypeRegistered = editor::cli::ProjectFileType::isRegistered();
            break;
        }
        case AppMenuCommand::RunScene:
            if (project.getSelectedScene() && !project.isAnyScenePlaying())
                project.start(project.getSelectedSceneId());
            break;
        case AppMenuCommand::NetworkPlay:
            if (project.getSelectedScene() && !project.isAnyScenePlaying())
                networkPlayWindow.open(&project);
            break;
        case AppMenuCommand::ReloadCppScripts:
            project.reloadActiveCppScripts();
            break;
        case AppMenuCommand::ToggleScriptDebugger:
            scriptDebuggerWindow.open(&project);
            break;
        case AppMenuCommand::PauseScene:
            if (SceneProject* scene = project.getSelectedScene();
                scene && scene->playState == ScenePlayState::PLAYING)
                project.pause(project.getSelectedSceneId());
            break;
        case AppMenuCommand::ResumeScene:
            if (SceneProject* scene = project.getSelectedScene();
                scene && scene->playState == ScenePlayState::PAUSED)
                project.resume(project.getSelectedSceneId());
            break;
        case AppMenuCommand::StopScene:
            if (SceneProject* scene = project.getSelectedScene(); scene &&
                scene->playState != ScenePlayState::SAVING &&
                (scene->playState == ScenePlayState::PLAYING ||
                 scene->playState == ScenePlayState::PAUSED ||
                 scene->playState == ScenePlayState::LOADING))
                project.stop(project.getSelectedSceneId());
            break;
        case AppMenuCommand::DetachCamera:
            project.toggleCameraDetached(project.getSelectedSceneId());
            break;
        case AppMenuCommand::RemoveScene: {
            const uint32_t sceneId = project.getSelectedSceneId();
            if (project.getSelectedScene() && !project.isAnyScenePlaying() &&
                project.getScenes().size() > 1) {
                project.checkUnsavedAndExecute(sceneId, [this, sceneId]() {
                    project.removeScene(sceneId);
                });
            }
            break;
        }
        case AppMenuCommand::ToggleNotifications:
            g_notificationsWindow->setOpen(!g_notificationsWindow->isOpen());
            break;
        case AppMenuCommand::ToggleResourceMonitor:
            g_resourceMonitorWindow->setOpen(!g_resourceMonitorWindow->isOpen());
            break;
        case AppMenuCommand::ToggleGraphEditor:
            g_graphEditorWindow->setOpen(!g_graphEditorWindow->isOpen());
            break;
        case AppMenuCommand::ToggleTerminal:
            if (g_terminalWindow->isOpen())
                g_terminalWindow->setOpen(false);
            else
                g_terminalWindow->focus();
            break;
        case AppMenuCommand::ToggleMaterialBrowser:
            materialBrowserWindow->setOpen(!materialBrowserWindow->isOpen());
            break;
        case AppMenuCommand::ToggleModelExport:
            g_modelExportWindow->setOpen(!g_modelExportWindow->isOpen());
            break;
        case AppMenuCommand::TogglePlugins:
            g_pluginBrowserWindow->setOpen(!g_pluginBrowserWindow->isOpen());
            break;
        case AppMenuCommand::ToggleWorldCreator:
            g_worldCreatorWindow->setOpen(!g_worldCreatorWindow->isOpen());
            break;
        case AppMenuCommand::TogglePhysicsDebugDraw:
            if (SceneProject* sceneProject = project.getSelectedScene();
                sceneProject && sceneProject->scene) {
                auto physics = sceneProject->scene->getSystem<PhysicsSystem>();
                physics->setDebugDrawEnabled(!physics->isDebugDrawEnabled());
                sceneProject->needUpdateRender = true;
            }
            break;
        case AppMenuCommand::CommandPalette:
            g_commandPaletteWindow->toggle();
            break;
        case AppMenuCommand::ToggleWelcome:
            g_welcomeScreenWindow->setOpen(!g_welcomeScreenWindow->isOpen());
            break;
        case AppMenuCommand::ThemeGray:
        case AppMenuCommand::ThemeLight:
        case AppMenuCommand::ThemeDark: {
            const Theme::Preset preset = action == AppMenuCommand::ThemeGray ? Theme::Preset::Gray
                : action == AppMenuCommand::ThemeLight ? Theme::Preset::Light : Theme::Preset::Dark;
            Theme::setPreset(preset);
            Theme::apply();
            AppSettings::setThemePreset(settingFromPreset(preset));
            break;
        }
        case AppMenuCommand::AccentBlue:
        case AppMenuCommand::AccentTeal:
        case AppMenuCommand::AccentAmber:
        case AppMenuCommand::AccentViolet: {
            const char* hex = accentForCommand(action);
            Theme::setAccentColor(hex);
            Theme::apply();
            AppSettings::setAccentColor(hex);
            break;
        }
        case AppMenuCommand::About:
            aboutWindow.open();
            break;
        case AppMenuCommand::DuplicateSelection:
            duplicateSelection();
            break;
        case AppMenuCommand::DeleteSelection:
            deleteSelection();
            break;
        case AppMenuCommand::ToggleCodeComment:
            codeEditor->toggleCommentLastFocused();
            break;
        case AppMenuCommand::ToggleCodeBlockComment:
            codeEditor->toggleBlockCommentLastFocused();
            break;
        case AppMenuCommand::FormatCodeDocument:
            codeEditor->formatDocumentLastFocused();
            break;
        case AppMenuCommand::Documentation:
        case AppMenuCommand::ReportIssue: {
            const char* url = action == AppMenuCommand::Documentation
                ? "https://alguemqualquer123.github.io/quantum-docs/"
                : "https://github.com/alguemqualquer123/quantum/issues";
            auto openInShell = ImGui::GetPlatformIO().Platform_OpenInShellFn;
            if (!openInShell || !openInShell(ImGui::GetCurrentContext(), url)) {
                registerAlert("Open in Browser", std::string("Could not open your browser. Visit:\n") + url);
            }
            break;
        }
        case AppMenuCommand::KeyboardShortcuts:
            registerAlert("Keyboard Shortcuts", buildShortcutHelp(),
                "On macOS, menu shortcuts use Command instead of Ctrl.");
            break;
        case AppMenuCommand::RunPluginCommand: {
            // A plugin's own command, fired by its menu item or by a registered tool. No arguments:
            // a menu row cannot type them, and `plugins run <command> key=value` is where that does.
            const std::string commandName = command.payload;
            plugin::UiValue result;
            std::string error;
            if (!plugin::PluginManager::instance().executeCommand(commandName,
                                                                  plugin::UiValueMap(),
                                                                  result, error)) {
                Out::error("plugin command \"%s\": %s", commandName.c_str(), error.c_str());
                break;
            }
            Out::info("plugin command \"%s\" -> %s", commandName.c_str(),
                      result.toString("-").c_str());
            break;
        }
        case AppMenuCommand::TogglePluginWindow:
            g_pluginWindowHost->toggleWindow(command.payload);
            break;
    }
}

void editor::App::showImGuiMenuItems(const std::vector<PlatformMenuItem>& items){
    for (const PlatformMenuItem& item : items) {
        if (item.type == PlatformMenuItemType::Separator) {
            ImGui::Separator();
        } else if (item.type == PlatformMenuItemType::Submenu) {
            if (ImGui::BeginMenu(item.label.c_str(), item.enabled)) {
                showImGuiMenuItems(item.children);
                ImGui::EndMenu();
            }
        } else if (ImGui::MenuItem(
                       imguiMenuLabel(item),
                       item.shortcut.empty() ? nullptr : item.shortcut.c_str(),
                       item.checked, item.enabled)) {
            executeMenuCommand(item.command);
        }
    }
}

void editor::App::showMenuModelContent(const PlatformMenuModel& menu){
    for (const PlatformMenuItem& topLevel : menu.menus) {
        if (ImGui::BeginMenu(topLevel.label.c_str(), topLevel.enabled)) {
            showImGuiMenuItems(topLevel.children);
            ImGui::EndMenu();
        }
    }
}

void editor::App::showImGuiMenu(const PlatformMenuModel& menu){
    if (ImGui::BeginMainMenuBar()) {
        showMenuModelContent(menu);
        ImGui::EndMainMenuBar();
    }
}

void editor::App::showMenu(){
    PlatformMenuModel menu = buildMenuModel();
    const float nativeMenuHeight = Backend::setMainMenu(
        menu, [this](const PlatformMenuCommand& command) {
            executeMenuCommand(command);
        });
    if (nativeMenuHeight < 0.0f)
        return;
    if (nativeMenuHeight > 0.0f) {
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoBackground |
            ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoNav;
        ImGui::BeginViewportSideBar("##NativeMainMenuSpace", ImGui::GetMainViewport(),
                                    ImGuiDir_Up, nativeMenuHeight, flags);
        ImGui::End();
        return;
    }
    // The library owns the bar; quantum owns its items.
    if (uilabHost)
        uilabHost->drawMenuBar(menu);
    else
        showImGuiMenu(menu);
}

namespace {
    // Footer-sized byte strings: 8.4 GB, 512 MB, 47 KB. The footer redraws every
    // frame, so the formatting stays snprintf-into-a-stack-buffer.
    char* footerBytes(char* buffer, size_t size, unsigned long long bytes) {
        const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
        if (mb >= 1024.0) {
            std::snprintf(buffer, size, "%.1f GB", mb / 1024.0);
        } else if (mb >= 1.0) {
            std::snprintf(buffer, size, "%.0f MB", mb);
        } else {
            std::snprintf(buffer, size, "%.0f KB", static_cast<double>(bytes) / 1024.0);
        }
        return buffer;
    }

    char* footerRate(char* buffer, size_t size, double bytesPerSecond) {
        if (bytesPerSecond < 0.0) bytesPerSecond = 0.0;
        const double kb = bytesPerSecond / 1024.0;
        if (kb >= 1024.0) {
            std::snprintf(buffer, size, "%.1f MB/s", kb / 1024.0);
        } else if (kb >= 1.0) {
            std::snprintf(buffer, size, "%.0f KB/s", kb);
        } else {
            std::snprintf(buffer, size, "0 KB/s");
        }
        return buffer;
    }
}

void editor::App::showFooter(){
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (!viewport) {
        return;
    }

    const float fontScale = 0.9f;
    const float footerHeight = (ImGui::GetTextLineHeight() * fontScale) + Theme::dpi(10.0f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                            ImGuiWindowFlags_NoMove |
                            ImGuiWindowFlags_NoDocking |
                            ImGuiWindowFlags_NoSavedSettings |
                            ImGuiWindowFlags_NoNav |
                            ImGuiWindowFlags_NoScrollbar |
                            ImGuiWindowFlags_NoFocusOnAppearing;

    // Position at the bottom of the main viewport
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + viewport->Size.y - footerHeight));
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, footerHeight));
    ImGui::SetNextWindowBgAlpha(1.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Theme::dpi(ImVec2(8.0f, 6.0f)));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_Border));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.60f, 0.60f, 0.60f, 1.00f));

    if (ImGui::Begin("##Footer", nullptr, flags)) {
        ImGui::SetWindowFontScale(fontScale);

        const float fps = footerFramerate;
        const float deltaMs = footerDeltaMs;
        const size_t queuedResources = Engine::getQueuedResourceCount();
        SceneProject* selectedScene = project.getSelectedScene();
        const bool hasSelectedScene = selectedScene != nullptr;
        const bool isPlaying = hasSelectedScene && selectedScene->playState == ScenePlayState::PLAYING;
        const bool isPaused = hasSelectedScene && selectedScene->playState == ScenePlayState::PAUSED;
        const bool isSaving = hasSelectedScene && selectedScene->playState == ScenePlayState::SAVING;
        const bool isLoading = hasSelectedScene && selectedScene->playState == ScenePlayState::LOADING;
        const bool isStopped = !hasSelectedScene || selectedScene->playState == ScenePlayState::STOPPED;
        const bool isCancelling = hasSelectedScene && selectedScene->playState == ScenePlayState::CANCELLING;
        const bool isAnySaving = project.isAnySceneSaving();
        const bool canPlayPause = hasSelectedScene && !isSaving && !isLoading && !isCancelling && (isPlaying || isPaused || (isStopped && !project.isAnyScenePlaying()));
        const bool canStop = hasSelectedScene && !isSaving && !isStopped && !isCancelling;
        const ImVec4 footerButtonHovered = ImVec4(1.0f, 1.0f, 1.0f, 0.08f);
        const ImVec4 footerButtonActive = ImVec4(1.0f, 1.0f, 1.0f, 0.14f);

        auto footerActionButton = [&](const char* label) -> bool {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, footerButtonHovered);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, footerButtonActive);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, Theme::dpi(ImVec2(6.0f, 1.0f)));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);

            bool pressed = ImGui::Button(label);

            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(3);

            return pressed;
        };

        showFooterGit();

        // Left side: Status
        bool statusShown = false;
        if (isSaving || isAnySaving) {
            ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), ICON_FA_FLOPPY_DISK " Saving scene");
            statusShown = true;
        } else if (isLoading) {
            ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), ICON_FA_SPINNER " Loading scene");
            statusShown = true;
        } else if (isCancelling) {
            ImGui::TextColored(ImVec4(0.8f, 0.7f, 0.4f, 1.0f), ICON_FA_STOP " Stopping");
            statusShown = true;
        } else if (queuedResources > 0) {
            ImGui::Text(ICON_FA_SPINNER " Loading resources: %zu", queuedResources);
            statusShown = true;
        } else {
             if (selectedScene) {
                if (selectedScene->playState == ScenePlayState::PLAYING){
                    ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f), ICON_FA_PLAY " Playing");
                    statusShown = true;
                } else if (selectedScene->playState == ScenePlayState::PAUSED){
                    ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.4f, 1.0f), ICON_FA_PAUSE " Paused");
                    statusShown = true;
                }
             }
        }

        if (!statusShown) {
            ImGui::TextDisabled(ICON_FA_CHECK " Ready");
        }

        // Middle: Scene Info & Selection
        if (selectedScene) {
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            ImGui::Text("Scene: %s", selectedScene->name.c_str());

            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();

            size_t selectionCount = selectedScene->selectedEntities.size();
            if (selectionCount == 0) {
                ImGui::TextDisabled("No Selection");
            } else if (selectionCount == 1) {
                ImGui::Text("Selected: 1 Entity");
            } else {
                ImGui::Text("Selected: %zu Entities", selectionCount);
            }

            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();

            ImGui::BeginDisabled(!canPlayPause);
            if (footerActionButton(isPlaying ? ICON_FA_PAUSE " Pause" : (isPaused ? ICON_FA_PLAY " Resume" : ICON_FA_PLAY " Play"))) {
                if (isPlaying) {
                    project.pause(selectedScene->id);
                } else if (isPaused) {
                    project.resume(selectedScene->id);
                } else {
                    project.start(selectedScene->id);
                }
            }
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::BeginDisabled(!canStop);
            if (footerActionButton(ICON_FA_STOP " Stop")) {
                project.stop(selectedScene->id);
            }
            ImGui::EndDisabled();
        }

        // Right side: performance stats. FPS and frame time are pinned to the
        // right edge; the machine cluster (CPU, RAM, GPU/VRAM, network) rides to
        // their left while it fits. On narrow windows the cluster sheds from the
        // tail (network first) instead of shoving the pinned stats off-screen.
        char fpsText[32];
        char msText[32];
        char drawText[48];
        char triText[48];
        sprintf(fpsText, ICON_FA_GAUGE_HIGH " %.1f FPS", fps);
        sprintf(msText, ICON_FA_CLOCK " %.2f ms", deltaMs);
        const Engine::FrameStats& frameStats = Engine::getFrameStats();
        sprintf(drawText, ICON_FA_CUBES " %u", frameStats.drawCalls);
        sprintf(triText, ICON_FA_DRAW_POLYGON " %llu", (unsigned long long)frameStats.triangles);

        struct FooterStat {
            const char* text;
            ImVec4 color;
        };
        FooterStat machineStats[6];
        int machineStatCount = 0;

        char cpuText[48];
        char ramText[64];
        char gpuText[48];
        char vramText[64];
        char downText[48];
        char upText[48];
        char scratch[32];

        const SystemSample& machine = SystemStats::current();
        if (machine.valid) {
            const auto pressureColor = [](double fraction) {
                if (fraction >= 0.90) return Theme::Colors::DangerText;
                if (fraction >= 0.70) return Theme::Colors::WarningText;
                return Theme::Colors::SuccessText;
            };

            sprintf(cpuText, ICON_FA_MICROCHIP " %.0f%%", machine.cpu.usagePercent);
            machineStats[machineStatCount++] = {cpuText, pressureColor(machine.cpu.usagePercent / 100.0)};

            sprintf(ramText, ICON_FA_MEMORY " %s",
                    footerBytes(scratch, sizeof(scratch), machine.memory.usedBytes));
            machineStats[machineStatCount++] = {ramText, ImGui::GetStyleColorVec4(ImGuiCol_Text)};

            if (machine.gpu.usagePercent >= 0.0) {
                sprintf(gpuText, ICON_FA_VIDEO " %.0f%%", machine.gpu.usagePercent);
                machineStats[machineStatCount++] = {gpuText, pressureColor(machine.gpu.usagePercent / 100.0)};
            }
            if (machine.gpu.vramTotalBytes > 0) {
                sprintf(vramText, ICON_FA_HARD_DRIVE " %s",
                        footerBytes(scratch, sizeof(scratch), machine.gpu.vramUsedBytes));
                machineStats[machineStatCount++] = {vramText, Theme::Colors::SubtleText};
            }

            sprintf(downText, ICON_FA_ARROW_DOWN " %s",
                    footerRate(scratch, sizeof(scratch), machine.network.downBytesPerSec));
            machineStats[machineStatCount++] = {downText, Theme::Colors::NetworkDown};
            sprintf(upText, ICON_FA_ARROW_UP " %s",
                    footerRate(scratch, sizeof(scratch), machine.network.upBytesPerSec));
            machineStats[machineStatCount++] = {upText, Theme::Colors::NetworkUp};
        }

        // Width the whole cluster needs, separators included, then shed entries
        // from the tail until the remainder fits beside the left side.
        const float separatorWidth = ImGui::CalcTextSize("|").x + ImGui::GetStyle().ItemSpacing.x * 2.0f;
        float statsWidth = ImGui::CalcTextSize(fpsText).x + ImGui::CalcTextSize(msText).x + separatorWidth;
        for (int i = 0; i < machineStatCount; ++i) {
            statsWidth += separatorWidth + ImGui::CalcTextSize(machineStats[i].text).x;
        }

        const float minStartX = ImGui::GetCursorPosX() + Theme::dpi(8.0f);
        while (machineStatCount > 0 && ImGui::GetWindowContentRegionMax().x - statsWidth < minStartX) {
            --machineStatCount;
            statsWidth -= separatorWidth + ImGui::CalcTextSize(machineStats[machineStatCount].text).x;
        }

        const float statsStartX = ImGui::GetWindowContentRegionMax().x - statsWidth;
        if (statsStartX > ImGui::GetCursorPosX()) {
            ImGui::SameLine(statsStartX);
        } else {
            ImGui::SameLine();
        }

        for (int i = 0; i < machineStatCount; ++i) {
            ImGui::TextColored(machineStats[i].color, "%s", machineStats[i].text);
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
        }

        ImGui::Text("%s", fpsText);
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::Text("%s", msText);
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::Text("%s", drawText);
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::Text("%s", triText);
    }
    ImGui::End();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();

    // Reserve space from the viewport work area so dockspace doesn't overlap
    viewport->WorkSize.y -= footerHeight;
}

// Branch and sync state at the far left, like VSCode's status bar
void editor::App::showFooterGit(){
    const GitStatus& git = gitMonitor.getStatus();
    if (!git.repository) {
        return;
    }

    const char* icon = ICON_FA_CODE_BRANCH;
    if (git.headType == GitHeadType::Tag) {
        icon = ICON_FA_TAG;
    } else if (git.headType == GitHeadType::Commit) {
        icon = ICON_FA_CODE_COMMIT;
    }

    // VSCode's marks: * changes, + staged changes, ! merge, rebase or conflicts
    std::string label = std::string(icon) + " " + git.head;
    if (git.unstaged > 0) label += "*";
    if (git.staged > 0) label += "+";
    if (git.operationInProgress) label += "!";

    ImGui::TextUnformatted(label.c_str());
    if (ImGui::BeginItemTooltip()) {
        ImGui::Text("%d staged, %d unstaged changes", git.staged, git.unstaged);
        if (!git.upstream.empty()) {
            ImGui::Text("Tracking %s", git.upstream.c_str());
        }
        if (git.operationInProgress) {
            ImGui::TextUnformatted("Unfinished merge, rebase or conflicts");
        }
        ImGui::EndTooltip();
    }

    if (git.ahead > 0 || git.behind > 0) {
        ImGui::SameLine();
        ImGui::Text(ICON_FA_ARROWS_ROTATE " %d" ICON_FA_ARROW_DOWN_LONG " %d" ICON_FA_ARROW_UP_LONG, git.behind, git.ahead);
        ImGui::SetItemTooltip("%d behind and %d ahead of %s", git.behind, git.ahead, git.upstream.c_str());
    }

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
}

void editor::App::showAlert(){
    if (alert.needShow) {
        ImGui::OpenPopup((alert.title + "##AlertModal").c_str());

        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

        ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize |
                                ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_Modal;

        if (ImGui::BeginPopupModal((alert.title + "##AlertModal").c_str(), nullptr, flags)) {
            ImGui::Text("%s", alert.message.c_str());
            if (!alert.note.empty()) {
                ImGui::Spacing();
                ImGui::TextColored(Theme::Colors::WarningText, "%s", alert.note.c_str());
            }
            ImGui::Separator();

            if (alert.type == AlertType::Info) {
                // For info alerts, just show OK button
                ImGui::SetCursorPosX((ImGui::GetWindowSize().x - 120) * 0.5f);
                if (ImGui::Button("OK", ImVec2(120, 0))) {
                    alert.needShow = false;
                    ImGui::CloseCurrentPopup();
                }
            } else if (alert.type == AlertType::Confirm) {
                // For confirmation alerts, show Yes and No buttons
                float windowWidth = ImGui::GetWindowSize().x;
                float buttonsWidth = 250; // Total width for both buttons and spacing
                ImGui::SetCursorPosX((windowWidth - buttonsWidth) * 0.5f);

                if (ImGui::Button("Yes", ImVec2(120, 0))) {
                    alert.needShow = false;
                    ImGui::CloseCurrentPopup();
                    if (alert.onYes) {
                        runWhenUiIdle(alert.onYes);
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("No", ImVec2(120, 0))) {
                    alert.needShow = false;
                    ImGui::CloseCurrentPopup();
                    if (alert.onNo) {
                        runWhenUiIdle(alert.onNo);
                    }
                }
            } else if (alert.type == AlertType::ThreeButton) {
                // For three-button alerts, show Yes, No and Cancel buttons
                float windowWidth = ImGui::GetWindowSize().x;
                float buttonsWidth = 370; // Width for three buttons
                ImGui::SetCursorPosX((windowWidth - buttonsWidth) * 0.5f);

                if (ImGui::Button("Yes", ImVec2(120, 0))) {
                    alert.needShow = false;
                    ImGui::CloseCurrentPopup();
                    if (alert.onYes) {
                        runWhenUiIdle(alert.onYes);
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("No", ImVec2(120, 0))) {
                    alert.needShow = false;
                    ImGui::CloseCurrentPopup();
                    if (alert.onNo) {
                        runWhenUiIdle(alert.onNo);
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                    alert.needShow = false;
                    ImGui::CloseCurrentPopup();
                    if (alert.onCancel) {
                        runWhenUiIdle(alert.onCancel);
                    }
                }
            }
            ImGui::EndPopup();
        }
    }
}

void editor::App::buildDockspace(bool resetLayout){
    // A reset forces every tab back to its default slot even if the ini has a
    // customized position saved for it (see dockTabWindow()).
    forceDockTabs = resetLayout;

    // Keep an existing layout (restored from the persisted ini) so the user's
    // customizations survive; only recover the scene node we dock tabs into.
    // A reset rebuilds the default skeleton from scratch instead.
    if (!resetLayout && ImGui::DockBuilderGetNode(dockspace_id) != nullptr) {
        dock_id_middle_top = getCentralDockId();
    } else {
        buildDefaultLayout();
        cappedDockSizes.clear();
        // Built from the current font size, so it needs no DPI correction.
        layoutUiScale = mainViewportScale();
        layoutScaleApplied = true;
    }

    dockProjectTabs();

    ImGui::DockBuilderFinish(dockspace_id);

    forceDockTabs = false;
}

// The ini stores dock sizes in absolute pixels and ImGui never converts them, so
// a layout saved at 100% stays narrow at 150% on every launch.
void editor::App::rescaleRestoredLayout(){
    if (layoutScaleApplied) return;

    // Nothing persisted: buildDefaultLayout() builds at the current scale below.
    ImGuiDockNode* root = ImGui::DockBuilderGetNode(dockspace_id);
    if (!root) return;

    const float currentScale = mainViewportScale();
    const float savedScale = sanitizeLayoutScale(layoutUiScale);
    layoutUiScale = currentScale;
    layoutScaleApplied = true;

    const float ratio = currentScale / savedScale;
    if (ratio > 0.99f && ratio < 1.01f) return;

    // The root node is pinned to the viewport, so only its descendants move.
    scaleDockChildren(root, ratio);
}

// SizeRef against a central node is read as an absolute size, so it has to grow
// with the text; siblings split by ratio stay proportional under the same factor.
void editor::App::scaleDockChildren(ImGuiDockNode* node, float ratio){
    const ImVec2 available = ImGui::GetMainViewport()->WorkSize;
    for (int i = 0; i < 2; i++) {
        ImGuiDockNode* child = node->ChildNodes[i];
        if (!child) continue;
        const ImVec2 sizeRef = child->SizeRef;
        child->Size = ImVec2(child->Size.x * ratio, child->Size.y * ratio);
        child->SizeRef = ImVec2(sizeRef.x * ratio, sizeRef.y * ratio);

        // Panels beside the scene only grow up to the caps, or the scene gets
        // squeezed out. One already past its cap keeps its size.
        if (holdsCentralNode(node->ChildNodes[i ^ 1]) && !holdsCentralNode(child)) {
            const int axis = node->SplitAxis;
            const float cap = available[axis] * (axis == ImGuiAxis_X ? sidePanelMaxShare : bottomPanelMaxShare);
            float uncapped = sizeRef[axis] * ratio;
            // Resume from the uncapped size unless the panel was resized (the ini keeps whole pixels).
            auto it = cappedDockSizes.find(child->ID);
            if (it != cappedDockSizes.end() && std::fabs(it->second.capped - sizeRef[axis]) < 1.0f) {
                uncapped = it->second.uncapped * ratio;
            }
            const float size = std::min(uncapped, std::max(sizeRef[axis], cap));
            child->Size[axis] = size;
            child->SizeRef[axis] = size;
            if (size < uncapped) {
                cappedDockSizes[child->ID] = { size, uncapped };
            } else {
                cappedDockSizes.erase(child->ID);
            }
        }
        scaleDockChildren(child, ratio);
    }
}

// The layout scale and the capped panel sizes are saved in the ini with the dock
// sizes they describe. Inis from older versions fall back to settings.yaml.
void editor::App::registerLayoutSettings(){
    ImGuiSettingsHandler handler;
    handler.TypeName = "QuantumLayout";
    handler.TypeHash = ImHashStr("QuantumLayout");
    handler.UserData = this;
    handler.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler*, const char*) -> void* {
        return reinterpret_cast<void*>(1);
    };
    handler.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler* handler, void*, const char* line) {
        App* app = static_cast<App*>(handler->UserData);
        ImGuiID id;
        float scale, capped, uncapped;
        if (std::sscanf(line, "Scale=%f", &scale) == 1) {
            app->layoutUiScale = scale;
        } else if (std::sscanf(line, "Panel=0x%08X Capped=%f Uncapped=%f", &id, &capped, &uncapped) == 3) {
            app->cappedDockSizes[id] = { capped, uncapped };
        }
    };
    handler.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf) {
        const App* app = static_cast<const App*>(handler->UserData);
        buf->appendf("[%s][Data]\n", handler->TypeName);
        buf->appendf("Scale=%g\n", sanitizeLayoutScale(app->layoutUiScale));
        for (const auto& [id, size] : app->cappedDockSizes) {
            buf->appendf("Panel=0x%08X Capped=%.1f Uncapped=%.1f\n", id, size.capped, size.uncapped);
        }
        buf->append("\n");
    };
    ImGui::AddSettingsHandler(&handler);
}

// A new UI scale resizes the docked panels along with the text.
void editor::App::applyUiScale(){
    const float uiScale = AppSettings::getUiScale();
    if (uiScale == Theme::uiScale()) return;

    // Until the layout is restored, rescaleRestoredLayout() takes care of it.
    if (layoutScaleApplied) {
        const float ratio = uiScale / Theme::uiScale();
        if (ImGuiDockNode* root = ImGui::DockBuilderGetNode(dockspace_id)) {
            scaleDockChildren(root, ratio);
        }
        layoutUiScale *= ratio;
    }
    Theme::setUiScale(uiScale);
}

void editor::App::buildDefaultLayout(){
    // WorkSize is what DockSpaceOverViewport() sizes the root node to, so the
    // caps below measure against the room the panels actually get.
    const ImVec2 viewport = ImGui::GetMainViewport()->WorkSize;
    ImGuiID dock_id_left, dock_id_left_top, dock_id_left_bottom, dock_id_right, dock_id_middle, dock_id_middle_bottom;
    // dock_id_middle_top is a member: dockTabWindow() reads it after this runs.

    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, viewport);

    // Sizes in font sizes hold the same text at any DPI, but can add up to more
    // than the window has, leaving a sibling with no room at all.
    auto preferredSize = [](float fontSizes, float available, float maxShare){
        return std::min(fontSizes * ImGui::GetFontSize(), available * maxShare);
    };

    // Structure on the left, Resources split off its bottom.
    ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.0f, &dock_id_left, &dock_id_middle);
    ImGui::DockBuilderSetNodeSize(dock_id_left, ImVec2(preferredSize(14, viewport.x, sidePanelMaxShare), viewport.y));
    ImGui::DockBuilderDockWindow(Structure::WINDOW_NAME, dock_id_left);

    ImGui::DockBuilderSplitNode(dock_id_left, ImGuiDir_Down, 0.0f, &dock_id_left_bottom, &dock_id_left_top);
    ImGui::DockBuilderSetNodeSize(dock_id_left_bottom, ImVec2(viewport.x, preferredSize(50, viewport.y, 0.65f)));
    ImGui::DockBuilderDockWindow(ResourcesWindow::WINDOW_NAME, dock_id_left_bottom);

    // Properties on the right.
    ImGui::DockBuilderSplitNode(dock_id_middle, ImGuiDir_Right, 0.0f, &dock_id_right, &dock_id_middle);
    ImGui::DockBuilderSetNodeSize(dock_id_right, ImVec2(preferredSize(19, viewport.x, sidePanelMaxShare), viewport.y));
    ImGui::DockBuilderDockWindow(Properties::WINDOW_NAME, dock_id_right);
    ImGui::DockBuilderDockWindow(AiChatWindow::WINDOW_NAME, dock_id_right);
    ImGui::DockBuilderDockWindow(NotificationsWindow::WINDOW_NAME, dock_id_right);

    auto stampPanelDockOrder = [](const char* windowName, short order) {
        ImGuiID windowId = ImHashStr(windowName);
        ImGuiWindowSettings* settings = ImGui::FindWindowSettingsByID(windowId);
        if (!settings) settings = ImGui::CreateNewWindowSettings(windowName);
        settings->DockOrder = order;
        if (ImGuiWindow* window = ImGui::FindWindowByName(windowName)) {
            window->DockOrder = order;
        }
    };
    stampPanelDockOrder(Properties::WINDOW_NAME, 0);
    stampPanelDockOrder(AiChatWindow::WINDOW_NAME, 1);

    // Output/Animation across the bottom; scenes fill the remaining centre.
    ImGui::DockBuilderSplitNode(dock_id_middle, ImGuiDir_Down, 0.0f, &dock_id_middle_bottom, &dock_id_middle_top);
    ImGui::DockBuilderSetNodeSize(dock_id_middle_bottom, ImVec2(viewport.x, preferredSize(10, viewport.y, bottomPanelMaxShare)));
    ImGui::DockBuilderDockWindow(OutputWindow::WINDOW_NAME, dock_id_middle_bottom);
    ImGui::DockBuilderDockWindow(AnimationWindow::WINDOW_NAME, dock_id_middle_bottom);
    ImGui::DockBuilderDockWindow(ResourceMonitorWindow::WINDOW_NAME, dock_id_middle_bottom);
    ImGui::DockBuilderDockWindow(TerminalWindow::WINDOW_NAME, dock_id_middle_bottom);
    ImGui::DockBuilderDockWindow(MaterialBrowserWindow::WINDOW_NAME, dock_id_middle_bottom);
    ImGui::DockBuilderDockWindow(ModelExportWindow::WINDOW_NAME, dock_id_middle_bottom);
    ImGui::DockBuilderDockWindow(PluginBrowserWindow::WINDOW_NAME, dock_id_middle_bottom);
    ImGui::DockBuilderDockWindow(WorldCreatorWindow::WINDOW_NAME, dock_id_middle_top);
    ImGui::DockBuilderDockWindow(GraphEditorWindow::WINDOW_NAME, dock_id_middle_top);
    // The library's console reads the same lines Out mirrors, so it joins the strip
    // as a sibling tab while Output still draws its own list.
    if (uilabHost)
        ImGui::DockBuilderDockWindow(uilabHost->consoleTitle(), dock_id_middle_bottom);

    // Mark the scene area as the central node. CentralNode is a saved flag, so it
    // persists in the ini and lets getCentralDockId() recover this node on the
    // next launch without rebuilding the layout.
    if (ImGuiDockNode* centerNode = ImGui::DockBuilderGetNode(dock_id_middle_top)) {
        centerNode->SetLocalFlags(centerNode->LocalFlags | ImGuiDockNodeFlags_CentralNode);
    }
}

void editor::App::dockProjectTabs(){
    // Re-open document tabs and dock scenes in their saved order. dockTabWindow()
    // leaves already-known windows where the ini put them (unless resetting).
    const std::vector<TabEntry>& tabs = project.getTabs();

    auto dockSceneTab = [&](const TabEntry& tab) {
        for (auto& sceneProject : project.getScenes()) {
            if (sceneProject.opened && sceneProject.filepath.string() == tab.filepath) {
                addNewSceneToDock(sceneProject.id);
                break;
            }
        }
    };

    auto dockCodeTab = [&](const TabEntry& tab) {
        fs::path fullPath = project.getProjectPath() / tab.filepath;
        if (fs::exists(fullPath)) {
            if (!codeEditor->isFileOpen(tab.filepath)) {
                codeEditor->openFile(tab.filepath);
            } else {
                dockTabWindow("###" + tab.filepath);
            }
        }
    };

    auto dockImageTab = [&](const TabEntry& tab) {
        fs::path fullPath = project.getProjectPath() / tab.filepath;
        if (fs::exists(fullPath)) {
            if (!imageViewerWindow->isFileOpen(tab.filepath)) {
                imageViewerWindow->openFile(tab.filepath, false);
            } else {
                dockTabWindow(ImageViewerWindow::getWindowId(tab.filepath));
            }
        }
    };

    auto dockModelTab = [&](const TabEntry& tab) {
        fs::path fullPath = project.getProjectPath() / tab.filepath;
        if (fs::exists(fullPath)) {
            if (!modelViewerWindow->isFileOpen(tab.filepath)) {
                modelViewerWindow->openFile(tab.filepath, false);
            } else {
                dockTabWindow(ModelViewerWindow::getWindowId(tab.filepath));
            }
        }
    };

    auto dockOrphanScenes = [&]() {
        for (auto& sceneProject : project.getScenes()) {
            if (!sceneProject.opened) continue;
            if (!project.hasTab(TabType::SCENE, sceneProject.filepath.string())) {
                addNewSceneToDock(sceneProject.id);
            }
        }
    };

    if (forceDockTabs) {
        // Reset layout: scenes first, then code, image and model documents.
        for (const auto& tab : tabs) {
            if (tab.type == TabType::SCENE) dockSceneTab(tab);
        }
        dockOrphanScenes();
        for (const auto& tab : tabs) {
            if (tab.type == TabType::CODE_EDITOR) dockCodeTab(tab);
        }
        for (const auto& tab : tabs) {
            if (tab.type == TabType::IMAGE_VIEWER) dockImageTab(tab);
        }
        for (const auto& tab : tabs) {
            if (tab.type == TabType::MODEL_VIEWER) dockModelTab(tab);
        }
    } else {
        for (const auto& tab : tabs) {
            if (tab.type == TabType::SCENE) dockSceneTab(tab);
            else if (tab.type == TabType::CODE_EDITOR) dockCodeTab(tab);
            else if (tab.type == TabType::IMAGE_VIEWER) dockImageTab(tab);
            else if (tab.type == TabType::MODEL_VIEWER) dockModelTab(tab);
        }
        dockOrphanScenes();
    }

    // Make the saved tab list authoritative for tab ordering. Scene window ids
    // (###Scene<id>) are reassigned on every load, so the per-window DockOrder
    // ImGui keeps in its ini can't be matched back to a scene. Stamp a sequential
    // DockOrder onto each tab's window settings from project.tabs order so the
    // shared central node restores its tabs in the order the user left them
    // (captured live by captureTabOrder()). ImGui copies this onto the window's
    // DockOrder when it is created, and tabs appearing on the same frame are
    // sorted by it.
    short dockOrder = 0;
    auto stampDockOrder = [&](const std::string& windowName) {
        if (windowName.empty()) return;
        ImGuiID windowId = ImHashStr(windowName.c_str());
        ImGuiWindowSettings* settings = ImGui::FindWindowSettingsByID(windowId);
        if (!settings) settings = ImGui::CreateNewWindowSettings(windowName.c_str());
        settings->DockOrder = dockOrder;
        if (ImGuiWindow* window = ImGui::FindWindowByName(windowName.c_str())) {
            window->DockOrder = dockOrder;
        }
        dockOrder++;
    };

    if (forceDockTabs) {
        for (const auto& tab : tabs) {
            if (tab.type == TabType::SCENE) stampDockOrder(tabWindowName(tab));
        }
        for (auto& sceneProject : project.getScenes()) {
            if (!sceneProject.opened) continue;
            if (!project.hasTab(TabType::SCENE, sceneProject.filepath.string())) {
                stampDockOrder("###Scene" + std::to_string(sceneProject.id));
            }
        }
        for (const auto& tab : tabs) {
            if (tab.type == TabType::CODE_EDITOR) stampDockOrder(tabWindowName(tab));
        }
        for (const auto& tab : tabs) {
            if (tab.type == TabType::IMAGE_VIEWER) stampDockOrder(tabWindowName(tab));
        }
        for (const auto& tab : tabs) {
            if (tab.type == TabType::MODEL_VIEWER) stampDockOrder(tabWindowName(tab));
        }
    } else {
        for (const auto& tab : tabs) {
            stampDockOrder(tabWindowName(tab));
        }
    }
}

std::string editor::App::tabWindowName(const TabEntry& tab) const {
    if (tab.type == TabType::SCENE) {
        for (const auto& sceneProject : project.getScenes()) {
            if (sceneProject.opened && sceneProject.filepath.string() == tab.filepath) {
                return "###Scene" + std::to_string(sceneProject.id);
            }
        }
        return {};
    }
    if (tab.type == TabType::IMAGE_VIEWER) {
        return ImageViewerWindow::getWindowId(tab.filepath);
    }
    if (tab.type == TabType::MODEL_VIEWER) {
        return ModelViewerWindow::getWindowId(tab.filepath);
    }
    // CodeEditor docks its windows as "###<relative-filepath>" (see getWindowTitle()).
    return "###" + tab.filepath;
}

void editor::App::captureTabOrder() {
    // Mirror the live ImGui tab order back into project.tabs so a user's
    // drag-reordering of scene/code/image tabs survives a save and the next launch.
    // ImGui keeps each window's visual position in its DockNode as DockOrder;
    // we reorder the (filepath-keyed, stable) tab list to match. Reordering a
    // tab triggers no save on its own, so we persist the change ourselves,
    // debounced until the user stops dragging.
    std::vector<TabEntry>& tabs = project.getTabs();
    if (tabs.size() >= 2) {
        struct OrderedTab { TabEntry entry; int order; };
        std::vector<OrderedTab> ordered;
        ordered.reserve(tabs.size());
        for (const TabEntry& tab : tabs) {
            ImGuiWindow* window = ImGui::FindWindowByName(tabWindowName(tab).c_str());
            // Windows that aren't currently docked (DockOrder < 0) or not yet
            // instantiated keep their relative position via the stable sort.
            int order = (window && window->DockOrder >= 0) ? window->DockOrder
                                                           : std::numeric_limits<int>::max();
            ordered.push_back({tab, order});
        }

        std::stable_sort(ordered.begin(), ordered.end(),
            [](const OrderedTab& a, const OrderedTab& b){ return a.order < b.order; });

        bool changed = false;
        for (size_t i = 0; i < tabs.size(); ++i) {
            if (tabs[i].type != ordered[i].entry.type || tabs[i].filepath != ordered[i].entry.filepath) {
                tabs[i] = ordered[i].entry;
                changed = true;
            }
        }

        if (changed) {
            tabsOrderDirty = true;
            tabsOrderChangeTime = ImGui::GetTime();
        }
    }

    // Persist a short moment after the last reorder so a drag results in one
    // write rather than one per frame while the tab slides past its neighbours.
    if (tabsOrderDirty && ImGui::GetTime() - tabsOrderChangeTime > 0.75) {
        project.saveWorkspaceFile();
        tabsOrderDirty = false;
    }
}

void editor::App::dockTabWindow(const std::string& windowName, bool force){
    // Auto-dock only a window the layout ini has never seen, so we never override
    // a position the user chose (docked or intentionally floating). A layout reset
    // (forceDockTabs) and explicit user opens (force) snap the tab to the centre.
    if (force || forceDockTabs || !ImGui::FindWindowSettingsByID(ImHashStr(windowName.c_str()))) {
        dock_id_middle_top = getCentralDockId();
        ImGui::DockBuilderDockWindow(windowName.c_str(), dock_id_middle_top);
    }
}

ImGuiID editor::App::getCentralDockId(){
    // The scene area carries ImGuiDockNodeFlags_CentralNode (a saved flag, so it
    // survives in the ini). We scan for it rather than calling
    // DockBuilderGetCentralNode(), which reads root->CentralNode — only populated
    // once DockSpace() has processed the dockspace, not yet true on the first
    // frame after the layout is restored from the ini.
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    for (const ImGuiStoragePair& pair : ctx->DockContext.Nodes.Data) {
        ImGuiDockNode* node = static_cast<ImGuiDockNode*>(pair.val_p);
        if (node && node->IsCentralNode() && ImGui::DockNodeGetRootNode(node)->ID == dockspace_id) {
            return node->ID;
        }
    }

    // No central node yet (e.g. a pre-existing ini without one): fall back to the
    // dockspace root so windows still attach somewhere sensible.
    return dockspace_id;
}

void editor::App::showStyleEditor(){
    ImGui::Begin("Dear ImGui Style Editor", nullptr);
    {
        // Get the current IO object to access display size
        ImGuiIO& io = ImGui::GetIO();
        ImVec2 windowSize = ImGui::GetWindowSize();

        // Calculate the centered position at the top
        float windowX = (io.DisplaySize.x - windowSize.x - 200) / 2;
        float windowY = 0.0f; // Top of the screen

        // Set the window position
        ImGui::SetWindowPos(ImVec2(windowX, windowY), ImGuiCond_Once);
        ImGui::SetWindowCollapsed(true, ImGuiCond_Once);

        ImGui::ShowStyleEditor();
    }
    ImGui::End();
}

ImFont* editor::App::getCodeFont() {
    return codeFont ? codeFont : ImGui::GetIO().Fonts->Fonts[0];
}

void editor::App::setup() {
    mainThreadId = std::this_thread::get_id();

    // Initialize application settings
    initializeSettings();

    ImGuiIO& io = ImGui::GetIO();

    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;      // Enable Docking
    // Activate only where it works: on Wayland the backend can't reposition windows,
    // so the feature stays off this session until a restart forces the X11 backend.
    if (AppSettings::getMultiViewportEnabled() && !Backend::isRunningOnWayland())
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;  // Multi-Viewport / Platform Windows (toggle in View menu)

    io.ConfigWindowsMoveFromTitleBarOnly = true;

    // Persist the docking layout next to settings.yaml at a stable absolute
    // path, so it survives restarts regardless of the working directory or
    // build type. (initializeSettings() above has already set the config dir.)
    layoutIniPath = (AppSettings::getConfigDirectory() / "editor_layout.ini").string();
    // ImGui parses this file before the first frame. After a crash, leave the
    // file untouched and build a fresh layout so malformed dock state cannot
    // abort boot; the original remains available for manual recovery.
    io.IniFilename = (safeMode || editor::LaunchOptions::get().resetLayout)
        ? nullptr : layoutIniPath.c_str();
    registerLayoutSettings();

    // Separate from the window scale: the layout keeps the scale it was built at,
    // while the window is measured on whichever monitor it closes on.
    layoutUiScale = AppSettings::getLayoutUiScale();

    io.Fonts->AddFontDefault();

    ImFontConfig config;
    config.MergeMode = true;
    config.FontDataOwnedByAtlas = false;
    // Merge size 0.0f: inherit the implicit reference size of AddFontDefault().
    // Since 1.92, merging with an explicit size into an implicit-ref-size font asserts,
    // and specifying GlyphMinAdvanceX (monospacing) requires a non-zero reference size,
    // so it is omitted here. The visible icon font is font1 below, which keeps monospacing.
    static const ImWchar icon_ranges[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
    io.Fonts->AddFontFromMemoryTTF(fa_solid_900_ttf, fa_solid_900_ttf_len, 0.0f, &config, icon_ranges);

    ImFontConfig config1;
    strcpy(config1.Name, "roboto-regular (16 px)");
    config1.FontDataOwnedByAtlas = false;
    config1.OversampleH = 2;
    config1.OversampleV = 2;
    config1.RasterizerMultiply = 1.5f;
    ImFont* font1 = io.Fonts->AddFontFromMemoryTTF(roboto_regular_ttf, roboto_regular_ttf_len, 16.0f, &config1);

    ImFontConfig config2;
    config2.MergeMode = true;
    config2.GlyphMinAdvanceX = 16.0f; // Use if you want to make the icon monospaced
    config2.FontDataOwnedByAtlas = false;
    static const ImWchar icon_ranges2[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
    io.Fonts->AddFontFromMemoryTTF(fa_solid_900_ttf, fa_solid_900_ttf_len, 16.0f, &config2, icon_ranges2);

    // Same Arabic subset the engine renders text with, so property fields and the scene
    // tree show Arabic instead of missing-glyph boxes. Since 1.92 the atlas loads glyphs
    // on demand, it only needs a font that carries them.
    ImFontConfig configArabic;
    configArabic.MergeMode = true;
    configArabic.FontDataOwnedByAtlas = false;
    //same rasterizer settings as the font it merges into, or Arabic comes out thinner
    //and fainter than the Latin beside it
    configArabic.OversampleH = 2;
    configArabic.OversampleV = 2;
    configArabic.RasterizerMultiply = 1.5f;
    io.Fonts->AddFontFromMemoryTTF(noto_sans_arabic_ttf, noto_sans_arabic_ttf_len, 16.0f, &configArabic);

    ImFontConfig config3;
    strcpy(config3.Name, "jetbrains-mono-regular");
    config3.FontDataOwnedByAtlas = false;
    config3.OversampleH = 2;
    config3.OversampleV = 2;
    codeFont = io.Fonts->AddFontFromMemoryTTF(jetbrains_mono_regular_ttf, jetbrains_mono_regular_ttf_len, 16.0f, &config3);

    //scripts can hold Arabic string literals
    ImFontConfig configCodeArabic;
    configCodeArabic.MergeMode = true;
    configCodeArabic.FontDataOwnedByAtlas = false;
    configCodeArabic.OversampleH = 2;
    configCodeArabic.OversampleV = 2;
    io.Fonts->AddFontFromMemoryTTF(noto_sans_arabic_ttf, noto_sans_arabic_ttf_len, 16.0f, &configCodeArabic);

    io.FontDefault = font1;

    io.ConfigDragClickToInputText = true;

    //io.BackendFlags |= ImGuiBackendFlags_HasSetMousePos;

    //ImGui::StyleColorsDark();
    // Setters only store state, so they run before apply() for the first paint.
    Theme::setPreset(presetFromSetting(AppSettings::getThemePreset()));
    Theme::setAccentColor(AppSettings::getAccentColor().c_str());
    Theme::apply();

    // Fonts follow each viewport's monitor DPI. Style paddings are global, so
    // App::show() re-scales them from the main viewport when that DPI changes.
    io.ConfigDpiScaleFonts = true;
    io.ConfigDpiScaleViewports = true;

    float dpiScale = 1.0f;
    if (ImGui::GetPlatformIO().Monitors.Size > 0) {
        dpiScale = ImGui::GetPlatformIO().Monitors[0].DpiScale;
    }
    Theme::setUiScale(AppSettings::getUiScale());
    Theme::applyDpiScale(dpiScale);

    mcpServer->applySettings(AppSettings::getMcpSettings());
    g_commandPaletteWindow->setCustomCommands(buildPaletteCommands());
    // Palette ids are the numeric AppMenuCommand values, so the callback reuses
    // the exact menu dispatch. Non-numeric ids (the AI tools the palette adds on
    // its own) need arguments the palette can't collect, so they are left alone.
    g_commandPaletteWindow->setExecuteCallback([this](const std::string& actionId) {
        char* end = nullptr;
        const unsigned long id = std::strtoul(actionId.c_str(), &end, 10);
        if (end && *end == '\0' && end != actionId.c_str()) {
            PlatformMenuCommand command;
            command.id = static_cast<uint32_t>(id);
            executeMenuCommand(command);
        }
    });

    WelcomeCallbacks welcomeCallbacks;
    welcomeCallbacks.openProjectDialog = [this]() { openProjectFunc(); };
    welcomeCallbacks.createEmptyProject = [this]() {
        PlatformMenuCommand command;
        command.id = static_cast<uint32_t>(AppMenuCommand::NewProject);
        executeMenuCommand(command);
    };
    welcomeCallbacks.openRecentProject = [this](const std::filesystem::path& projectPath) {
        PlatformMenuCommand command;
        command.id = static_cast<uint32_t>(AppMenuCommand::OpenRecentProject);
        command.payload = projectPath.string();
        executeMenuCommand(command);
    };
    // Same deferral the other welcome actions use: the panel is drawn inside the
    // UI pass, and switching projects there tears down windows mid-frame.
    welcomeCallbacks.createFromTemplate = [this](const std::filesystem::path& templateName) {
        PlatformMenuCommand command;
        command.id = static_cast<uint32_t>(AppMenuCommand::NewProjectFromTemplate);
        command.payload = templateName.string();
        executeMenuCommand(command);
    };
    welcomeCallbacks.openDocumentation = []() {
        ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
        if (platform_io.Platform_OpenInShellFn) {
            platform_io.Platform_OpenInShellFn(ImGui::GetCurrentContext(), "https://quantumengine.org");
        }
    };
    g_welcomeScreenWindow->setCallbacks(welcomeCallbacks);
    g_welcomeScreenWindow->setTemplatesDirectory(FileUtils::getTemplatesDir());

    // First-run screen only: never persisted, so it appears solely while there is
    // no recent project to fall back to.
    g_welcomeScreenWindow->setOpen(AppSettings::getRecentProjects().empty());
}

bool editor::App::canEditSelection(bool duplicate) {
    // Enabled items keep their native key equivalent, stealing it from a focused text field.
    if (ImGui::GetIO().WantTextInput) return false;
    if (lastFocusedWindow != LastFocusedWindow::AnySceneWindow || project.isAnyScenePlaying()) return false;
    const uint32_t sceneId = project.getSelectedSceneId();
    uint32_t targetSceneId = sceneId;
    const uint32_t propertiesSceneId = project.getSelectedSceneForProperties();
    if (!duplicate && propertiesSceneId != NULL_PROJECT_SCENE &&
        (propertiesSceneId == sceneId || project.hasChildScene(sceneId, propertiesSceneId))) {
        targetSceneId = propertiesSceneId;
    }
    SceneProject* scene = project.getScene(targetSceneId);
    if (!scene) return false;
    if (scene->sceneRender && (scene->sceneRender->getSelectedTileIndex() >= 0 ||
        (!duplicate && scene->sceneRender->getSelectedInstanceIndex() >= 0))) return true;
    if (!project.getSelectedEntities(targetSceneId).empty()) return true;
    return !duplicate && propertiesSceneId != NULL_PROJECT_SCENE && propertiesSceneId != sceneId &&
        project.hasChildScene(sceneId, propertiesSceneId);
}

void editor::App::deleteSelection() {
    if (!canEditSelection(false)) return;
    const uint32_t sceneId = project.getSelectedSceneId();
    uint32_t selectedSceneForProperties = project.getSelectedSceneForProperties();
    uint32_t targetSceneId = sceneId;
    if (selectedSceneForProperties != NULL_PROJECT_SCENE &&
        (selectedSceneForProperties == sceneId || project.hasChildScene(sceneId, selectedSceneForProperties))) {
        targetSceneId = selectedSceneForProperties;
    }

    // Check if a tile is selected — delete it instead of the entity
    SceneProject* sp = project.getScene(targetSceneId);
    bool tileDeleted = false;
    bool instanceDeleted = false;
    if (sp && sp->sceneRender) {
        int tileIdx = sp->sceneRender->getSelectedTileIndex();
        Entity tileEntity = sp->sceneRender->getSelectedTileEntity();
        if (tileIdx >= 0) {
            Command* deleteCmd = ProjectUtils::buildDeleteTileCmd(&project, targetSceneId, tileEntity, (unsigned int)tileIdx);
            if (deleteCmd) {
                CommandHandle::get(sceneId)->addCommand(deleteCmd);
                sp->sceneRender->clearTileSelection();
                tileDeleted = true;
            }
        }

        int instIdx = sp->sceneRender->getSelectedInstanceIndex();
        Entity instEntity = sp->sceneRender->getSelectedInstanceEntity();
        if (!tileDeleted && instIdx >= 0) {
            Command* deleteCmd = ProjectUtils::buildDeleteInstanceCmd(&project, targetSceneId, instEntity, (unsigned int)instIdx);
            if (deleteCmd) {
                CommandHandle::get(sceneId)->addCommand(deleteCmd);
                sp->sceneRender->clearInstanceSelection();
                instanceDeleted = true;
            }
        }
    }

    if (!tileDeleted && !instanceDeleted) {
        const std::vector<Entity>& selectedEntities = project.getSelectedEntities(targetSceneId);

        Command* lastCmd = nullptr;
        if (!selectedEntities.empty()) {
            for (const Entity& entity : selectedEntities){
                lastCmd = new DeleteEntityCmd(&project, targetSceneId, entity);
                CommandHandle::get(sceneId)->addCommand(lastCmd);
            }
        } else {
            if (selectedSceneForProperties != NULL_PROJECT_SCENE &&
                selectedSceneForProperties != sceneId &&
                project.hasChildScene(sceneId, selectedSceneForProperties)) {
                    lastCmd = new RemoveChildSceneCmd(&project, sceneId, selectedSceneForProperties);
                    CommandHandle::get(sceneId)->addCommand(lastCmd);
            }
        }
        if (lastCmd) {
            lastCmd->setNoMerge();
        }
    }
}

void editor::App::duplicateSelection() {
    if (!canEditSelection(true)) return;
    const uint32_t sceneId = project.getSelectedSceneId();
    uint32_t targetSceneId = sceneId;
    SceneProject* sp = project.getScene(targetSceneId);
    bool tileDuplicated = false;
    if (sp && sp->sceneRender) {
        int tileIdx = sp->sceneRender->getSelectedTileIndex();
        Entity tileEntity = sp->sceneRender->getSelectedTileEntity();
        if (tileIdx >= 0) {
            Command* dupCmd = ProjectUtils::buildDuplicateTileCmd(&project, targetSceneId, tileEntity, (unsigned int)tileIdx);
            if (dupCmd) {
                CommandHandle::get(sceneId)->addCommand(dupCmd);
                TilemapComponent* tilemap = sp->scene->findComponent<TilemapComponent>(tileEntity);
                if (tilemap) {
                    sp->sceneRender->selectTile(tileEntity, (int)tilemap->numTiles - 1);
                }
                tileDuplicated = true;
            }
        }
    }
    if (!tileDuplicated) {
        const std::vector<Entity>& selectedEntities = project.getSelectedEntities(targetSceneId);
        if (!selectedEntities.empty()){
            CommandHandle::get(sceneId)->addCommandNoMerge(new DuplicateEntityCmd(&project, targetSceneId, selectedEntities));
        }
    }
}

void editor::App::show(){
    applyUiScale();

    // Actions deferred by the previous pass (project swaps triggered from the
    // palette, the welcome screen or alerts) run now, before this pass records
    // any draw command that could reference a texture they destroy.
    processDeferredUiActions();

    float dpiScale = 1.0f;
    if (const ImGuiViewport* mainViewport = ImGui::GetMainViewport()) {
        dpiScale = mainViewport->DpiScale;
    }
    Theme::applyDpiScale(dpiScale);

    if (hubMode) {
        // Hub mode: the launcher owns the window. No dockspace, menu or editor
        // panels; projects are opened by spawning separate editor processes.
        hubWindow.show();
        uiPassActive = false;
        return;
    }

    if (projectLoading) {
        if (isInitialized) {
            ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_KeepAliveOnly);
        }
        loadingWindow->show();
        return;
    }

    // Scripts ran in engineRender, so drop what they destroyed before any window reads the selection
    for (SceneProject& sceneProject : project.getScenes()) {
        const bool running = sceneProject.playState == ScenePlayState::PLAYING || sceneProject.playState == ScenePlayState::PAUSED;
        if (running && sceneProject.scene) {
            project.deselectDestroyedEntities(&sceneProject);
        }
    }

    if (!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        if (resourcesWindow->isFocused()) {
            lastFocusedWindow = LastFocusedWindow::Resources;
        } else if (codeEditor->isFocused()) {
            lastFocusedWindow = LastFocusedWindow::Code;
        } else if (aiChatWindow->isFocused()) {
            lastFocusedWindow = LastFocusedWindow::AI;
        }else{
            lastFocusedWindow = LastFocusedWindow::AnySceneWindow;
        }
    }

    // Every chord below is read through the registry, so what Editor Settings > Shortcuts
    // shows is what fires. Undo and Redo are computed once and consumed by the two panels
    // further down, which is also why they are not tested where they are used:
    // IsKeyPressed() is an edge, and only the first reader of a frame sees it.
    bool isUndo = editor::KeyBindings::pressed("edit.undo");
    bool isRedo = editor::KeyBindings::pressed("edit.redo");

    const bool popupOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    if (!popupOpen) {
        // Exact modifiers make Save and Save All disjoint chords, so the order here does
        // not matter; the old code tested Ctrl+S once and branched on Shift.
        if (editor::KeyBindings::pressed("file.saveAll")) {
            // Save scenes, scripts, and project configuration together.
            saveAllFunc();
        } else if (editor::KeyBindings::pressed("file.save")) {
            saveFunc();
        }

        if (editor::KeyBindings::pressed("file.openProject")) {
            // CTRL+O opens a project
            openProjectFunc();
        }

        // The palette stays live while a text field has the caret, so it can be opened
        // from the search box itself.
        if (editor::KeyBindings::pressed("view.commandPalette")) {
            g_commandPaletteWindow->toggle();
        }

        if (editor::KeyBindings::pressed("view.terminal")) {
            executeMenuCommand({ static_cast<uint32_t>(AppMenuCommand::ToggleTerminal), {} });
        }

        // Play/Pause/Stop shortcuts
        {
            SceneProject* selectedScene = project.getSelectedScene();
            uint32_t selectedSceneId = project.getSelectedSceneId();
            bool hasSelectedScene = selectedScene != nullptr;
            bool isPlaying = hasSelectedScene && selectedScene->playState == ScenePlayState::PLAYING;
            bool isPaused = hasSelectedScene && selectedScene->playState == ScenePlayState::PAUSED;
            bool isLoading = hasSelectedScene && selectedScene->playState == ScenePlayState::LOADING;

            if (editor::KeyBindings::pressed("scene.run")) {
                if (hasSelectedScene && !project.isAnyScenePlaying()) {
                    project.start(selectedSceneId);
                } else if (isPaused) {
                    project.resume(selectedSceneId);
                }
            }
            if (editor::KeyBindings::pressed("scene.pause")) {
                if (isPlaying) {
                    project.pause(selectedSceneId);
                }
            }
            if (editor::KeyBindings::pressed("scene.stop")) {
                if (isPlaying || isPaused || isLoading) {
                    project.stop(selectedSceneId);
                }
            }
            if (editor::KeyBindings::pressed("scene.detachCamera")) {
                project.toggleCameraDetached(selectedSceneId);
            }
        }
        if (editor::KeyBindings::pressed("help.documentation")) {
            executeMenuCommand({static_cast<uint32_t>(AppMenuCommand::Documentation), {}});
        }
        // The three settings tabs ship unbound; a chord assigned in Editor Settings >
        // Shortcuts makes them fire with no other change here.
        if (editor::KeyBindings::pressed("settings.shortcuts")) {
            executeMenuCommand({static_cast<uint32_t>(AppMenuCommand::SettingsShortcutsTab), {}});
        }
        if (editor::KeyBindings::pressed("settings.graphics")) {
            executeMenuCommand({static_cast<uint32_t>(AppMenuCommand::SettingsGraphicsTab), {}});
        }
        if (editor::KeyBindings::pressed("settings.api")) {
            executeMenuCommand({static_cast<uint32_t>(AppMenuCommand::SettingsEngineApiTab), {}});
        }
    }

    if (isDroppedExternalPaths) {
        isDroppedExternalPaths = false;
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceExtern)) {
            std::vector<char> payload = Util::encodeStringsPayload(droppedExternalPaths);
            ImGui::SetDragDropPayload("external_files", payload.data(), payload.size());
            ImGui::EndDragDropSource();
        }
    }

    if (!ImGui::IsAnyItemActive() && !ImGui::IsAnyItemFocused()){
        // space to keys events
    }

    if (!popupOpen && !resourcesWindow->isFocused() && !codeEditor->isFocused() && !aiChatWindow->isFocused()){
        uint32_t sceneId = project.getSelectedSceneId();

        // Update the Undo and Redo button logic:
        if (isUndo) {
            CommandHandle::get(sceneId)->undo();
        }
        if (isRedo) {
            CommandHandle::get(sceneId)->redo();
        }

        if (editor::KeyBindings::pressed("edit.delete")) deleteSelection();

        if (editor::KeyBindings::pressed("edit.duplicate")) duplicateSelection();

        if (editor::KeyBindings::pressed("inspector.rename")) propertiesWindow->focusNameInput();
    }

    if (!popupOpen && resourcesWindow->isFocused()) {
        if (isUndo) {
            project.getProjectCommandHistory()->undo();
            resourcesWindow->refreshCurrentDirectory();
        }
        if (isRedo) {
            project.getProjectCommandHistory()->redo();
            resourcesWindow->refreshCurrentDirectory();
        }
    }

    // Created on the first frame, once the ImGui context exists: the library's
    // AppContext is a real object with its own model, not a POD.
    if (!uilabHost) {
        uilabHost.reset(new UILabHost(*this, structureWindow, propertiesWindow, resourcesWindow));
        dockspaceNeedsRebuild = true;
    }
    dockspace_id = uilabHost->dockSpaceId();

    gitMonitor.update(project.getProjectPath(), project.hasVersionControlMetadata());

    showMenu();
    showFooter();

    // Windows below record draw commands, and the palette/welcome overlays run
    // their callbacks after recording; nothing may tear down the project from
    // here until the end of the pass (see uiPassActive).
    uiPassActive = true;

    isInitialized = true;

    // Before the library's host window lays the restored sizes out.
    rescaleRestoredLayout();

    if (dockspaceNeedsRebuild || ImGui::DockBuilderGetNode(dockspace_id) == nullptr) {
        buildDockspace();
        dockspaceNeedsRebuild = false;
    }

    // The library's host window carries the dockspace: same root node id, same
    // PassthruCentralNode behaviour as DockSpaceOverViewport had.
    uilabHost->beginFrame();

    #ifdef SHOW_STYLE_WINDOW
    showStyleEditor();
    #endif

    showAlert();

    sceneSaveDialog.show();
    projectSaveDialog.show();
    exportWindow.show();
    editorSettingsWindow.show();
    aboutWindow.show();
    projectSettingsWindow.show();
    bundlesWindow.show();
    scenesWindow.show();
    networkPlayWindow.show();
    scriptDebuggerWindow.show();

    if (!saveDialogInProgress && !sceneSaveDialog.isOpen() && !projectSaveDialog.isOpen() && !saveDialogQueue.empty()) {
        processNextSaveDialog();
    }

    structureWindow->show();
    resourcesWindow->show();
    imageViewerWindow->show();
    modelViewerWindow->show();
    materialBrowserWindow->show();
    outputWindow->show();
    if (uilabHost)
        uilabHost->drawConsole();
    animationWindow->show();
    terrainEditWindow->show();
    propertiesWindow->show();
    // The three panels above draw their contents inside windows the library opened.
    if (uilabHost)
        uilabHost->drawDataPanels();
    aiChatWindow->show();
    codeEditor->show();
    sceneWindow->show();

    // Feed the monitor from cheap App-side numbers only while it is visible.
    static double lastMonitorFeed = 0.0;
    if (g_resourceMonitorWindow->isOpen() && ImGui::GetTime() - lastMonitorFeed > 0.5) {
        lastMonitorFeed = ImGui::GetTime();
        g_resourceMonitorWindow->setCounter("Scene", "Scenes",
                                            static_cast<double>(project.getScenes().size()), 0.0);
        g_resourceMonitorWindow->setCounter("Resources", "Queued",
                                            static_cast<double>(Engine::getQueuedResourceCount()), 0.0);
        const uint32_t selectedSceneId = project.getSelectedSceneId();
        if (const SceneProject* scene = project.getScene(selectedSceneId)) {
            g_resourceMonitorWindow->setCounter("GPU", "Shaders",
                                                static_cast<double>(scene->shaderKeys.size()), 0.0);
            g_resourceMonitorWindow->setCounter("Scene", "Entities",
                                                static_cast<double>(project.getEntities(selectedSceneId).size()), 0.0);
        }
    }

    g_resourceMonitorWindow->show();
    g_graphEditorWindow->show();
    g_terminalWindow->show();
    g_modelExportWindow->show();
    g_pluginBrowserWindow->show();
    // The plugin windows a loaded plugin registered, described fresh each frame by the plugin itself.
    g_pluginWindowHost->show();
    g_worldCreatorWindow->show();
    g_notificationsWindow->show();

    loadingWindow->show();

    // Overlay windows draw after the docked ones so they float above the dockspace.
    g_welcomeScreenWindow->show();
    g_commandPaletteWindow->show();

    // Keep the persisted tab list in sync with the live tab order so a user's
    // drag-reordering of scene/code tabs is saved (and restored on next launch).
    captureTabOrder();

    // Closing a panel through its title-bar button changes the same state as the
    // View menu, so mirror the live state after all panels have rendered.
    persistPanelVisibilitySettings();

    // The panels above are a fixed set of names, so their state is a struct field. A plugin's windows
    // are not, so their open list is read back from the host - and only when it actually changed,
    // because this is the frame path. The prime is repeated here because buildMenuModel() can return
    // before it reaches the Plugins menu, and this is the only place guaranteed to run.
    g_pluginWindowHost->restoreWantedWindows(AppSettings::getOpenPluginWindows());
    if (g_pluginWindowHost->takeVisibilityChange()) {
        AppSettings::setOpenPluginWindows(g_pluginWindowHost->wantedWindowIds());
    }

    uiPassActive = false;
}

void editor::App::setLoadingPump(std::function<void(bool)> pump) {
    loadingPump = std::move(pump);
}

void editor::App::reportLoadingProgress(const std::string& status) {
    if (!isMainThread() || !projectLoading || pumpingLoading) return;
    if (!status.empty()) loadingStatus = status;
    if (!loadingPump) return;

    const auto now = std::chrono::steady_clock::now();
    if (lastLoadingFrame.time_since_epoch().count() != 0 &&
        now - lastLoadingFrame < std::chrono::milliseconds(33)) return;
    lastLoadingFrame = now;

    // Project loading stays on the main thread. Pump only native events and the
    // loading UI here; editor tasks must wait until the project is complete.
    pumpingLoading = true;
    try {
        loadingPump(Engine::isViewLoaded());
    } catch (...) {
        pumpingLoading = false;
        throw;
    }
    pumpingLoading = false;
}

static std::string jsonEscape(const std::string& text) {
    std::ostringstream out;
    for (unsigned char c : text) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20)
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (int)c << std::dec;
                else
                    out << c;
        }
    }
    return out.str();
}

void editor::App::parseBenchmarkArgs(int argc, char** argv) {
    if (argc < 2 || std::strcmp(argv[1], "benchmark") != 0)
        return;

    benchmarkEnabled = true;
    bool sawFrames = false;
    bool sawWarmup = false;
    auto requireValue = [&](int& index, const char* name) -> const char* {
        if (index + 1 >= argc) {
            failBenchmark(std::string("missing value for ") + name);
            return nullptr;
        }
        return argv[++index];
    };

    for (int i = 2; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "-p" || arg == "--project") {
            if (const char* value = requireValue(i, "--project"))
                benchmarkProject = std::filesystem::absolute(value);
        } else if (arg == "-s" || arg == "--scene") {
            if (const char* value = requireValue(i, "--scene"))
                benchmarkScene = value;
        } else if (arg == "--frames") {
            if (const char* value = requireValue(i, "--frames")) {
                benchmarkMeasureFrames = std::max(1, std::atoi(value));
                sawFrames = true;
            }
        } else if (arg == "--warmup") {
            if (const char* value = requireValue(i, "--warmup")) {
                benchmarkWarmupFrames = std::max(0, std::atoi(value));
                sawWarmup = true;
            }
        } else if (arg == "--screenshot") {
            if (const char* value = requireValue(i, "--screenshot"))
                benchmarkScreenshot = std::filesystem::path(value);
        } else if (arg == "--mesh-lod") {
            if (const char* value = requireValue(i, "--mesh-lod"))
                benchmarkMeshLod = (std::strcmp(value, "off") != 0 && std::strcmp(value, "0") != 0) ? 1 : 0;
        } else if (arg == "--depth-prepass") {
            if (const char* value = requireValue(i, "--depth-prepass"))
                benchmarkDepthPrepass = (std::strcmp(value, "off") != 0 && std::strcmp(value, "0") != 0) ? 1 : 0;
        } else if (arg == "--name") {
            if (const char* value = requireValue(i, "--name"))
                benchmarkName = value;
        } else if (arg == "--out") {
            if (const char* value = requireValue(i, "--out"))
                benchmarkOut = value;
        } else if (arg == "-h" || arg == "--help") {
            continue;
        } else {
            failBenchmark("unknown argument '" + arg + "'");
            return;
        }
    }

    if (benchmarkProject.empty()) {
        failBenchmark("--project is required");
    }

    if (!benchmarkScreenshot.empty()) {
        // Parsed before the engine starts, so the working directory is still the shell the command
        // was typed in - the same rule the standalone runner applies to --screenshot. The error-code
        // form because a throw here has no handler in a _HAS_EXCEPTIONS=0 build.
        std::error_code cwdError;
        const fs::path cwd = fs::current_path(cwdError);
        if (benchmarkScreenshot.is_relative() && !cwdError)
            benchmarkScreenshot = cwd / benchmarkScreenshot;
        benchmarkScreenshot = benchmarkScreenshot.lexically_normal();
        // The picture is served on the frame after the request, so a capture run needs two measured
        // frames; the long FPS sample it would otherwise inherit is pointless here.
        if (!sawFrames)
            benchmarkMeasureFrames = 2;
        if (!sawWarmup)
            benchmarkWarmupFrames = 30;
    }
}

// the project's start scene unless --scene names another; true once it is the selected one
bool editor::App::selectBenchmarkScene() {
    fs::path scenePath = benchmarkScene;
    if (scenePath.is_relative())
        scenePath = project.getProjectPath() / scenePath;

    SceneProject* match = nullptr;
    for (SceneProject& sceneProject : project.getScenes()) {
        const bool wanted = benchmarkScene.empty()
            ? sceneProject.id == project.getStartSceneId()
            : (sceneProject.name == benchmarkScene || sceneProject.filepath.filename() == benchmarkScene ||
               project.getProjectPath() / sceneProject.filepath == scenePath);
        if (wanted) {
            match = &sceneProject;
            break;
        }
    }
    if (!match)
        return false;

    benchmarkSceneId = match->id;
    if (!match->opened)
        project.openScene(match->filepath, false);
    else
        project.setSelectedSceneId(benchmarkSceneId);

    SceneProject* selected = project.getSelectedScene();
    return selected && selected->id == benchmarkSceneId;
}

void editor::App::failBenchmark(const std::string& message) {
    std::cerr << "benchmark: " << message << "\n";
    benchmarkFailed = true;
    benchmarkExit = true;
}

bool editor::App::consumeBenchmarkExit() {
    const bool exit = benchmarkExit;
    benchmarkExit = false;
    return exit;
}

void editor::App::engineInit(int argc, char** argv) {
    CrashHandler::setLastPhase("editor.engine-init");
    parseBenchmarkArgs(argc, argv);
    reportLoadingProgress("Initializing engine...");
    Engine::systemInit(argc, argv, new editor::Platform(&project));

    // Hub mode: the launcher manages projects and installs and spawns separate
    // editor processes; it never loads a project itself.
    if (editor::LaunchOptions::get().hub) {
        hubMode = true;
        Backend::updateWindowTitle("Quantum Hub");
        Engine::pauseGameEvents(true);
        Engine::setAsyncLoading(true);
        return;
    }

    Engine::pauseGameEvents(true);

    // Any edit/undo/redo flags its scene for redraw, so the loop can idle otherwise.
    CommandHistory::onSceneModified = [this](size_t sceneId, bool structural){
        if (SceneProject* sceneProject = project.getScene((uint32_t)sceneId)){
            sceneProject->needUpdateRender = true;
            // Only structural commands, so a gizmo drag doesn't rebuild the tree each frame.
            if (structural){
                sceneProject->structureVersion++;
            }
        }
    };

    ShaderPool::setShaderBuilder([this](quantum::ShaderKey shaderKey) -> quantum::ShaderBuildResult {
        static quantum::editor::ShaderBuilder builder;  // Make static to reuse
        return builder.buildShader(shaderKey, &project);
    });

    Engine::setAsyncLoading(true);
}

void editor::App::engineViewLoaded(){
    reportLoadingProgress("Initializing graphics...");
    Engine::systemViewLoaded();
}

void editor::App::loadStartupProject() {
    CrashHandler::setLastPhase("editor.project-loading");
    lastLoadingFrame = {};
    reportLoadingProgress("Loading project...");

    // a failed benchmark still runs one frame: the main loop exits on benchmarkExit, and
    // closing before the first frame would leave the render queue waiting on shutdown
    if (benchmarkEnabled) {
        if (benchmarkExit)
            return;

        fs::path projectPath = benchmarkProject;
        if (fs::is_regular_file(projectPath) && projectPath.filename() == "project.yaml")
            projectPath = projectPath.parent_path();

        if (!project.loadProject(projectPath, false)) {
            failBenchmark("failed to load project '" + benchmarkProject.string() + "'");
            return;
        }

        if (!selectBenchmarkScene()) {
            failBenchmark(benchmarkScene.empty() ? "the project has no start scene" : "scene '" + benchmarkScene + "' is not in the project");
            return;
        }

        project.setVSyncEnabled(false);
        if (SceneProject* selected = project.getScene(benchmarkSceneId)) {
            selected->displaySettings.showGrid3D = false;
            selected->displaySettings.showOrigin = false;
            selected->displaySettings.hideSelectionOutline = true;
        }
        projectLoading = false;
        requestRedraw();
        return;
    }

    // Hub mode: the launcher manages projects and installs and spawns separate
    // editor processes; it never loads a project itself.
    if (hubMode)
        return;

    const editor::LaunchOptions& launch = editor::LaunchOptions::get();

    if (!launch.projectPath.empty()) {
        // A launcher (Hub, desktop shortcut) opened or created this project.
        std::error_code ec;
        if (std::filesystem::exists(launch.projectPath / "project.yaml", ec)) {
            if (!project.loadProject(launch.projectPath)) {
                Out::error("Failed to open project: \"%s\"", launch.projectPath.string().c_str());
                project.createTempProject("MyQuantumProject");
            }
        } else if (launch.create) {
            const std::string projectName = launch.projectPath.filename().string();
            const bool created = launch.templateName.empty()
                ? project.createNewProjectAt(launch.projectPath, projectName)
                : project.createFromTemplateAt(launch.projectPath, launch.templateName);
            if (!created) {
                Out::error("Failed to create project: \"%s\"", launch.projectPath.string().c_str());
                project.createTempProject("MyQuantumProject");
            }
        } else {
            Out::error("Not a Quantum project: \"%s\" (no project.yaml)", launch.projectPath.string().c_str());
            project.createTempProject("MyQuantumProject");
        }
    } else if (!safeMode) {
        // Check if there's a last opened project
        std::filesystem::path lastProjectPath = AppSettings::getLastProjectPath();

        if (!lastProjectPath.empty() && std::filesystem::exists(lastProjectPath)) {
            // Try to load the last project
            if (project.loadProject(lastProjectPath)) {
                Out::info("Loaded last opened project: \"%s\"", lastProjectPath.string().c_str());
            } else {
                // If loading fails, create a new temp project
                project.createTempProject("MyQuantumProject");
            }
        } else {
            // No last project, create a new temp project
            project.createTempProject("MyQuantumProject");
        }
    } else {
        Out::warning("Recovery mode skipped the last opened project");
        project.createTempProject("Recovery Project");
    }

    projectLoading = false;
    CrashHandler::setLastPhase("editor.project-ready");
    // The Project menu shows the association as a checked item, so its state is read
    // once here rather than on every menu rebuild.
    g_projectFileTypeRegistered = editor::cli::ProjectFileType::isRegistered();
    requestRedraw();
}

bool editor::App::requestProjectChange(std::function<void()> change) {
    if (projectLoading || pendingProjectChange) return false;
    pendingProjectChange = std::move(change);
    requestRedraw();
    return true;
}

void editor::App::processProjectChange() {
    if (projectLoading || !pendingProjectChange) return;
    auto change = std::move(pendingProjectChange);
    pendingProjectChange = nullptr;
    if (project.isAnyScenePlaying()) return;

    // Menu and dialog callbacks queue the change. Run it between frames so the
    // loading pump can draw without nesting ImGui frames or invalidating scene UI.
    projectLoading = true;
    lastLoadingFrame = {};
    Backend::setMainMenu({}, {});
    try {
        reportLoadingProgress("Loading project...");
        change();
    } catch (const std::exception& e) {
        Out::error("Failed to switch projects: %s", e.what());
        registerAlert("Error", "Failed to switch projects.");
    }
    projectLoading = false;
    requestRedraw();
}

void editor::App::engineRender(){
    if (projectLoading) {
        SystemRender::executeQueue();
        return;
    }

    processMainThreadTasks();
    project.refreshLinkedMaterials();
    project.refreshFbxImports();
    project.updateGeneratedSources();
    renderedSceneThisFrame = false;
    const uint32_t selectedSceneId = project.getSelectedSceneId();

    // Redraw the selected scene only when it changed (needUpdateRender) or while a
    // continuous state (play/load/preview/thumbnail) is active, so the loop can idle.
    const bool thumbnailsPending = resourcesWindow && resourcesWindow->hasPendingThumbnailWork();

    // Drop targets preview a dragged resource directly, with no command to flag the scene.
    const ImGuiPayload* dragPayload = ImGui::GetDragDropPayload();
    const bool draggingResourceFile = dragPayload && dragPayload->IsDataType("resource_files");

    // Keep drawing a few frames after the last change so late async uploads and
    // shader builds still get presented.
    const int RENDER_SETTLE_FRAMES = 30;

    auto sceneNeedsRender = [&](SceneProject& sp, bool isSelected) -> bool {
        bool active = sp.needUpdateRender;
        bool gaugeAnimating = false;
        if (benchmarkEnabled) active = true;
        // A running play session must advance and redraw its simulation every frame.
        if (sp.playState == ScenePlayState::PLAYING) active = true;
        // A drag preview redraws until the settle frames cover its restore.
        if (draggingResourceFile) active = true;
        if (isSelected && sp.scene && sp.sceneRender) {
            // Still loading a model, previewing a camera, or capturing a thumbnail.
            auto ms = sp.scene->getSystem<MeshSystem>();
            if (ms && (ms->hasPendingAsyncModelLoads() || ms->hasPendingFoliageUpdates())) active = true;
            // A parsed model still has per-mesh work that only runs while drawing.
            auto rs = sp.scene->getSystem<RenderSystem>();
            if (rs && !rs->isAllLoaded()) active = true;
            // isAllLoaded skips empty UIs, and an image builds its geometry only once its texture loads
            auto us = sp.scene->getSystem<UISystem>();
            if (us && us->hasPendingImages()) active = true;
            if (sp.sceneRender->isPreviewCameraActive()) active = true;
            gaugeAnimating = sp.sceneRender->getUILayer()->isCameraGaugeAnimating();
            if (thumbnailsPending) active = true;
            // Preview scenes (shape, material, direction) only draw inside systemDraw().
            if (Engine::hasScenesToExecuteOnce()) active = true;
        }

        if (active) {
            sp.renderSettleFrames = RENDER_SETTLE_FRAMES;
            return true;
        }
        if (gaugeAnimating) {
            if (sp.renderSettleFrames > 0) sp.renderSettleFrames--;
            return true;
        }
        if (sp.renderSettleFrames > 0) {
            sp.renderSettleFrames--;
            return true;
        }
        return false;
    };

    for (auto& sceneProject : project.getScenes()) {
        if (!sceneProject.opened) continue;
        if (!sceneProject.scene || !sceneProject.sceneRender) continue;
        if (sceneProject.playState == ScenePlayState::SAVING || sceneProject.playState == ScenePlayState::LOADING || sceneProject.playState == ScenePlayState::CANCELLING) continue;

        const bool isSelected = (sceneProject.id == selectedSceneId);

        auto meshSystem = sceneProject.scene->getSystem<MeshSystem>();
        bool hasPendingModelLoads = meshSystem && meshSystem->hasPendingAsyncModelLoads();

        // A tab hidden behind another has nothing to present, and drawing it makes
        // lastActivatedScene alternate between tabs. The selected one always draws.
        const bool hiddenTab = !sceneProject.isVisible && !isSelected
            && sceneProject.playState != ScenePlayState::PLAYING;
        if (hiddenTab || (hasPendingModelLoads && !sceneProject.needUpdateRender && !isSelected)) {
            if (hasPendingModelLoads) meshSystem->update(0);
            continue;
        }

        if (sceneNeedsRender(sceneProject, isSelected)){
            int width = sceneWindow->getWidth(sceneProject.id);
            int height = sceneWindow->getHeight(sceneProject.id);

            // Not laid out yet (background tab): nothing to draw, and activating it here
            // would thrash lastActivatedScene. Stays dirty until SceneWindow gives it a size.
            if (width == 0 || height == 0) continue;

            SceneRender* sceneRender = sceneProject.sceneRender;

            // The viewport render distance is the editor's own view: it lives in settings.yaml,
            // so it is pushed onto the live scene each frame rather than authored into it, and
            // Play drops it so what you see while running is what the game really draws.
            const float viewportRenderDistance =
                sceneProject.playState == ScenePlayState::PLAYING
                    ? 0.0f : AppSettings::getViewportRenderDistance();
            sceneProject.scene->setRenderDistanceOverride(viewportRenderDistance);

            bool sceneChanged = false;

            // Collect loaded child scene layers (skip if playing — runtime manages its own layers)
            std::vector<Scene*> childLayers;
            if (sceneProject.playState == ScenePlayState::STOPPED) {
                for (const ChildSceneRef& childSceneRef : sceneProject.childScenes) {
                    uint32_t childId = childSceneRef.id;
                    const SceneProject* childScene = project.getScene(childId);
                    if (childScene && childScene->expandedInline && childScene->scene) {
                        childLayers.push_back(childScene->scene);
                    }
                }
            }
            sceneProject.sceneRender->setChildSceneLayers(childLayers);

            // activate() reloads this scene and every layer, so it runs only on a scene
            // change. Anything else needing it calls resetLastActivatedScene().
            if (lastActivatedScene != sceneProject.id){
                // Collected before activate(): it drops every engine layer, and a dropped
                // layer no longer counts as running, so this would come back empty.
                project.storeRuntimeScenes(lastActivatedScene);
                Scene* runtimeMainScene = nullptr;
                std::vector<Scene*> runtimeLayers = project.getRunningRuntimeLayers(sceneProject.id, runtimeMainScene);
                sceneProject.sceneRender->activate();

                if (runtimeMainScene) {
                    Engine::setScene(runtimeMainScene);
                }
                for (Scene* runtimeLayer : runtimeLayers) {
                    Engine::addSceneLayer(runtimeLayer);
                }

                lastActivatedScene = sceneProject.id;
                pendingResizeScene = sceneProject.id;
                sceneChanged = true;
                #ifdef _DEBUG
                printf("DEBUG: Activated scene %u\n", lastActivatedScene);
                #endif
            }

            if (pendingResizeScene == sceneProject.id && width != 0 && height != 0) {
                sceneChanged = true;
                pendingResizeScene = NULL_PROJECT_SCENE;
            }

            if (width != 0 && height != 0){
                sceneRender->setOverlayScale(sceneWindow->getOverlayScale(sceneProject.id));
                Framebuffer* framebuffer = Engine::getFramebuffer();
                const bool framebufferMismatch = !framebuffer
                    || !framebuffer->isCreated()
                    || framebuffer->getWidth() != static_cast<unsigned int>(width)
                    || framebuffer->getHeight() != static_cast<unsigned int>(height);

                if (Platform::setSizes(width, height) || sceneChanged || framebufferMismatch){
                    Engine::systemViewChanged();
                    sceneRender->updateSize(width, height);
                    sceneChanged = false;
                }

                // Update child scene render systems so their entities display correctly
                for (const ChildSceneRef& childSceneRef : sceneProject.childScenes) {
                    uint32_t childId = childSceneRef.id;
                    SceneProject* childScene = project.getScene(childId);
                    if (childScene && childScene->expandedInline && childScene->scene) {
                        childScene->scene->setRenderDistanceOverride(viewportRenderDistance);
                        if (childScene->sceneRender) {
                            childScene->sceneRender->hideAllGizmos();
                        }
                        childScene->scene->getSystem<MeshSystem>()->update(0);
                        childScene->scene->getSystem<UISystem>()->update(0);
                        childScene->scene->getSystem<RenderSystem>()->update(0);
                        childScene->needUpdateRender = false;
                    }
                }

                // to avoid delay when move objects with gizmo
                sceneRender->updateRenderSystem();

                std::vector<Entity> entities = project.getEntities(sceneProject.id);
                // overlays for the game's own entities too while detached
                if (sceneRender->isCameraDetached()){
                    std::vector<Entity> created = project.getPlayCreatedEntities(&sceneProject);
                    entities.insert(entities.end(), created.begin(), created.end());
                }

                //TODO: avoid calling every frame
                sceneRender->update(project.getSelectedEntities(sceneProject.id), entities, sceneProject.mainCamera, sceneProject.displaySettings);

                Engine::systemDraw();

                resourcesWindow->processMaterialThumbnails();
                resourcesWindow->processModelThumbnails();
                materialBrowserWindow->processPendingPreviews();

                // Only a real draw counts as activity, or the loop never idles.
                renderedSceneThisFrame = true;
                sceneProject.needUpdateRender = false;
            }
        }
    }

    // A frame resuming after an idle gap measures the gap, not a frame time.
    if (renderedSceneThisFrame && renderedScenePrevFrame){
        footerFramerate = Engine::getFramerate();
        footerDeltaMs = (float)(Engine::getDeltatime() * 1000.0);
    }else{
        footerFramerate = 0.0f;
        footerDeltaMs = 0.0f;
    }
    renderedScenePrevFrame = renderedSceneThisFrame;
    if (benchmarkEnabled)
        tickBenchmark();
}

void editor::App::tickBenchmark() {
    if (benchmarkExit)
        return;

    SceneProject* selected = project.getScene(benchmarkSceneId);
    if (!selected || !selected->scene || !selected->sceneRender) {
        if (benchmarkWaitStart.time_since_epoch().count() == 0)
            benchmarkWaitStart = std::chrono::steady_clock::now();
        if (std::chrono::steady_clock::now() - benchmarkWaitStart > std::chrono::seconds(90)) {
            failBenchmark("timed out waiting for the scene to load");
        }
        return;
    }

    if (!benchmarkCameraReady) {
        // the scene tab decides the selected scene, so bring it to front like Play does
        if (sceneWindow)
            sceneWindow->requestPlayFocus(benchmarkSceneId);
        Entity camera = selected->scene->getCamera();
        if (camera == NULL_ENTITY)
            camera = selected->mainCamera;
        if (selected->sceneRender->setPreviewCamera(camera))
            benchmarkCameraReady = true;
        if (benchmarkMeshLod >= 0)
            selected->scene->setMeshLodEnabled(benchmarkMeshLod != 0);
        if (benchmarkDepthPrepass >= 0)
            selected->scene->setDepthPrepassEnabled(benchmarkDepthPrepass != 0);
        selected->displaySettings.showGrid3D = false;
        selected->displaySettings.showOrigin = false;
        selected->displaySettings.hideSelectionOutline = true;
        selected->sceneRender->hideAllGizmos();
    }

    auto meshSystem = selected->scene->getSystem<MeshSystem>();
    auto renderSystem = selected->scene->getSystem<RenderSystem>();
    auto uiSystem = selected->scene->getSystem<UISystem>();
    const bool loading =
        Engine::getQueuedResourceCount() > 0 ||
        (meshSystem && (meshSystem->hasPendingAsyncModelLoads() || meshSystem->hasPendingFoliageUpdates())) ||
        (renderSystem && (!renderSystem->isAllLoaded() || renderSystem->hasPendingMeshLods())) ||
        (uiSystem && uiSystem->hasPendingImages());

    if (benchmarkPhase == 0) {
        if (benchmarkWaitStart.time_since_epoch().count() == 0)
            benchmarkWaitStart = std::chrono::steady_clock::now();
        if (loading) {
            if (std::chrono::steady_clock::now() - benchmarkWaitStart > std::chrono::seconds(90)) {
                std::cerr << "benchmark: timed out waiting for resources; measuring anyway\n";
                benchmarkPhase = 1;
                benchmarkPhaseFrames = 0;
            }
            return;
        }
        if (!renderedSceneThisFrame || sceneWindow->getWidth(selected->id) <= 0)
            return;
        benchmarkPhase = 1;
        benchmarkPhaseFrames = 0;
        return;
    }

    if (!renderedSceneThisFrame)
        return;

    if (benchmarkPhase == 1) {
        if (++benchmarkPhaseFrames >= benchmarkWarmupFrames) {
            benchmarkPhase = 2;
            benchmarkPhaseFrames = 0;
            benchmarkFps.clear();
            benchmarkDraws.clear();
            benchmarkInstances.clear();
            benchmarkTris.clear();
            benchmarkFixedMs = benchmarkUpdateMs = benchmarkDrawMs = benchmarkCommitMs = benchmarkFrameMs = 0.0;
            benchmarkSystemMs.clear();
            benchmarkMeasureStart = std::chrono::steady_clock::now();
            if (!benchmarkScreenshot.empty()) {
                std::error_code made;
                fs::create_directories(benchmarkScreenshot.parent_path(), made);
                FrameCapture::request(benchmarkScreenshot.generic_string(), 0);
            }
        }
        return;
    }

    if (benchmarkPhase == 2) {
        const Engine::FrameStats& stats = Engine::getFrameStats();
        benchmarkFps.push_back(Engine::getFramerate());
        benchmarkDraws.push_back(stats.drawCalls);
        benchmarkInstances.push_back(stats.instances);
        benchmarkTris.push_back(stats.triangles);
        benchmarkFixedMs += stats.fixedUpdateMs;
        benchmarkUpdateMs += stats.updateMs;
        benchmarkDrawMs += stats.drawSceneMs;
        benchmarkCommitMs += stats.drawCommitMs;
        benchmarkFrameMs += stats.frameMs;
        for (int i = 0; i < stats.timedSystemCount; i++) {
            const Engine::FrameStats::SystemTime& timed = stats.timedSystems[i];
            benchmarkSystemMs[timed.name] += timed.fixedMs + timed.updateMs + timed.drawMs;
        }
        if (static_cast<int>(benchmarkFps.size()) >= benchmarkMeasureFrames)
            finishBenchmark();
    }
}

void editor::App::finishBenchmark() {
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - benchmarkMeasureStart).count();
    const size_t n = benchmarkFps.size();
    double avgFps = 0.0;
    double avgDraws = 0.0;
    double avgInstances = 0.0;
    double avgTris = 0.0;
    for (size_t i = 0; i < n; i++) {
        avgFps += benchmarkFps[i];
        avgDraws += benchmarkDraws[i];
        avgInstances += benchmarkInstances[i];
        avgTris += static_cast<double>(benchmarkTris[i]);
    }
    if (n > 0) {
        avgFps /= static_cast<double>(n);
        avgDraws /= static_cast<double>(n);
        avgInstances /= static_cast<double>(n);
        avgTris /= static_cast<double>(n);
    }

    float minFps = 0.0f;
    float maxFps = 0.0f;
    if (!benchmarkFps.empty()) {
        minFps = *std::min_element(benchmarkFps.begin(), benchmarkFps.end());
        maxFps = *std::max_element(benchmarkFps.begin(), benchmarkFps.end());
    }

    const double wallFps = (elapsed > 0.0 && n > 0) ? static_cast<double>(n) / elapsed : 0.0;

    const double frames = n > 0 ? static_cast<double>(n) : 1.0;
    std::vector<std::pair<std::string, double>> slowest;
    slowest.reserve(benchmarkSystemMs.size());
    for (const auto& entry : benchmarkSystemMs)
        slowest.push_back({entry.first, entry.second / frames});
    std::sort(slowest.begin(), slowest.end(),
              [](const std::pair<std::string, double>& a, const std::pair<std::string, double>& b) {
                  return a.second > b.second;
              });

    SceneProject* selected = project.getScene(benchmarkSceneId);
    const int width = selected && sceneWindow ? sceneWindow->getWidth(selected->id) : 0;
    const int height = selected && sceneWindow ? sceneWindow->getHeight(selected->id) : 0;
    const size_t entities = selected && selected->scene ? selected->scene->getEntityList().size() : 0;

    std::ostringstream json;
    json << std::fixed;
    json.precision(2);
    json << "{\n"
         << "  \"name\": \"" << jsonEscape(benchmarkName.empty() ? (selected ? selected->name : "scene") : benchmarkName) << "\",\n"
         << "  \"scene\": \"" << jsonEscape(selected ? selected->name : benchmarkScene) << "\",\n"
         << "  \"meshLod\": " << ((selected && selected->scene && selected->scene->isMeshLodEnabled()) ? "true" : "false") << ",\n"
         << "  \"depthPrepass\": " << ((selected && selected->scene && selected->scene->isDepthPrepassEnabled()) ? "true" : "false") << ",\n"
         << "  \"viewport\": [" << width << ", " << height << "],\n"
         << "  \"entities\": " << entities << ",\n"
         << "  \"frames\": " << n << ",\n"
         << "  \"warmupFrames\": " << benchmarkWarmupFrames << ",\n"
         << "  \"elapsedSeconds\": " << elapsed << ",\n"
         << "  \"wallFps\": " << wallFps << ",\n"
         << "  \"avgFps\": " << avgFps << ",\n"
         << "  \"minFps\": " << minFps << ",\n"
         << "  \"maxFps\": " << maxFps << ",\n"
         << "  \"avgMs\": " << (wallFps > 0.0 ? 1000.0 / wallFps : 0.0) << ",\n"
         << "  \"fixedUpdateMs\": " << benchmarkFixedMs / frames << ",\n"
         << "  \"updateMs\": " << benchmarkUpdateMs / frames << ",\n"
         << "  \"drawSceneMs\": " << benchmarkDrawMs / frames << ",\n"
         << "  \"commitMs\": " << benchmarkCommitMs / frames << ",\n"
         << "  \"engineFrameMs\": " << benchmarkFrameMs / frames << ",\n"
         // What the editor itself spends outside the engine loop, per measured frame.
         << "  \"editorMs\": " << (wallFps > 0.0 ? 1000.0 / wallFps - benchmarkFrameMs / frames : 0.0) << ",\n"
         << "  \"avgDrawCalls\": " << avgDraws << ",\n"
         << "  \"avgInstances\": " << avgInstances << ",\n"
         << "  \"avgTriangles\": " << avgTris << ",\n"
         << "  \"slowestSystems\": [";
    for (size_t i = 0; i < slowest.size() && i < 8; i++) {
        json << (i ? ", " : "") << "{\"name\": \"" << jsonEscape(slowest[i].first)
             << "\", \"ms\": " << slowest[i].second << "}";
    }
    json << "]\n"
         << "}\n";

    const std::string text = json.str();
    std::cout << text;
    std::cout.flush();
    if (!benchmarkOut.empty()) {
        std::ofstream file(benchmarkOut);
        if (file)
            file << text;
        else
            failBenchmark("failed to write " + benchmarkOut.string());
    }

    benchmarkEnabled = false;
    benchmarkExit = true;
}

void editor::App::runWhenUiIdle(std::function<void()> action) {
    if (uiPassActive) {
        deferredUiActions.push_back(action);
    } else {
        action();
    }
}

void editor::App::processDeferredUiActions() {
    if (deferredUiActions.empty()) return;
    std::vector<std::function<void()>> actions;
    actions.swap(deferredUiActions);
    // An action may queue more work (alert callbacks chain, for instance), and
    // this may run while the pass happened to be marked active: run everything
    // as if outside the pass, restoring the flag afterwards.
    const bool wasActive = uiPassActive;
    uiPassActive = false;
    for (std::function<void()>& action : actions) {
        action();
    }
    uiPassActive = wasActive;
}

void editor::App::enqueueMainThreadTask(std::function<void()> task) {
    if (!task) return;
    std::function<void()> wake;
    {
        std::lock_guard<std::mutex> lock(mainThreadTaskMutex);
        mainThreadTasks.push(std::move(task));
        wake = wakeCallback;
    }
    // Wake an idle backend loop so cross-thread work runs promptly.
    if (wake) wake();
}

bool editor::App::hasPendingMainThreadTasks() {
    std::lock_guard<std::mutex> lock(mainThreadTaskMutex);
    return !mainThreadTasks.empty();
}

// Counts as activity, so the loop also keeps drawing for the idle delay past the
// last request: enough for ImGui to auto-resize a window whose content just grew.
void editor::App::requestRedraw() {
    redrawRequested = true;
}

bool editor::App::consumeRedrawRequest() {
    const bool requested = redrawRequested;
    redrawRequested = false;
    return requested;
}

void editor::App::requestFrame() {
    frameRequested = true;
}

bool editor::App::consumeFrameRequest() {
    const bool requested = frameRequested;
    frameRequested = false;
    return requested;
}

void editor::App::setWakeCallback(std::function<void()> cb) {
    {
        // Worker threads read it in enqueueMainThreadTask()
        std::lock_guard<std::mutex> lock(mainThreadTaskMutex);
        wakeCallback = cb;
    }
    // The AI service keeps its own copy, so nothing it holds points back here.
    if (aiChatWindow) aiChatWindow->setWakeCallback(std::move(cb));
}

void editor::App::setWindowFocused(bool focused) {
    // A panel dragged out into its own OS window counts too
    for (const ImGuiViewport* viewport : ImGui::GetPlatformIO().Viewports) {
        focused = focused || (viewport->Flags & ImGuiViewportFlags_IsFocused) != 0;
    }
    gitMonitor.setFocused(focused);
}

void editor::App::shutdownBackgroundWork() {
    loadingPump = nullptr;
    pendingProjectChange = nullptr;
    // Before glfwTerminate/SDL_Quit, or a late reply posts to a dead window system.
    if (aiChatWindow) aiChatWindow->shutdown();
    if (mcpServer) mcpServer->shutdown();
    gitMonitor.shutdown();
    // Not stop: the server is built to outlive the editor and wait in the
    // notification area, where its own menu offers to close it.
    detachResourceServer();
    std::lock_guard<std::mutex> lock(mainThreadTaskMutex);
    wakeCallback = nullptr;
}

void editor::App::engineViewDestroyed(){
    imageViewerWindow->closeAll();
    modelViewerWindow->closeAll();
    // Closing during project loading can leave uploads committed before any scene frame
    // consumes them. Drain that batch before shutdown waits to flush the queue.
    SystemRender::executeQueue();
    Engine::systemViewDestroyed();
}

void editor::App::engineShutdown(){
    Engine::systemShutdown();
}

void editor::App::addNewSceneToDock(uint32_t sceneId){
    if (isInitialized && !projectLoading){
        const std::string windowName = "###Scene" + std::to_string(sceneId);
        const SceneProject* sceneProject = project.getScene(sceneId);
        const bool isUnsavedScene = sceneProject && sceneProject->filepath.empty();

        dockTabWindow(windowName, isUnsavedScene);
    }
}

void editor::App::clearSceneWindowState(uint32_t sceneId) {
    if (sceneWindow) {
        sceneWindow->clearSceneState(sceneId);
    }

    if (propertiesWindow) {
        propertiesWindow->clearSceneState(sceneId);
    }

    if (animationWindow) {
        animationWindow->clearSceneState(sceneId);
    }

    if (lastActivatedScene == sceneId) {
        resetLastActivatedScene();
    }
}

void editor::App::prepareForProjectSwitch() {
    stopResourceServer();
    imageViewerWindow->closeAll();
    modelViewerWindow->closeAll();
    resourcesWindow->cancelThumbnailWork();
    // Thumbnail cancellation prevents new preview loads from being queued;
    // now quiesce any remaining scene/model jobs before pool cleanup.
    MeshSystem::cancelAllAsyncModelLoads();
    // Project deletes these scenes and framebuffers before another scene frame runs.
    Engine::removeAllScenes();
    Engine::setFramebuffer(nullptr);
    codeEditor->closeAll();
    outputWindow->clear();
    sceneWindow->resetProjectState();
    terrainEditWindow->resetProjectState();
    dockspaceNeedsRebuild = true;
}

void editor::App::requestScenePlayFocus(uint32_t sceneId) {
    if (!isMainThread()) {
        enqueueMainThreadTask([this, sceneId]() {
            requestScenePlayFocus(sceneId);
        });
        return;
    }

    if (sceneWindow) {
        sceneWindow->requestPlayFocus(sceneId);
    }
}

void editor::App::addNewCodeWindowToDock(fs::path path, bool force){
    if (isInitialized && !projectLoading){
        dockTabWindow("###" + path.string(), force);
    }
}

void editor::App::addImageViewerWindowToDock(fs::path path, bool force){
    if (isInitialized && !projectLoading){
        dockTabWindow(ImageViewerWindow::getWindowId(path), force);
    }
}

void editor::App::addModelViewerWindowToDock(fs::path path, bool force){
    if (isInitialized && !projectLoading){
        dockTabWindow(ModelViewerWindow::getWindowId(path), force);
    }
}

void editor::App::handleExternalDrop(const std::vector<std::string>& paths) {
    isDroppedExternalPaths = true;
    droppedExternalPaths = paths;
}

void editor::App::handleExternalDragEnter() {
    resourcesWindow->handleExternalDragEnter();
}

void editor::App::handleExternalDragLeave() {
    resourcesWindow->handleExternalDragLeave();
}

void editor::App::resetLastActivatedScene(){
    lastActivatedScene = NULL_PROJECT_SCENE;
    pendingResizeScene = NULL_PROJECT_SCENE;
}

bool editor::App::shouldSyncEngineApi() const {
    return true;
}

bool editor::App::recordsRecentProjects() const {
    return true;
}

void editor::App::updateResourcesPath(){
    std::filesystem::path currentProjectPath = project.getProjectPath().lexically_normal();
    bool projectPathChanged = lastResourcesProjectPath != currentProjectPath;

    if (isInitialized){
        if (projectPathChanged) {
            resourcesWindow->notifyProjectPathChange();
        } else {
            resourcesWindow->refreshCurrentDirectory();
        }
    }

    if (projectPathChanged) {
        updateResourceServer(currentProjectPath);
        updateProjectPluginSearch(currentProjectPath);
    }

    // Initial project loading happens before the Resources window is initialized,
    // but prepareForProjectSwitch() still suspends its worker.
    resourcesWindow->resumeThumbnailWork();

    lastResourcesProjectPath = currentProjectPath;
    resourcesWindow->cleanupThumbnails();
}

void editor::App::updateProjectPluginSearch(const std::filesystem::path& projectPath) {
    // Unreal keeps engine plugins and project plugins in one extension list; the engine already does
    // the same, so a project only has to offer its folder. Both spellings are scanned because a
    // project authored on Windows and checked out on a case-sensitive filesystem reads differently.
    if (projectPath.empty()) return;

    plugin::PluginManager& plugins = plugin::PluginManager::instance();
    const size_t before = plugins.searchDirectories().size();
    plugins.addSearchDirectory((projectPath / "Plugins").generic_string());
    plugins.addSearchDirectory((projectPath / "plugins").generic_string());
    if (plugins.searchDirectories().size() == before) return;

    // Not loaded here: the engine maps DLLs at start-up, and mapping one in the middle of a frame
    // would run a third party's initialize() against a live scene.
    Out::info("Project plugins found in %s - they load on the next editor start",
              (projectPath / "Plugins").string().c_str());
}

void editor::App::updateResourceServer(const std::filesystem::path& projectPath) {
    // One server per project: retire the previous spawn before starting another.
    // (stopResourceServer() bumps the generation first, which is what wakes a
    // worker still waiting on a booting server out of its poll loop.)
    stopResourceServer();
    if (projectPath.empty()) return;

    std::filesystem::path serverExe;
    if (!editor::cli::findResourceServerExecutable(serverExe)) return;

    resourceServerRecord = resourceServerRecordFile(projectPath);

    // A server started by an earlier editor session survives on purpose (it lives
    // in the notification area). Adopt it rather than indexing the same project
    // twice; anything the record points at that no longer answers is pruned.
    int recordedPort = 0;
    unsigned long long recordedPid = 0;
    std::filesystem::path recordedProject;
    std::string recordError;
    bool adopted = false;
    bool recordedServerBooting = false;
    if (editor::cli::readServerRecord(resourceServerRecord, recordedPort, recordedPid,
                                      recordedProject, recordError) &&
        recordedProject == projectPath && editor::cli::processIsRunning(recordedPid)) {
        std::string statusJson;
        if (editor::cli::requestStatus(recordedPort, statusJson, recordError)) {
            adopted = true;
            resourceServerPort = recordedPort;
            resourceServerPid = recordedPid;
            if (g_resourceMonitorWindow) {
                g_resourceMonitorWindow->addChildProcess(recordedPid,
                    "quantum-resource-server:" + std::to_string(recordedPort));
            }
            Out::info("Resource server: using the instance already listening on 127.0.0.1:%d",
                      recordedPort);
        } else {
            // Alive but not answering: the server indexes the project before it
            // starts listening, so this is what restarting right after an editor
            // crash during its boot looks like. Wait for it below instead of
            // pruning the record and spawning a duplicate.
            recordedServerBooting = true;
        }
    }
    if (adopted) return;
    if (!recordedServerBooting) {
        editor::cli::removeServerRecord(resourceServerRecord);
    }

    const unsigned long long generation = ++resourceServerGeneration;
    // Captured by value: the worker thread writes the record the moment the
    // child exists, before the handshake, so an editor crash during boot still
    // leaves an adoptable record instead of an invisible orphan.
    const std::filesystem::path recordFile = resourceServerRecord;
    const int bootingPort = recordedServerBooting ? recordedPort : 0;
    const unsigned long long bootingPid = recordedServerBooting ? recordedPid : 0;
    // spawnResourceServer waits on the child's stdout, so keep it off the UI thread.
    resourceServerThread = std::thread([this, projectPath, serverExe, generation, recordFile,
                                        bootingPort, bootingPid]() {
        int port = 0;
        unsigned long long pid = 0;
        std::string error;
        bool ok = false;
        bool adoptedBooting = false;
        if (bootingPort > 0) {
            // As long as a cold spawn can take to index a project; a server that
            // dies or never answers falls through to a fresh spawn.
            constexpr int kBootWaitMs = 30000;
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(kBootWaitMs);
            while (std::chrono::steady_clock::now() < deadline) {
                if (resourceServerGeneration.load() != generation) return;
                std::string statusJson;
                std::string statusError;
                if (editor::cli::requestStatus(bootingPort, statusJson, statusError)) {
                    port = bootingPort;
                    pid = bootingPid;
                    adoptedBooting = true;
                    ok = true;
                    break;
                }
                if (!editor::cli::processIsRunning(bootingPid)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
            }
            if (!ok) editor::cli::removeServerRecord(recordFile);
        }
        if (!ok) {
            ok = editor::cli::spawnResourceServer(projectPath, serverExe, port, pid, error,
                                                  editor::cli::currentProcessId(), recordFile);
        }
        enqueueMainThreadTask([this, generation, ok, port, pid, error, projectPath, adoptedBooting]() {
            if (generation != resourceServerGeneration) {
                std::string ignored;
                if (ok && !adoptedBooting) editor::cli::stopResourceServer(pid, ignored);
                return;
            }
            if (!ok) {
                Out::warning("Resource server: %s", error.c_str());
                return;
            }
            resourceServerPort = port;
            resourceServerPid = pid;
            if (adoptedBooting) {
                if (g_resourceMonitorWindow) {
                    g_resourceMonitorWindow->addChildProcess(pid,
                        "quantum-resource-server:" + std::to_string(port));
                }
                Out::info("Resource server: using the instance already listening on 127.0.0.1:%d",
                          port);
                return;
            }
            std::string recordError;
            if (!editor::cli::writeServerRecord(resourceServerRecord, port, pid, projectPath,
                                                recordError) && !recordError.empty()) {
                Out::warning("Resource server: %s", recordError.c_str());
            }
            if (g_resourceMonitorWindow) {
                g_resourceMonitorWindow->addChildProcess(pid,
                    "quantum-resource-server:" + std::to_string(port));
            }
            Out::info("Resource server listening on 127.0.0.1:%d", port);
        });
    });
}

std::filesystem::path editor::App::resourceServerRecordFile(const std::filesystem::path& projectPath) {
    // Keyed by project so servers for different projects never overwrite each
    // other's record; hashed because a full path is not a legal filename.
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : projectPath.string()) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.record", static_cast<unsigned long long>(hash));
    return App::getUserCacheBaseDir() / "quantum" / "resource-server" / name;
}

void editor::App::stopResourceServer() {
    // Bump before the join: the worker may be polling a booting server and only
    // exits its loop once it sees the generation change.
    ++resourceServerGeneration;   // drop any in-flight spawn or boot wait
    if (resourceServerThread.joinable()) resourceServerThread.join();
    if (!resourceServerRecord.empty()) {
        editor::cli::removeServerRecord(resourceServerRecord);
        resourceServerRecord.clear();
    }
    if (resourceServerPid == 0) return;
    std::string error;
    if (!editor::cli::stopResourceServer(resourceServerPid, error) && !error.empty()) {
        Out::warning("Resource server: %s", error.c_str());
    }
    if (g_resourceMonitorWindow) g_resourceMonitorWindow->removeChildProcess(resourceServerPid);
    resourceServerPid = 0;
    resourceServerPort = 0;
}

void editor::App::detachResourceServer() {
    ++resourceServerGeneration;
    if (resourceServerThread.joinable()) resourceServerThread.join();
    if (resourceServerPid != 0) {
        if (g_resourceMonitorWindow) g_resourceMonitorWindow->removeChildProcess(resourceServerPid);
        Out::info("Resource server: still running on 127.0.0.1:%d (close it from its tray icon)",
                  resourceServerPort);
    }
    // The record file is left behind on purpose: it is how the next editor
    // session finds this server instead of starting a second one.
    resourceServerRecord.clear();
    resourceServerPid = 0;
    resourceServerPort = 0;
}

void editor::App::requestDockspaceRebuild() {
    dockspaceNeedsRebuild = true;
}

void editor::App::updateWindowTitle(const std::string& projectName) {
    Backend::updateWindowTitle(projectName);
}

void editor::App::stopTransientPreviews() {
    if (terrainEditWindow) {
        terrainEditWindow->endStroke();
        terrainEditWindow->updateFoliagePreview();
    }
    if (propertiesWindow) {
        propertiesWindow->stopTransientPreviews();
    }
}

void editor::App::flushSceneMaterialWrites(uint32_t sceneId) {
    if (propertiesWindow) {
        propertiesWindow->flushDirtyMaterialsForScene(sceneId);
    }
}

void editor::App::saveAllCodeEditors() {
    if (codeEditor) {
        codeEditor->saveAll();
    }
}

void editor::App::registerAlert(std::string title, std::string message) {
    registerAlert(title, message, "");
}

void editor::App::registerAlert(std::string title, std::string message, std::string note) {
    alert.needShow = true;
    alert.title = title;
    alert.message = message;
    alert.note = note;
    alert.type = AlertType::Info;
    alert.onYes = nullptr;
    alert.onNo = nullptr;
}

void editor::App::registerOutsideAssetsAlert(const std::string& path) {
    registerAlert(
        "File outside the assets directory",
        "Only files inside \"" + project.getAssetsDir().generic_string() + "\" can be used.\n"
        "Move or copy it there and drop it again.",
        project.normalizeToProjectRelative(path).generic_string());
}

void editor::App::registerConfirmAlert(std::string title, std::string message, std::function<void()> onYes, std::function<void()> onNo) {
    alert.needShow = true;
    alert.title = title;
    alert.message = message;
    alert.note.clear();
    alert.type = AlertType::Confirm;
    alert.onYes = onYes;
    alert.onNo = onNo;
}

void editor::App::registerThreeButtonAlert(std::string title, std::string message, std::function<void()> onYes, std::function<void()> onNo, std::function<void()> onCancel) {
    alert.needShow = true;
    alert.title = title;
    alert.message = message;
    alert.note.clear();
    alert.type = AlertType::ThreeButton;
    alert.onYes = onYes;
    alert.onNo = onNo;
    alert.onCancel = onCancel;
}

void editor::App::registerSaveSceneDialog(uint32_t sceneId, std::function<void(bool)> callback) {
    // Add scene to the save dialog queue with callback
    SaveDialogQueueItem item = {SaveDialogType::Scene, sceneId, callback};
    saveDialogQueue.push(item);

    // If this is the only item in the queue, process it immediately
    if (saveDialogQueue.size() == 1 && !saveDialogInProgress && !sceneSaveDialog.isOpen() && !projectSaveDialog.isOpen()) {
        processNextSaveDialog();
    }
    // If queue has more items or another dialog is open, they'll be processed later
}

void editor::App::registerProjectSaveDialog(std::function<void(bool)> callback) {
    // Add project save to the dialog queue with callback
    SaveDialogQueueItem item = {SaveDialogType::Project, 0, callback};  // sceneId is unused for Project saves
    saveDialogQueue.push(item);

    // If this is the only item in the queue, process it immediately
    if (saveDialogQueue.size() == 1 && !saveDialogInProgress && !sceneSaveDialog.isOpen() && !projectSaveDialog.isOpen()) {
        processNextSaveDialog();
    }
    // If queue has more items or another dialog is open, they'll be processed later
}

std::filesystem::path editor::App::getUserCacheBaseDir() {
    // Cache the result to avoid repeated syscalls/env lookups
    static std::filesystem::path cached = []() -> std::filesystem::path {
    #if defined(_WIN32)
        // Ensure COM is initialized for SHGetKnownFolderPath
        // Using COINIT_APARTMENTTHREADED is safe for most GUI apps
        HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        // hrInit == S_OK means we initialized, RPC_E_CHANGED_MODE means already initialized differently (OK)
        // We don't call CoUninitialize here since we want COM available for the app lifetime

        PWSTR widePath = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &widePath)) && widePath) {
            std::filesystem::path p(widePath);
            CoTaskMemFree(widePath);
            return p; // e.g. C:\Users\<you>\AppData\Local
        }
        // Fallback to environment variable
        if (const char* localAppData = std::getenv("LOCALAPPDATA"); localAppData && *localAppData) {
            return std::filesystem::path(localAppData);
        }
        return std::filesystem::temp_directory_path();

    #elif defined(__APPLE__)
        // Conventional macOS cache location: ~/Library/Caches
        const char* home = std::getenv("HOME");
        if (home && *home) {
            return std::filesystem::path(home) / "Library" / "Caches";
        }
        return std::filesystem::temp_directory_path();

    #else
        // Linux / other Unix: XDG Base Dir spec
        // Prefer $XDG_CACHE_HOME, fallback to ~/.cache
        const char* xdg = std::getenv("XDG_CACHE_HOME");
        if (xdg && *xdg) {
            return std::filesystem::path(xdg);
        }
        const char* home = std::getenv("HOME");
        if (home && *home) {
            return std::filesystem::path(home) / ".cache";
        }
        return std::filesystem::temp_directory_path();
    #endif
    }();

    return cached;
}

std::filesystem::path editor::App::getUserShaderCacheDir(){
    // Every cached .sdat also carries the SHA1 of the source graph it was compiled from
    // (ShaderBuilder::isCacheStale), so editing engine/shaderlib invalidates exactly the
    // variants built from that text and nothing else. Bump this directory only for what a
    // source hash cannot see: a change to the .sdat format, or to a #define the C++ side
    // feeds the compiler (setupShaderArgs constants, MAX_LIGHTS, SSR_MAX_STEPS).
    // v4: mirror (USE_MIRROR) variants changed the u_fs_mirror block guard.
    // v5: SSAO (mesh.frag USE_SSAO + viewportInfo, new ssao/ssao_blur shaders).
    // v6: SSR (G-buffer geometry pass + ssr/ssr_blur/composite fullscreen shaders;
    //     energy-conserving SSR-over-IBL, glossy roughness blur, debug modes).
    // v7: second UV set (HAS_UV_SET2) — per-texture UV selection via u_fs_texCoordSets.
    // v8: directional/spot shadows share u_shadowAtlas (replaces u_shadowMap1..6),
    //     3x3 atlas, SSAO on terrain (USE_SSAO + depth HAS_TERRAIN).
    // v9: dual shadow atlases — u_shadowAtlas (directional/spot, projective) and
    //     u_shadowPointAtlas (point cube faces); removes the u_shadowCubeMap* samplers
    //     and keeps each atlas within the GPU max texture size.
    // v10: 2D lighting — mesh USE_LIGHT2D/USE_SHADOWS_2D variants (u_fs_lighting2d,
    //      u_normalTexture on unlit, v_position guard) and the shadow2d 1D polar pass.
    // v11: uniform-driven shadow PCF (ShadowQuality) — USE_SHADOWS_PCF variant removed,
    //      shadows.glsl/lighting2d.glsl loop by radius from cameraDir.w / atlasInfo.w.
    // v12: fixed game resolution — new blit fullscreen shader (blit.frag).
    // v13: box-projected local reflection probes in the mesh IBL path.
    // v14: G-buffer (SSR) terrain fix — gbuffer.vert no longer declares a_texcoord1
    //      for terrain (generates the base-tile UV in-shader), so the terrain G-buffer
    //      pipeline's vertex layout is continuous and passes sokol validation.
    // v15: skinning capacity increased to 128 bones; every skinned shader uniform block changed.
    // v16: projected spotlight masks add spotUp_maskAspect[MAX_LIGHTS] to u_fs_lighting
    //      and add the shared u_spotMaskAtlas sampler to punctual mesh variants,
    //      using the engine's native texture Y orientation.
    // v17: projective shadows use a depth-only atlas with a comparison sampler.
    // v18: vec3 vertex colors on meshes — v_color is promoted to vec4 under instancing.
    // v19: Vulkan shadow atlas lookups and screen-space depth/texture orientation fixes,
    //      later widened from IS_VULKAN to every non-GL backend (Metal, D3D11) across
    //      the depth, G-buffer and fullscreen passes: same shaders, so still v19.
    // v20: dynamically sized skinning uses storage buffers, or an unfilterable bone
    //      texture with a nonfiltering sampler on GL/GLES.
    // v21: backend-specific storage-buffer bindings are preserved in reflection data.
    // v22: composite.frag samples the scene color and reflection in destination space
    //      (params.w), so SSR can composite to the swapchain in exported builds.
    // v23: explicit SSR LOD sampling prevents D3D11 loop-unrolling failures.
    // v24: instanced distance fade (USE_INSTANCE_FADE) scales each instance toward its own
    //      base in the mesh and depth vertex shaders, from the new u_vs_fade block. Terrain
    //      detail layers blend by height, project triplanar on steep ground and break their
    //      tiling with distance, from three new terrain varyings.
    // v25: skinned normals and tangents are transformed as directions (mat3), no longer
    //      as points, so posed bone translations stop flattening the lighting.
    // v26: shadow PCF loop bounds are clamped on every backend, not just HLSL, so a
    //      bad uniform can no longer produce an unbounded loop (a GPU hang).
    // v27: per-layer PBR terrain (HAS_TERRAIN_PBR) — layer maps are slices of the detail
    //      array driven by u_fs_terrainLayers, in the color pass and the G-buffer alike.
    // v28: paired bilinear shadow PCF taps; depth.vert takes a prebuilt mvpMatrix and
    //      gl_Position is invariant, so a depth prepass matches the color pass; instanced
    //      normals use the cofactor matrix; backfaces flip the shading normal; leaf
    //      transmission adds foliageTransmission to u_fs_pbrParams.
    // v29: sprite and tilemap UVs (HAS_TEXTURERECT) use centroid interpolation, so MSAA
    //      edge pixels no longer sample outside their atlas rect.
    // v30: Water component — new water and underwater shaders.
    // v32: Water exclusions, appended to u_fs_waterParams.
    // v33: pbr.glsl reads u_fs_texCoordSets.set1 in every UV-set variant.
    // v34: shadow projection in the fragment shader, directional-only variant.
    // v35: the directional-only variant declares no point or spot bindings.
    // v36: water simulation variant (USE_WATER_SIM) fetches the depth field in the vertex stage.
    // v37: exposure.frag is a 1x1 eye-adaptation meter with history, and tonemap.frag gained the
    //      exposure sampler plus the grade/grade2 colour-grading terms.
    // v38: bloom_composite.frag multiplies the glow by a procedural lens-dirt mask (params.z).
    // v39: fxaa.frag is wired as the last pass of the frame; it samples with the combined
    //      sampler pair, and params.z/w are the blend and the source-Y flip.
    // v40: parallax occlusion mapping (HAS_HEIGHT) — every map UV in pbr.glsl now passes
    //      through heightOffset(), which is the identity for a variant without a height
    //      map but a different function call, so the cached ones cannot be reused.
    // v41: tone mapping carries a real texture2DArray color-grading LUT uniform block.
    // v42: fullscreen velocity reconstruction plus TAA and motion-blur pass uniforms.
    return App::getUserCacheBaseDir() / "quantum" / "shaders" / "v42";
}

void editor::App::pushTabNotificationStyle(){
    ImGui::PushStyleColor(ImGuiCol_Tab,        ImVec4(0.22f, 0.30f, 0.40f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_TabDimmed,   ImVec4(0.22f, 0.30f, 0.40f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_TabHovered,  ImVec4(0.26f, 0.35f, 0.46f, 1.00f));
}

void editor::App::popTabNotificationStyle(){
    ImGui::PopStyleColor(3);
}

editor::Project* editor::App::getProject(){
    return &project;
}

const editor::Project* editor::App::getProject() const{
    return &project;
}

editor::Properties* editor::App::getPropertiesWindow() const{
    return propertiesWindow;
}

editor::CodeEditor* editor::App::getCodeEditor() const{
    return codeEditor;
}

editor::ImageViewerWindow* editor::App::getImageViewerWindow() const{
    return imageViewerWindow;
}

editor::ModelViewerWindow* editor::App::getModelViewerWindow() const{
    return modelViewerWindow;
}

editor::ModelExportWindow* editor::App::getModelExportWindow() const{
    return g_modelExportWindow;
}

editor::MaterialBrowserWindow* editor::App::getMaterialBrowserWindow() const{
    return materialBrowserWindow;
}

editor::ResourcesWindow* editor::App::getResourcesWindow() const{
    return resourcesWindow;
}

bool editor::App::popSaveDialogQueueItem() {
    if (saveDialogQueue.empty()) {
        Out::warning("Attempted to pop an empty save dialog queue.");
        return false;
    }

    saveDialogQueue.pop();
    return true;
}

editor::AnimationWindow* editor::App::getAnimationWindow() const{
    return animationWindow;
}

editor::TerrainEditWindow* editor::App::getTerrainEditWindow() const{
    return terrainEditWindow;
}

editor::AnimatorWindow* editor::App::getAnimatorWindow() const{
    return animatorWindow;
}

editor::Structure* editor::App::getStructureWindow() const{
    return structureWindow;
}

editor::AiChatWindow* editor::App::getAiChatWindow() const{
    return aiChatWindow;
}

editor::ai::McpServer* editor::App::getMcpServer() const{
    return mcpServer;
}

void editor::App::openEditorSettings(EditorSettingsWindow::Tab tab, bool expandAiChat){
    editorSettingsWindow.open(&project, tab, expandAiChat);
}

void editor::App::processNextSaveDialog() {
    // Check if there's anything to process and no dialogs are currently open
    if (saveDialogInProgress || saveDialogQueue.empty() || sceneSaveDialog.isOpen() || projectSaveDialog.isOpen()) {
        return;
    }

    // Get the next item from the queue
    SaveDialogQueueItem item = saveDialogQueue.front();
    // Store the callback to use it later
    std::function<void(bool)> completionCallback = item.callback;

    if (item.type == SaveDialogType::Scene) {
        // Process scene save dialog
        uint32_t sceneId = item.sceneId;
        SceneProject* sceneProject = project.getScene(sceneId);
        if (!sceneProject) {
            // Invalid scene, remove from queue and try next item
            popSaveDialogQueueItem();
            if (completionCallback) {
                completionCallback(false);
            }
            processNextSaveDialog();
            return;
        }

        // Set default filename
        std::string defaultName = sceneProject->name + ".scene";
        fs::path initialDirectory = project.getProjectPath();
        if (resourcesWindow) {
            fs::path resourcesPath = resourcesWindow->getCurrentPath();
            if (!resourcesPath.empty()) {
                initialDirectory = resourcesPath;
            }
        }

        saveDialogInProgress = true;

        // Open dialog for the current scene
        sceneSaveDialog.open(
            project.getProjectPath(),
            initialDirectory,
            defaultName,
            // Save callback
            [this, sceneId, completionCallback](const fs::path& fullPath) {
                bool saveStarted = false;
                SceneProject* sceneProject = project.getScene(sceneId);
                if (sceneProject) {
                    // Create directory if it doesn't exist
                    std::filesystem::create_directories(fullPath.parent_path());

                    // Save the scene
                    std::error_code ec;
                    fs::path relPath = fs::relative(fullPath, project.getProjectPath(), ec);
                    if (ec || relPath.empty()) {
                        Out::error("Scene filepath must be relative to project path: %s", fullPath.string().c_str());
                        Backend::getApp().registerAlert("Error", "Scene file must be inside the project folder.");
                    } else {
                        sceneProject->filepath = relPath;
                        saveStarted = true;
                        project.saveSceneToPathAsync(sceneId, fullPath, [this, completionCallback](bool success) {
                            saveDialogInProgress = false;
                            popSaveDialogQueueItem();

                            if (completionCallback) {
                                completionCallback(success);
                            }

                            processNextSaveDialog();
                        });
                    }
                }

                if (!saveStarted) {
                    saveDialogInProgress = false;
                    popSaveDialogQueueItem();
                    if (completionCallback) {
                        completionCallback(false);
                    }
                    processNextSaveDialog();
                }
            },
            // Cancel callback
            [this, completionCallback]() {
                // Remove the current item from the queue without saving
                saveDialogInProgress = false;
                popSaveDialogQueueItem();

                if (completionCallback) {
                    completionCallback(false);
                }

                // Process the next item if available
                processNextSaveDialog();
            }
        );
    }
    else if (item.type == SaveDialogType::Project) {
        // Process project save dialog
        std::string defaultName = project.getName();
        if (defaultName.empty()) {
            defaultName = "MyProject";
        }

        saveDialogInProgress = true;

        projectSaveDialog.open(
            defaultName,
            // Save callback
            [this, completionCallback](const std::string& projectName, const fs::path& projectPath) {
                // Set the project name if provided
                if (!projectName.empty()) {
                    project.setName(projectName);
                }

                // Save the project to the selected path
                const bool saved = project.saveProjectToPath(projectPath);
                if (!saved) registerAlert("Save Failed", "The project could not be saved. Check its directory and try again.");

                // Remove this item from the queue
                saveDialogInProgress = false;
                popSaveDialogQueueItem();

                // Execute the completion callback if provided
                if (completionCallback) {
                    completionCallback(saved);
                }

                // Process the next item if available
                processNextSaveDialog();
            },
            // Cancel callback
            [this, completionCallback]() {
                // Remove the current item from the queue without saving
                saveDialogInProgress = false;
                popSaveDialogQueueItem();

                if (completionCallback) {
                    completionCallback(false);
                }

                // Process the next item if available
                processNextSaveDialog();
            }
        );
    }
}

void editor::App::processMainThreadTasks() {
    if (projectLoading) return;

    std::queue<std::function<void()>> tasks;
    {
        std::lock_guard<std::mutex> lock(mainThreadTaskMutex);
        std::swap(tasks, mainThreadTasks);
    }

    while (!tasks.empty()) {
        auto task = std::move(tasks.front());
        tasks.pop();
        if (task) task();
    }
}

bool editor::App::isMainThread() const {
    return std::this_thread::get_id() == mainThreadId;
}

void editor::App::initializeSettings() {
    AppSettings::initialize();

    const editor::LaunchOptions& launch = editor::LaunchOptions::get();
    const bool previousSessionUnclean = AppSettings::wasPreviousSessionUnclean();
    safeMode = launch.safeMode || previousSessionUnclean;

    // Next to settings.yaml, where a user can find it to report a crash.
    const std::filesystem::path logFile = AppSettings::getConfigDirectory() / "editor.log";
    Out::setLogFile(logFile);
    CrashHandler::install(logFile);
    AppSettings::beginSession();
    if (safeMode && previousSessionUnclean) {
        Out::warning("Previous editor session ended unexpectedly; starting in recovery mode (last project and layout are preserved)");
    }

    applyPanelVisibilitySettings();
}

void editor::App::applyPanelVisibilitySettings() {
    const PanelVisibilitySettings visibility = AppSettings::getPanelVisibility();
    structureWindow->setOpen(visibility.structure);
    propertiesWindow->setOpen(visibility.properties);
    resourcesWindow->setOpen(visibility.resources);
    outputWindow->setOpen(visibility.output);
    animationWindow->setOpen(visibility.animation);
    terrainEditWindow->setOpen(visibility.terrain);
    aiChatWindow->setOpen(visibility.aiChat);
    g_notificationsWindow->setOpen(visibility.notifications);
    g_resourceMonitorWindow->setOpen(visibility.resourceMonitor);
    g_graphEditorWindow->setOpen(visibility.graphEditor);
    g_terminalWindow->setOpen(visibility.terminal);
    materialBrowserWindow->setOpen(visibility.materialBrowser);
    g_modelExportWindow->setOpen(visibility.modelExport);
    g_pluginBrowserWindow->setOpen(visibility.pluginBrowser);
    g_worldCreatorWindow->setOpen(visibility.worldCreator);
    // Plugin windows are not a field of that struct: their names belong to whichever plugins loaded,
    // so the list of ids is what comes back out of the settings file.
    g_pluginWindowHost->restoreWantedWindows(AppSettings::getOpenPluginWindows());
}

void editor::App::persistPanelVisibilitySettings() {
    PanelVisibilitySettings visibility;
    visibility.structure = structureWindow->isOpen();
    visibility.properties = propertiesWindow->isOpen();
    visibility.resources = resourcesWindow->isOpen();
    visibility.output = outputWindow->isOpen();
    visibility.animation = animationWindow->isOpen();
    visibility.terrain = terrainEditWindow->isOpen();
    visibility.aiChat = aiChatWindow->isOpen();
    visibility.notifications = g_notificationsWindow->isOpen();
    visibility.resourceMonitor = g_resourceMonitorWindow->isOpen();
    visibility.graphEditor = g_graphEditorWindow->isOpen();
    visibility.terminal = g_terminalWindow->isOpen();
    visibility.materialBrowser = materialBrowserWindow->isOpen();
    visibility.modelExport = g_modelExportWindow->isOpen();
    visibility.pluginBrowser = g_pluginBrowserWindow->isOpen();
    visibility.worldCreator = g_worldCreatorWindow->isOpen();

    if (visibility != AppSettings::getPanelVisibility()) {
        AppSettings::setPanelVisibility(visibility);
    }
}

int editor::App::getInitialWindowWidth(float uiScale) const {
    return scaleSavedWindowSize(AppSettings::getWindowWidth(), uiScale);
}

int editor::App::getInitialWindowHeight(float uiScale) const {
    return scaleSavedWindowSize(AppSettings::getWindowHeight(), uiScale);
}

bool editor::App::getInitialWindowMaximized() const {
    return AppSettings::getIsMaximized();
}

void editor::App::saveWindowSettings(int width, int height, bool maximized, float uiScale) {
    AppSettings::setWindowWidth(width);
    AppSettings::setWindowHeight(height);
    AppSettings::setIsMaximized(maximized);
    AppSettings::setWindowUiScale(sanitizeUiScale(uiScale));
    // The ini is written at the scale the layout has held all session, not the
    // monitor scale above, which is read fresh and may have changed since.
    if (layoutScaleApplied) {
        AppSettings::setLayoutUiScale(sanitizeLayoutScale(layoutUiScale));
    }
    AppSettings::saveSettings();
}

void editor::App::exit() {
    if (projectLoading) {
        Backend::closeWindow();
        return;
    }

    // Check if any modal popup is currently open (including ComponentAddDialog, ScriptCreateDialog, etc.)
    ImGuiWindow* modal = ImGui::GetTopMostAndVisiblePopupModal();
    if (modal != nullptr) {
        // A modal is open - close it first
        ImGui::CloseCurrentPopup();
        return;  // Don't proceed with exit yet, user needs to click close again
    }

    // Also check if the scene save dialog is specifically open
    if (sceneSaveDialog.isOpen()) {
        sceneSaveDialog.close();
        return;
    }

    if (project.isAnySceneSaving()) {
        registerAlert("Saving Scene", "Wait for the current scene save to finish before exiting.");
        return;
    }

    if (project.hasScenesUnsavedChanges() || codeEditor->hasUnsavedChanges() || project.isTempUnsavedProject()) {
        registerThreeButtonAlert(
            "Unsaved Changes",
            "There are unsaved changes. Do you want to save them before exiting?",
            [this]() {
                // No agent edit may land after the save; a failed save brings the server back
                mcpServer->shutdown();
                saveAllFunc([this](bool success) {
                    if (success) {
                        closeWindow();
                    } else {
                        mcpServer->applySettings(AppSettings::getMcpSettings());
                    }
                });
            },
            [this]() {
                // No callback - just exit without saving
                closeWindow();
            },
            []() {
                // Cancel callback - do nothing, just close the dialog
            }
        );
    } else {
        // No unsaved changes, proceed with exit
        closeWindow();
    }
}

void editor::App::closeWindow(){
    // Agents must not change the project while it closes
    mcpServer->shutdown();

    // A moved camera, a different selected scene and a changed terrain brush live
    // only in the model until this runs, and nothing else asks for it. Also flushes
    // a tab reorder still sitting in captureTabOrder()'s debounce.
    if (!benchmarkEnabled) {
        project.saveWorkspaceFile();
        tabsOrderDirty = false;
    }

    // Stop all playing scenes before shutdown to properly cleanup script instances
    for (auto& sceneProject : project.getScenes()) {
        if (sceneProject.playState == ScenePlayState::PLAYING || 
            sceneProject.playState == ScenePlayState::PAUSED ||
            sceneProject.playState == ScenePlayState::LOADING) {
            project.stop(sceneProject.id);
        }
    }

    project.waitForPlaySessionToFinish();

    project.clearTrash();

    editor::ShaderBuilder::requestShutdown();
    AppSettings::endSession();
    Backend::closeWindow();
}
