#include "ui/Icons.h"

#include <cmath>
#include <imgui_internal.h>

namespace uilab {
namespace icons {

const char* Name(Icon icon) {
    switch (icon) {
    case Icon::None:         return "none";
    case Icon::New:          return "new";
    case Icon::Open:         return "open";
    case Icon::Save:         return "save";
    case Icon::SaveAll:      return "save-all";
    case Icon::Undo:         return "undo";
    case Icon::Redo:         return "redo";
    case Icon::Play:         return "play";
    case Icon::Pause:        return "pause";
    case Icon::Stop:         return "stop";
    case Icon::Step:         return "step";
    case Icon::Grid:         return "grid";
    case Icon::Search:       return "search";
    case Icon::Gear:         return "gear";
    case Icon::Layers:       return "layers";
    case Icon::Cube:         return "cube";
    case Icon::Tree:         return "tree";
    case Icon::List:         return "list";
    case Icon::Widget:       return "widget";
    case Icon::Palette:      return "palette";
    case Icon::Trash:        return "trash";
    case Icon::Copy:         return "copy";
    case Icon::Refresh:      return "refresh";
    case Icon::Eye:          return "eye";
    case Icon::EyeOff:       return "eye-off";
    case Icon::Lock:         return "lock";
    case Icon::Unlock:       return "unlock";
    case Icon::ChevronDown:  return "chevron-down";
    case Icon::ChevronRight: return "chevron-right";
    case Icon::ChevronUp:    return "chevron-up";
    case Icon::ChevronLeft:  return "chevron-left";
    case Icon::Close:        return "close";
    case Icon::Check:        return "check";
    case Icon::Warning:      return "warning";
    case Icon::Info:         return "info";
    case Icon::Error:        return "error";
    case Icon::Star:         return "star";
    case Icon::Camera:       return "camera";
    case Icon::Sun:          return "sun";
    case Icon::Moon:         return "moon";
    case Icon::Bug:          return "bug";
    case Icon::Chart:        return "chart";
    case Icon::Dock:         return "dock";
    case Icon::PanelLeft:    return "panel-left";
    case Icon::PanelRight:   return "panel-right";
    case Icon::PanelBottom:  return "panel-bottom";
    case Icon::Sliders:      return "sliders";
    case Icon::Link:         return "link";
    case Icon::Plus:         return "plus";
    case Icon::Minus:        return "minus";
    case Icon::Filter:       return "filter";
    case Icon::Home:         return "home";
    case Icon::Folder:       return "folder";
    case Icon::File:         return "file";
    case Icon::Image:        return "image";
    case Icon::Brush:        return "brush";
    case Icon::Bolt:         return "bolt";
    case Icon::Heart:        return "heart";
    case Icon::Shield:       return "shield";
    case Icon::Coin:         return "coin";
    case Icon::Backpack:     return "backpack";
    case Icon::Point:        return "point";
    default:                 return "?";
    }
}

namespace {
// Helpers operating on a normalised (-1..1) space scaled around 'c'.
struct Ctx {
    ImDrawList* dl;
    ImVec2 c;
    float r;      // half size
    ImU32 col;
    float th;

