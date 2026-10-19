// AppContext.h - everything the panels and demo pages share.
//
// The context is created once by the Application and passed by reference, which
// keeps the panels/demos free of global state and makes them testable through
// the headless self-test.
#pragma once

#include <string>
#include <vector>

#include "core/Dialogs.h"
#include "core/EventLog.h"
#include "core/Notifications.h"
#include "core/Registry.h"
#include "core/Settings.h"
#include "core/Types.h"
#include "core/UndoStack.h"
#include "canvas/CanvasScene.h"
#include "host/HostHooks.h"
#include "panels/AssetLibrary.h"
#include "themes/Theme.h"
#include "ui/Texture.h"

namespace uilab {

// ---------------------------------------------------------------------------
// Frame statistics (per-frame samples kept for the debug graphs)
// ---------------------------------------------------------------------------
struct FrameStats {
    static constexpr size_t kMaxSamples = 240;

    std::vector<float> frameMs;      // milliseconds
    std::vector<float> fpsHistory;
    std::vector<float> vertices;
    std::vector<float> drawCalls;

    float currentFrameMs = 16.6f;
    float averageFrameMs = 16.6f;
    float minFrameMs = 0.0f;
    float maxFrameMs = 0.0f;
    float currentFps = 60.0f;
    float averageFps = 60.0f;

    double uptime = 0.0;
    int frameCount = 0;
    int uiDrawCalls = 0;
    int uiVertices = 0;
    int uiWindows = 0;
    int uiTables = 0;
    size_t logEntries = 0;
    size_t toasts = 0;

    void PushFrame(float ms);
    void CaptureImGuiStats();
    void Reset();
    const char* FrameBudgetLabel() const; // helper for the status/debug panels
};

// ---------------------------------------------------------------------------
// State shared between the demo pages (animation clock + HUD/game values)
// ---------------------------------------------------------------------------
struct DemoShared {
    float time = 0.0f;             // animation clock (affected by play/pause)
    float wallTime = 0.0f;         // never pauses
    bool playing = true;
    float speed = 1.0f;

    // HUD / game values used by the UMG and uGUI demos
    float health = 0.72f;
    float energy = 0.45f;
    float score = 1250.0f;
    float ammo = 24.0f;
    int wave = 3;
    bool hudVisible = true;
    float hudScale = 1.0f;
    float damagePerHit = 0.08f;
    std::vector<std::string> inventory = { "Medkit", "Ammo Belt", "Key Card", "Grenade" };
    std::vector<int> inventoryCounts = { 3, 120, 1, 2 };
    int inventorySelected = 0;

    // code editor style text used by demos with validation
    std::string blueprintName = "BP_PlayerController";
    std::string searchQuery;
    std::string multilineBuffer;
    int tileCount = 9;

    // interaction counters (debug panel / stats)
    int buttonClicks = 0;
    int sliderChanges = 0;
    int checkboxToggles = 0;
    int eventsThisSecond = 0;
    float eventsTimer = 0.0f;
};

struct SelectionRef {
    std::string widgetId;
    int sceneNodeId = -1;
};

class AppContext {
public:
    // ---- subsystems ----------------------------------------------------
    WidgetRegistry registry;
    EventLog log;
    NotificationCenter toasts;
    DialogManager dialogs;
    UndoStack undo;
    ThemeManager theme;
    AppSettings settings;
    FrameStats stats;
    DemoShared demo;
    CanvasScene scene;
    AssetLibrary assets;

    // Procedural textures. Owned by the Application and only valid while a GL
    // context exists (null in the headless self-test).
    TextureLibrary* textures = nullptr;

    // Content overrides for an embedding application. Empty here means every panel
    // draws the laboratory's own data, which is what the standalone app does.
    HostHooks host;

    // ---- selection -----------------------------------------------------
    SelectionRef selection;
    Category activeCategory = Category::Basic;
    SelectionRef clipboardSelection;

    // ---- requests raised by panels, consumed by the application --------
    bool requestExit = false;
    // A host that docks its own windows keeps its own node tree and its own ini
    // arrangement: layout::BeginFrame() then creates the dockspace but never installs
    // the laboratory's default tree.
    bool installDefaultDockTree = true;
    bool requestResetLayout = false;
    bool requestSaveLayout = false;
    bool requestLoadLayout = false;
    bool requestCloseAllFloatingWindows = false;
    int frameIndex = 0;

    // ---- floating windows (spawned by the demos) ------------------------
    bool showSlateWindow = false;
    bool showImmediateWindow = false;
    bool showDebugOverlayWindow = false;
    bool showUiToolkitInspectorWindow = false;
    bool showSplitterDemoWindow = false;

    std::string appName = "Universal UI Components Lab";
    std::string appVersion = "1.0.0";
    std::string renderer = "OpenGL";
    std::string rendererVersion;
    std::string platform;

    WidgetInstance* SelectedWidget();
    const WidgetInstance* SelectedWidget() const;
    void SelectWidget(const std::string& id);
    void SelectCategory(Category c);

    // Logging helpers ----------------------------------------------------
    void LogConsole(LogLevel level, const std::string& source, const std::string& message);
    void LogEvent(const std::string& source, const std::string& message);
    void LogDebug(const std::string& source, const std::string& message);
    void Interaction(const std::string& widgetName, const std::string& action);
    void Notify(ToastKind kind, const std::string& title, const std::string& body, float ttl = 4.0f);

    // Undo helpers -------------------------------------------------------
    // Records a property edit if the state actually changed.
    void RecordEdit(const WidgetInstance& inst, const std::string& label, const WidgetState& before);
    void ResetWidgetState(WidgetInstance& inst);
    void DuplicateSelectedWidget();

    // Copy/paste of properties through the clipboard-like buffer below.
    std::string propertyClipboard;
    void CopySelectedProperties();
    void PastePropertiesIntoSelected();

    void CaptureFrameStats(float dt);
    std::string StatusLine() const;
};

} // namespace uilab
