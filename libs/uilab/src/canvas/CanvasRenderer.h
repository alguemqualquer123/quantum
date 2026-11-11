// CanvasRenderer.h - draws and interacts with a CanvasScene (UMG / uGUI pages).
#pragma once

#include <string>

#include <imgui.h>

#include "canvas/CanvasScene.h"

namespace uilab {

class AppContext;

// Camera/zoom state of a canvas view (kept per demo page).
struct CanvasView {
    ImVec2 pan = ImVec2(0.0f, 0.0f);
    float zoom = 1.0f;
    bool autoFit = true;
    ImVec2 resolution = ImVec2(960.0f, 540.0f);
};

struct CanvasRenderOptions {
    bool showGrid = true;
    bool showBounds = false;
    bool showAnchors = false;
    bool interactive = true;
    bool dimNonSelected = false;
    float gridStep = 32.0f;
    const char* surfaceId = "canvas_surface";
    ImVec4 background = ImVec4(0.06f, 0.07f, 0.09f, 1.00f);

    // When valid, the renderer edits this component instead of the scene node
    // (used by the UMG "Widget Transform" demo).
    std::string transformWidgetId;
};

struct CanvasFrameResult {
    int hoveredNode = -1;
    int clickedNode = -1;
    int selectedNode = -1;
    int activatedButton = -1;   // button node clicked (single click)
    bool dragStarted = false;
    bool dragActive = false;
    bool dragMoved = false;
    bool dropped = false;
    bool hoveredButton = false;
    ImVec2 dragDelta = ImVec2(0.0f, 0.0f);
    ImVec2 surfaceMin = ImVec2(0.0f, 0.0f);
    ImVec2 surfaceMax = ImVec2(0.0f, 0.0f);
};

// Draws the canvas inside the remaining space of the current window and handles
// zoom/pan/selection/drag. Node rects are computed for 'view.resolution'.
CanvasFrameResult DrawCanvas(AppContext& ctx, CanvasView& view, const CanvasRenderOptions& options);

// Paints a single node (used by previews and by the renderer itself).
void DrawCanvasNode(ImDrawList* dl, const CanvasNode& node, const ImVec2& origin, float zoom, bool selected,
                    bool hovered, bool drawText = true);

// Maps a canvas (logical) point to screen coordinates.
ImVec2 CanvasToScreen(const ImVec2& canvasPoint, const ImVec2& origin, float zoom);
ImVec2 ScreenToCanvas(const ImVec2& screenPoint, const ImVec2& origin, float zoom);

} // namespace uilab
