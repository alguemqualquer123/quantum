// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace quantum::editor {

// Every command the main menu, the command palette and the keyboard can fire. The
// value is the wire id of PlatformMenuCommand, so it is stored in menu items and in
// the palette entries and must never be renumbered - append new commands instead.
enum class AppMenuCommand : uint32_t {
    NewProject = 1,
    NewScene3D,
    NewScene2D,
    NewSceneUI,
    OpenProject,
    OpenRecentProject,
    ClearRecentProjects,
    SaveProject,
    SaveProjectAs,
    Save,
    SaveAll,
    ExportProject,
    EditorSettings,
    Exit,
    Undo,
    Redo,
    DuplicateSelection,
    DeleteSelection,
    ToggleCodeComment,
    ToggleCodeBlockComment,
    FormatCodeDocument,
    ToggleStructure,
    ToggleProperties,
    ToggleResources,
    ToggleOutput,
    ToggleAnimation,
    ToggleTerrain,
    ToggleAnimator,
    ToggleAiChat,
    ToggleDetachableWindows,
    ToggleEditorVSync,
    ResetLayout,
    ProjectSettings,
    ProjectScenes,
    ProjectBundles,
    ClearTrash,
    ClearShaderCache,
    RunScene,
    PauseScene,
    ResumeScene,
    StopScene,
    DetachCamera,
    RemoveScene,
    About,
    Documentation,
    KeyboardShortcuts,
    ReportIssue,
    ToggleNotifications,
    ToggleResourceMonitor,
    ToggleGraphEditor,
    ToggleTerminal,
    ToggleMaterialBrowser,
    ToggleModelExport,
    CommandPalette,
    ThemeGray,
    ThemeLight,
    ThemeDark,
    AccentBlue,
    AccentTeal,
    AccentAmber,
    AccentViolet,
    NewProjectFromTemplate,
    ToggleWelcome,
    ToggleProjectFileType,
    // Deep links into the Editor Settings dialog. Appended: the ordinal is the wire id the
    // platform menu and the command palette both carry.
    SettingsShortcutsTab,
    SettingsGraphicsTab,
    SettingsEngineApiTab,
    // Appended for the same reason as the three above: the ordinal is the wire id.
    TogglePlugins,
    ToggleWorldCreator,
    TogglePhysicsDebugDraw,
    // Appended for ABI 2 plugin windows. Both carry a PlatformMenuCommand payload - a command name
    // and a window id - because the enum has no room for either, and the palette can only fire the
    // payloadless commands.
    RunPluginCommand,
    TogglePluginWindow,
    NetworkPlay,
    ToggleScriptDebugger,
    ReloadCppScripts,
    ValidateProject
};

} // namespace quantum::editor