    ImVec2 P(float x, float y) const { return ImVec2(c.x + x * r, c.y + y * r); }
    void Line(float x0, float y0, float x1, float y1) const { dl->AddLine(P(x0, y0), P(x1, y1), col, th); }
    void Rect(float x0, float y0, float x1, float y1, float rounding = 0.0f) const {
        dl->AddRect(P(x0, y0), P(x1, y1), col, rounding * r, 0, th);
    }
    void RectFilled(float x0, float y0, float x1, float y1, float rounding = 0.0f) const {
        dl->AddRectFilled(P(x0, y0), P(x1, y1), col, rounding * r);
    }
    void Circle(float x, float y, float radius) const {
        dl->AddCircle(P(x, y), radius * r, col, 0, th);
    }
    void CircleFilled(float x, float y, float radius) const {
        dl->AddCircleFilled(P(x, y), radius * r, col);
    }
    void Tri(float x0, float y0, float x1, float y1, float x2, float y2, bool filled = true) const {
        if (filled)
            dl->AddTriangleFilled(P(x0, y0), P(x1, y1), P(x2, y2), col);
        else
            dl->AddTriangle(P(x0, y0), P(x1, y1), P(x2, y2), col, th);
    }
    void Arc(float cx, float cy, float radius, float a0, float a1) const {
        dl->PathArcTo(P(cx, cy), radius * r, a0, a1, 24);
        dl->PathStroke(col, 0, th);
    }
};
} // namespace

bool Draw(ImDrawList* dl, Icon icon, ImVec2 center, float size, ImU32 col, float thickness) {
    if (!dl || icon == Icon::None || size <= 1.0f)
        return false;
    Ctx g{ dl, center, size * 0.5f, col, thickness };

    switch (icon) {
    case Icon::New:
        g.Rect(-0.85f, -0.85f, 0.35f, 0.85f, 0.12f);
        g.Line(0.35f, -0.85f, 0.85f, -0.35f);
        g.Line(0.85f, -0.35f, 0.35f, -0.35f);
        g.Line(0.35f, -0.85f, 0.35f, -0.35f);
        g.Line(0.55f, 0.25f, 0.55f, 0.85f);
        g.Line(0.25f, 0.55f, 0.85f, 0.55f);
        break;
    case Icon::Open:
    case Icon::Folder:
        g.Line(-0.9f, 0.7f, -0.9f, -0.55f);
        g.Line(-0.9f, -0.55f, -0.3f, -0.55f);
        g.Line(-0.3f, -0.55f, -0.1f, -0.3f);
        g.Line(-0.1f, -0.3f, 0.85f, -0.3f);
        g.Line(0.85f, -0.3f, 0.75f, 0.7f);
        g.Line(0.75f, 0.7f, -0.9f, 0.7f);
        break;
    case Icon::Save:
    case Icon::SaveAll:
        g.Rect(-0.8f, -0.85f, 0.8f, 0.85f, 0.1f);
        g.Rect(-0.5f, -0.85f, 0.5f, -0.15f, 0.05f);
        g.RectFilled(-0.25f, 0.1f, 0.25f, 0.85f, 0.05f);
        if (icon == Icon::SaveAll) {
            g.Line(0.2f, 0.55f, 0.85f, 0.55f);
            g.Line(0.85f, 0.55f, 0.85f, -0.1f);
        }
        break;
    case Icon::Undo:
        g.Arc(0.0f, 0.15f, 0.62f, IM_PI * 0.95f, IM_PI * 2.05f);
        g.Tri(-0.62f, 0.15f, -0.95f, -0.15f, -0.32f, -0.1f);
        break;
    case Icon::Redo:
        g.Arc(0.0f, 0.15f, 0.62f, IM_PI * 1.0f - 0.15f, IM_PI * 2.0f + 0.05f);
        g.Tri(0.62f, 0.15f, 0.95f, -0.15f, 0.32f, -0.1f);
        break;
    case Icon::Play:
        g.Tri(-0.55f, -0.8f, 0.8f, 0.0f, -0.55f, 0.8f);
        break;
    case Icon::Pause:
        g.RectFilled(-0.65f, -0.8f, -0.2f, 0.8f, 0.08f);
        g.RectFilled(0.2f, -0.8f, 0.65f, 0.8f, 0.08f);
        break;
    case Icon::Stop:
        g.RectFilled(-0.65f, -0.65f, 0.65f, 0.65f, 0.1f);
        break;
    case Icon::Step:
        g.Tri(-0.75f, -0.7f, 0.25f, 0.0f, -0.75f, 0.7f);
        g.Line(0.6f, -0.7f, 0.6f, 0.7f);
        break;
    case Icon::Grid:
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                const float x = -0.85f + static_cast<float>(i) * 0.62f;
                const float y = -0.85f + static_cast<float>(j) * 0.62f;
                g.Rect(x, y, x + 0.45f, y + 0.45f, 0.08f);
            }
        break;
    case Icon::Search:
        g.Circle(-0.2f, -0.2f, 0.55f);
        g.Line(0.2f, 0.2f, 0.85f, 0.85f);
        break;
    case Icon::Gear:
        g.Circle(0.0f, 0.0f, 0.35f);
        g.Circle(0.0f, 0.0f, 0.78f);
        for (int i = 0; i < 8; ++i) {
            const float a = static_cast<float>(i) * IM_PI * 0.25f;
            g.Line(std::cos(a) * 0.78f, std::sin(a) * 0.78f, std::cos(a) * 1.05f, std::sin(a) * 1.05f);
        }
        break;
    case Icon::Layers:
        g.Tri(-0.9f, -0.25f, 0.0f, -0.85f, 0.9f, -0.25f, false);
        g.Tri(-0.9f, 0.25f, 0.0f, -0.35f, 0.9f, 0.25f, false);
        g.Tri(-0.9f, 0.7f, 0.0f, 0.1f, 0.9f, 0.7f, false);
        break;
    case Icon::Cube:
        g.Tri(-0.85f, -0.35f, 0.0f, -0.85f, 0.85f, -0.35f, false);
        g.Tri(-0.85f, -0.35f, -0.85f, 0.45f, 0.0f, 0.9f, false);
        g.Tri(0.85f, -0.35f, 0.85f, 0.45f, 0.0f, 0.9f, false);
        g.Tri(0.0f, -0.85f, 0.0f, 0.05f, 0.85f, -0.35f, false);
        break;
    case Icon::Tree:
        g.Line(-0.7f, -0.6f, -0.2f, -0.6f);
        g.Line(-0.7f, -0.6f, -0.7f, 0.6f);
        g.Line(-0.7f, 0.0f, -0.2f, 0.0f);
        g.Line(-0.7f, 0.6f, -0.2f, 0.6f);
        g.RectFilled(0.05f, -0.75f, 0.85f, -0.45f, 0.08f);
        g.RectFilled(0.05f, -0.15f, 0.85f, 0.15f, 0.08f);
        g.RectFilled(0.05f, 0.45f, 0.85f, 0.75f, 0.08f);
        break;
    case Icon::List:
        for (int i = 0; i < 3; ++i) {
            const float y = -0.55f + static_cast<float>(i) * 0.55f;
            g.CircleFilled(-0.7f, y, 0.12f);
            g.Line(-0.35f, y, 0.85f, y);
        }
        break;
    case Icon::Widget:
        g.Rect(-0.85f, -0.85f, 0.85f, 0.85f, 0.15f);
        g.RectFilled(-0.55f, -0.55f, 0.2f, -0.1f, 0.06f);
        g.Line(-0.55f, 0.45f, 0.55f, 0.45f);
        break;
    case Icon::Palette:
    case Icon::Brush:
        g.Arc(0.0f, 0.1f, 0.6f, IM_PI * 0.75f, IM_PI * 2.35f);
        g.CircleFilled(-0.35f, -0.15f, 0.14f);
        g.CircleFilled(0.0f, -0.4f, 0.14f);
        g.CircleFilled(0.38f, -0.05f, 0.14f);
        g.CircleFilled(0.15f, 0.4f, 0.14f);
        break;
    case Icon::Trash:
        g.Line(-0.75f, -0.55f, 0.75f, -0.55f);
        g.Rect(-0.55f, -0.55f, 0.55f, 0.85f, 0.1f);
        g.Line(-0.3f, -0.55f, -0.3f, -0.85f);
        g.Line(0.3f, -0.55f, 0.3f, -0.85f);
        g.Line(-0.3f, -0.85f, 0.3f, -0.85f);
        break;
    case Icon::Copy:
        g.Rect(-0.85f, -0.85f, 0.25f, 0.25f, 0.1f);
        g.Rect(-0.25f, -0.25f, 0.85f, 0.85f, 0.1f);
        break;
    case Icon::Refresh:
        g.Arc(0.0f, 0.0f, 0.65f, IM_PI * 0.2f, IM_PI * 1.7f);
        g.Tri(0.65f, -0.1f, 0.95f, 0.25f, 0.35f, 0.2f);
        break;
    case Icon::Eye:
        g.Arc(0.0f, 0.15f, 0.85f, IM_PI * 1.15f, IM_PI * 1.85f);
        g.Arc(0.0f, -0.15f, 0.85f, IM_PI * 0.15f, IM_PI * 0.85f);
        g.Circle(0.0f, 0.0f, 0.3f);
        break;
    case Icon::EyeOff:
        g.Arc(0.0f, 0.15f, 0.85f, IM_PI * 1.15f, IM_PI * 1.85f);
        g.Line(-0.85f, 0.8f, 0.85f, -0.8f);
        break;
    case Icon::Lock:
        g.Rect(-0.6f, -0.05f, 0.6f, 0.85f, 0.1f);
        g.Arc(0.0f, -0.05f, 0.38f, IM_PI, IM_PI * 2.0f);
        break;
    case Icon::Unlock:
        g.Rect(-0.6f, -0.05f, 0.6f, 0.85f, 0.1f);
        g.Arc(0.15f, -0.05f, 0.38f, IM_PI, IM_PI * 1.85f);
        break;
    case Icon::ChevronDown:
        g.Line(-0.6f, -0.25f, 0.0f, 0.35f);
        g.Line(0.0f, 0.35f, 0.6f, -0.25f);
        break;
    case Icon::ChevronUp:
        g.Line(-0.6f, 0.25f, 0.0f, -0.35f);
        g.Line(0.0f, -0.35f, 0.6f, 0.25f);
        break;
    case Icon::ChevronRight:
        g.Line(-0.25f, -0.6f, 0.35f, 0.0f);
        g.Line(0.35f, 0.0f, -0.25f, 0.6f);
        break;
    case Icon::ChevronLeft:
        g.Line(0.25f, -0.6f, -0.35f, 0.0f);
        g.Line(-0.35f, 0.0f, 0.25f, 0.6f);
        break;
    case Icon::Close:
        g.Line(-0.65f, -0.65f, 0.65f, 0.65f);
        g.Line(0.65f, -0.65f, -0.65f, 0.65f);
        break;
    case Icon::Check:
        g.Line(-0.7f, 0.05f, -0.15f, 0.6f);
        g.Line(-0.15f, 0.6f, 0.75f, -0.6f);
        break;
    case Icon::Warning:
        g.Tri(0.0f, -0.85f, 0.9f, 0.75f, -0.9f, 0.75f, false);
        g.Line(0.0f, -0.35f, 0.0f, 0.28f);
        g.CircleFilled(0.0f, 0.55f, 0.09f);
        break;
    case Icon::Info:
        g.Circle(0.0f, 0.0f, 0.85f);
        g.CircleFilled(0.0f, -0.45f, 0.1f);
        g.Line(0.0f, -0.1f, 0.0f, 0.5f);
        break;
    case Icon::Error:
        g.Circle(0.0f, 0.0f, 0.85f);
        g.Line(-0.35f, -0.35f, 0.35f, 0.35f);
        g.Line(0.35f, -0.35f, -0.35f, 0.35f);
        break;
    case Icon::Star: {
        const float r0 = 0.85f;
        const float r1 = 0.36f;
        ImVec2 pts[10];
        for (int i = 0; i < 10; ++i) {
            const float a = -IM_PI * 0.5f + static_cast<float>(i) * IM_PI * 0.2f;
            const float rr = (i % 2 == 0) ? r0 : r1;
            pts[i] = g.P(std::cos(a) * rr, std::sin(a) * rr);
        }
        dl->AddPolyline(pts, 10, col, ImDrawFlags_Closed, g.th);
        break;
    }
    case Icon::Camera:
        g.Rect(-0.85f, -0.45f, 0.85f, 0.7f, 0.12f);
        g.Rect(-0.3f, -0.65f, 0.3f, -0.45f, 0.05f);
        g.Circle(0.0f, 0.12f, 0.32f);
        break;
    case Icon::Sun:
        g.Circle(0.0f, 0.0f, 0.38f);
        for (int i = 0; i < 8; ++i) {
            const float a = static_cast<float>(i) * IM_PI * 0.25f;
            g.Line(std::cos(a) * 0.6f, std::sin(a) * 0.6f, std::cos(a) * 0.9f, std::sin(a) * 0.9f);
        }
        break;
    case Icon::Moon:
        g.Arc(0.1f, 0.0f, 0.7f, IM_PI * 0.45f, IM_PI * 1.55f);
        g.Arc(-0.15f, -0.05f, 0.55f, IM_PI * 1.35f, IM_PI * 2.5f);
        break;
    case Icon::Bug:
        g.Circle(0.0f, 0.1f, 0.42f);
        g.Line(0.0f, -0.32f, 0.0f, 0.52f);
        for (int i = -1; i <= 1; ++i) {
            const float y = 0.1f + static_cast<float>(i) * 0.35f;
            g.Line(-0.42f, y, -0.85f, y + 0.15f);
            g.Line(0.42f, y, 0.85f, y + 0.15f);
        }
        break;
    case Icon::Chart:
        g.Line(-0.85f, 0.8f, 0.85f, 0.8f);
        g.RectFilled(-0.7f, 0.2f, -0.35f, 0.8f, 0.05f);
        g.RectFilled(-0.15f, -0.2f, 0.2f, 0.8f, 0.05f);
        g.RectFilled(0.4f, -0.65f, 0.75f, 0.8f, 0.05f);
        break;
    case Icon::Dock:
        g.Rect(-0.85f, -0.85f, 0.85f, 0.85f, 0.12f);
        g.Line(-0.85f, -0.3f, 0.85f, -0.3f);
        g.Line(0.1f, -0.3f, 0.1f, 0.85f);
        break;
    case Icon::PanelLeft:
        g.Rect(-0.85f, -0.85f, 0.85f, 0.85f, 0.12f);
        g.RectFilled(-0.85f, -0.85f, -0.2f, 0.85f, 0.12f);
        break;
    case Icon::PanelRight:
        g.Rect(-0.85f, -0.85f, 0.85f, 0.85f, 0.12f);
        g.RectFilled(0.2f, -0.85f, 0.85f, 0.85f, 0.12f);
        break;
    case Icon::PanelBottom:
        g.Rect(-0.85f, -0.85f, 0.85f, 0.85f, 0.12f);
        g.RectFilled(-0.85f, 0.2f, 0.85f, 0.85f, 0.12f);
        break;
    case Icon::Sliders:
        g.Line(-0.85f, -0.45f, 0.85f, -0.45f);
        g.Line(-0.85f, 0.45f, 0.85f, 0.45f);
        g.CircleFilled(-0.15f, -0.45f, 0.22f);
        g.CircleFilled(0.4f, 0.45f, 0.22f);
        break;
    case Icon::Link:
        g.Arc(-0.3f, -0.3f, 0.42f, IM_PI * 0.85f, IM_PI * 2.1f);
        g.Arc(0.3f, 0.3f, 0.42f, IM_PI * 1.85f, IM_PI * 3.1f);
        g.Line(-0.15f, 0.15f, 0.15f, -0.15f);
        break;
    case Icon::Plus:
        g.Line(0.0f, -0.75f, 0.0f, 0.75f);
        g.Line(-0.75f, 0.0f, 0.75f, 0.0f);
        break;
    case Icon::Minus:
        g.Line(-0.75f, 0.0f, 0.75f, 0.0f);
        break;
    case Icon::Filter:
        g.Tri(-0.85f, -0.7f, 0.85f, -0.7f, 0.0f, 0.2f, false);
        g.Line(0.0f, 0.2f, 0.0f, 0.8f);
        break;
    case Icon::Home:
        g.Tri(-0.9f, -0.05f, 0.0f, -0.85f, 0.9f, -0.05f, false);
        g.Rect(-0.6f, -0.05f, 0.6f, 0.8f, 0.08f);
        break;
    case Icon::File:
        g.Rect(-0.6f, -0.85f, 0.35f, 0.85f, 0.1f);
        g.Line(0.35f, -0.85f, 0.7f, -0.45f);
        g.Line(0.7f, -0.45f, 0.35f, -0.45f);
        g.Line(0.35f, -0.85f, 0.35f, -0.45f);
        break;
    case Icon::Image:
        g.Rect(-0.85f, -0.7f, 0.85f, 0.7f, 0.1f);
        g.Circle(0.35f, -0.3f, 0.16f);
        g.Tri(-0.7f, 0.55f, -0.15f, -0.15f, 0.3f, 0.55f);
        g.Tri(0.1f, 0.55f, 0.4f, 0.2f, 0.7f, 0.55f);
        break;
    case Icon::Bolt:
        g.Tri(-0.15f, -0.9f, 0.45f, -0.1f, -0.05f, -0.05f);
        g.Tri(0.1f, 0.9f, -0.45f, 0.05f, 0.02f, 0.0f);
        break;
    case Icon::Heart:
        g.Circle(-0.32f, -0.25f, 0.4f);
        g.Circle(0.32f, -0.25f, 0.4f);
        g.Tri(-0.72f, -0.1f, 0.72f, -0.1f, 0.0f, 0.8f);
        break;
    case Icon::Shield:
        g.Tri(-0.7f, -0.75f, 0.7f, -0.75f, 0.0f, 0.85f, false);
        g.Line(-0.7f, -0.75f, -0.7f, -0.1f);
        g.Line(0.7f, -0.75f, 0.7f, -0.1f);
        g.Line(-0.7f, -0.1f, 0.0f, 0.85f);
        g.Line(0.7f, -0.1f, 0.0f, 0.85f);
        break;
    case Icon::Coin:
        g.Circle(0.0f, 0.0f, 0.8f);
        g.Circle(0.0f, 0.0f, 0.55f);
        g.Line(0.0f, -0.3f, 0.0f, 0.3f);
        break;
    case Icon::Backpack:
        g.Rect(-0.7f, -0.35f, 0.7f, 0.8f, 0.15f);
        g.Arc(0.0f, -0.35f, 0.42f, IM_PI, IM_PI * 2.0f);
        g.Line(-0.7f, 0.15f, 0.7f, 0.15f);
        break;
    case Icon::Point:
        g.CircleFilled(0.0f, 0.0f, 0.35f);
        break;
    default:
        g.Rect(-0.5f, -0.5f, 0.5f, 0.5f, 0.1f);
        break;
    }
    return true;
}

bool IconButton(const char* id, Icon icon, const char* tooltip, ImVec2 size, bool active, bool enabled,
                float iconScale) {
    ImGui::PushID(id);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##btn", size) && enabled;
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 min = pos;
    const ImVec2 max = ImVec2(pos.x + size.x, pos.y + size.y);
    ImU32 bg = 0;
    if (!enabled)
        bg = ImGui::GetColorU32(ImGuiCol_Button, 0.35f);
    else if (active)
        bg = ImGui::GetColorU32(ImGuiCol_ButtonActive);
    else if (held)
        bg = ImGui::GetColorU32(ImGuiCol_ButtonActive);
    else if (hovered)
        bg = ImGui::GetColorU32(ImGuiCol_ButtonHovered);
    else
        bg = ImGui::GetColorU32(ImGuiCol_Button, active ? 1.0f : 0.65f);

    dl->AddRectFilled(min, max, bg, ImGui::GetStyle().FrameRounding);
    if (active)
        dl->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_ButtonActive), ImGui::GetStyle().FrameRounding, 0, 1.5f);

    const float iconSize = ImMin(size.x, size.y) * iconScale * 1.35f;
    const ImU32 fg = enabled ? (hovered || active ? ImGui::GetColorU32(ImGuiCol_Text)
                                                 : ImGui::GetColorU32(ImGuiCol_Text, 0.9f))
                             : ImGui::GetColorU32(ImGuiCol_TextDisabled);
    Draw(dl, icon, ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f), iconSize, fg, 1.6f);

    if (tooltip && *tooltip && hovered)
        ImGui::SetTooltip("%s", tooltip);
    ImGui::PopID();
    return clicked;
}

