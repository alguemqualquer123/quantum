#include "core/Settings.h"

#include <algorithm>
#include <cstdio>

namespace uilab {

void AppSettings::Clamp() {
    gridSize = std::clamp(gridSize, 4.0f, 256.0f);
    canvasZoom = std::clamp(canvasZoom, 0.25f, 4.0f);
    uiFontScale = std::clamp(uiFontScale, 0.75f, 2.0f);
    if (logExportPath.empty())
        logExportPath = "logs/ui-components-lab.log";
    if (layoutIniPath.empty())
        layoutIniPath = "default_layout.ini";
    if (themePath.empty())
        themePath = "assets/themes/uicomp.ui-theme";
}

bool AppSettings::Validate(std::string& errorOut) const {
    if (gridSize < 4.0f || gridSize > 256.0f) {
        errorOut = "Grid size must be between 4 and 256 pixels.";
        return false;
    }
    if (canvasZoom < 0.25f || canvasZoom > 4.0f) {
        errorOut = "Canvas zoom must be between 0.25 and 4.0.";
        return false;
    }
    if (uiFontScale < 0.75f || uiFontScale > 2.0f) {
        errorOut = "UI font scale must be between 0.75 and 2.0.";
        return false;
    }
    if (layoutIniPath.empty()) {
        errorOut = "Layout file path must not be empty.";
        return false;
    }
    errorOut.clear();
    return true;
}

std::string AppSettings::Describe() const {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "zoom %.2f | grid %.0f px (%s) | dock %s | font %.2f",
                  static_cast<double>(canvasZoom), static_cast<double>(gridSize),
                  snapToGrid ? "snap" : "free", dockingEnabled ? "on" : "off",
                  static_cast<double>(uiFontScale));
    return std::string(buf);
}

} // namespace uilab
