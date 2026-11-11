// Settings.h - application/UI settings persisted between runs.
#pragma once

#include <string>

namespace uilab {

struct AppSettings {
    // panel visibility
    bool showToolbar = true;
    bool showComponentTree = true;
    bool showHierarchy = true;
    bool showInspector = true;
    bool showConsole = true;
    bool showAssetBrowser = true;
    bool showStatusBar = true;
    bool showThemeEditor = false;
    bool showImGuiDemo = false;
    bool showImGuiMetrics = false;
    bool showImGuiStyleEditor = false;
    bool showAbout = false;

    // behaviour
    bool dockingEnabled = true;
    bool saveLayoutOnExit = true;
    bool autoScrollConsole = true;
    bool logInteractions = true;
    bool logAllWidgetEdits = false;

    // canvas / preview
    bool showCanvasGrid = true;
    bool snapToGrid = false;
    float gridSize = 24.0f;
    float canvasZoom = 1.0f;
    bool drawBoundsOverlay = false;
    bool drawAnchorMarkers = false;

    // appearance
    bool compactMode = false;
    float uiFontScale = 1.0f;
    std::string themePath = "assets/themes/uicomp.ui-theme";
    std::string layoutIniPath = "default_layout.ini";
    std::string logExportPath = "logs/ui-components-lab.log";

    void Clamp();
    bool Validate(std::string& errorOut) const;
    std::string Describe() const;
};

} // namespace uilab