bool IconTextButton(const char* id, Icon icon, const char* label, ImVec2 size, bool active, bool enabled,
                    const char* tooltip) {
    ImGui::PushID(id);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##btn", size) && enabled;
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 min = pos;
    const ImVec2 max = ImVec2(pos.x + size.x, pos.y + size.y);
    ImU32 bg;
    if (!enabled)
        bg = ImGui::GetColorU32(ImGuiCol_Button, 0.35f);
    else if (active || held)
        bg = ImGui::GetColorU32(ImGuiCol_ButtonActive);
    else if (hovered)
        bg = ImGui::GetColorU32(ImGuiCol_ButtonHovered);
    else
        bg = ImGui::GetColorU32(ImGuiCol_Button, 0.65f);

    dl->AddRectFilled(min, max, bg, ImGui::GetStyle().FrameRounding);
    if (active)
        dl->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_ButtonActive), ImGui::GetStyle().FrameRounding, 0, 1.5f);

    const ImU32 fg = enabled ? ImGui::GetColorU32(ImGuiCol_Text) : ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const float iconSize = ImGui::GetFontSize() * 0.95f;
    const float textW = ImGui::CalcTextSize(label).x;
    const float gap = label && *label ? 6.0f : 0.0f;
    const float total = iconSize + gap + textW;
    float x = min.x + (size.x - total) * 0.5f;
    const float cy = (min.y + max.y) * 0.5f;
    if (icon != Icon::None) {
        Draw(dl, icon, ImVec2(x + iconSize * 0.5f, cy), iconSize * 1.25f, fg, 1.5f);
        x += iconSize + gap;
    }
    if (label && *label) {
        const ImVec2 ts = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2(x, cy - ts.y * 0.5f), fg, label);
    }
    if (tooltip && *tooltip && hovered)
        ImGui::SetTooltip("%s", tooltip);
    ImGui::PopID();
    return clicked;
}

void IconText(Icon icon, const char* label, ImU32 textColor) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = ImGui::GetFontSize();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    if (icon != Icon::None) {
        Draw(dl, icon, ImVec2(pos.x + h * 0.5f, pos.y + h * 0.35f), h * 0.95f, textColor, 1.4f);
        ImGui::Dummy(ImVec2(h * 1.15f, 0.0f));
        ImGui::SameLine(0.0f, 5.0f);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, textColor);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
}

Icon ForAssetType(int assetTypeIndex) {
    switch (assetTypeIndex) {
    case 0: return Icon::Folder;    // Folder
    case 1: return Icon::Widget;    // Blueprint
    case 2: return Icon::Brush;     // Material
    case 3: return Icon::Image;     // Texture
    case 4: return Icon::Cube;      // Mesh
    case 5: return Icon::Play;      // Animation
    case 6: return Icon::Bolt;      // Sound
    case 7: return Icon::Widget;    // Widget blueprint
    case 8: return Icon::File;      // Data asset
    case 9: return Icon::Home;      // Level
    default: return Icon::File;
    }
}

} // namespace icons
} // namespace uilab
