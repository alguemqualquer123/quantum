#include "canvas/CanvasRenderer.h"

#include "core/AppContext.h"
#include "ui/Icons.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <imgui_internal.h>

namespace uilab {

namespace {

float Deg2Rad(float deg) { return deg * 3.14159265358979323846f / 180.0f; }

void RotatePoint(const ImVec2& p, const ImVec2& pivot, float radians, ImVec2& out) {
    const float s = std::sin(radians);
    const float c = std::cos(radians);
    const float dx = p.x - pivot.x;
    const float dy = p.y - pivot.y;
    out.x = pivot.x + dx * c - dy * s;
    out.y = pivot.y + dx * s + dy * c;
}

int RoundInt(float v) { return static_cast<int>(v < 0.0f ? v - 0.5f : v + 0.5f); }

// Screen-space rectangle of a node (no rotation applied).
void NodeScreenRect(const CanvasNode& n, const ImVec2& origin, float zoom, ImVec2& min, ImVec2& max) {
    min = CanvasToScreen(n.computed.min, origin, zoom);
    max = CanvasToScreen(n.computed.max, origin, zoom);
}

void FilledRotatedRect(ImDrawList* dl, ImVec2 min, ImVec2 max, float radians, ImVec2 pivot, ImU32 col,
                       float rounding) {
    if (rounding > 0.5f || std::fabs(radians) < 0.0001f) {
        if (std::fabs(radians) < 0.0001f) {
            dl->AddRectFilled(min, max, col, rounding);
            return;
        }
    }
    ImVec2 pts[4];
    RotatePoint(min, pivot, radians, pts[0]);
    RotatePoint(ImVec2(max.x, min.y), pivot, radians, pts[1]);
    RotatePoint(max, pivot, radians, pts[2]);
    RotatePoint(ImVec2(min.x, max.y), pivot, radians, pts[3]);
    dl->AddConvexPolyFilled(pts, 4, col);
}

void StrokedRotatedRect(ImDrawList* dl, ImVec2 min, ImVec2 max, float radians, ImVec2 pivot, ImU32 col,
                        float thickness) {
    ImVec2 pts[4];
    RotatePoint(min, pivot, radians, pts[0]);
    RotatePoint(ImVec2(max.x, min.y), pivot, radians, pts[1]);
    RotatePoint(max, pivot, radians, pts[2]);
    RotatePoint(ImVec2(min.x, max.y), pivot, radians, pts[3]);
    dl->AddPolyline(pts, 4, col, ImDrawFlags_Closed, thickness);
}

ImU32 WithOpacity(ImU32 col, float opacity) {
    const ImU32 a = (col >> IM_COL32_A_SHIFT) & 0xFFu;
    return (col & ~IM_COL32_A_MASK) | (static_cast<ImU32>(static_cast<float>(a) * opacity) << IM_COL32_A_SHIFT);
}

const icons::Icon kSlotIcons[4] = { icons::Icon::Heart, icons::Icon::Bolt, icons::Icon::Coin,
                                    icons::Icon::Shield };

} // namespace

ImVec2 CanvasToScreen(const ImVec2& canvasPoint, const ImVec2& origin, float zoom) {
    return ImVec2(origin.x + canvasPoint.x * zoom, origin.y + canvasPoint.y * zoom);
}

ImVec2 ScreenToCanvas(const ImVec2& screenPoint, const ImVec2& origin, float zoom) {
    const float z = zoom > 0.0001f ? zoom : 1.0f;
    return ImVec2((screenPoint.x - origin.x) / z, (screenPoint.y - origin.y) / z);
}

void DrawCanvasNode(ImDrawList* dl, const CanvasNode& node, const ImVec2& origin, float zoom, bool selected,
                    bool hovered, bool drawText) {
    ImVec2 min;
    ImVec2 max;
    NodeScreenRect(node, origin, zoom, min, max);
    const float w = max.x - min.x;
    const float h = max.y - min.y;
    if (w <= 0.5f || h <= 0.5f)
        return;

    const float opacity = ImClamp(node.opacity, 0.0f, 1.0f);
    const float radians = Deg2Rad(node.rotation);
    const ImVec2 pivot(min.x + w * node.pivot.x, min.y + h * node.pivot.y);
    const ImU32 fill = WithOpacity(ImGui::ColorConvertFloat4ToU32(node.color), opacity);
    const ImU32 textCol = WithOpacity(ImGui::ColorConvertFloat4ToU32(node.textColor), opacity);
    const ImU32 borderCol = WithOpacity(ImGui::GetColorU32(ImGuiCol_Border), opacity);
    const float rounding = node.type == CanvasNodeType::Button ? 4.0f : 3.0f;

    switch (node.type) {
    case CanvasNodeType::Panel:
    case CanvasNodeType::ScrollView: {
        FilledRotatedRect(dl, min, max, radians, pivot, fill, rounding);
        StrokedRotatedRect(dl, min, max, radians, pivot, borderCol, 1.0f);
        break;
    }
    case CanvasNodeType::Button: {
        const ImU32 accent = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive).x > 0.0f
                                 ? ImGui::GetColorU32(ImGuiCol_ButtonActive)
                                 : fill;
        const ImU32 base = hovered ? WithOpacity(accent, opacity) : fill;
        FilledRotatedRect(dl, min, max, radians, pivot, base, rounding);
        StrokedRotatedRect(dl, min, max, radians, pivot,
                           hovered ? WithOpacity(ImGui::GetColorU32(ImGuiCol_Text), opacity) : borderCol, 1.0f);
        break;
    }
    case CanvasNodeType::Label:
        break;
    case CanvasNodeType::ProgressBar:
    case CanvasNodeType::HealthBar: {
        const ImU32 bg = WithOpacity(ImGui::ColorConvertFloat4ToU32(ImVec4(0.05f, 0.05f, 0.07f, 0.85f)), opacity);
        FilledRotatedRect(dl, min, max, radians, pivot, bg, h * 0.35f);
        const float fraction = ImClamp(node.value, 0.0f, 1.0f);
        ImVec2 fillMax(max.x, max.y);
        if (node.type == CanvasNodeType::HealthBar)
            fillMax.x = min.x + w * fraction * 0.5f;
        else
            fillMax.x = min.x + w * fraction;
        if (node.type == CanvasNodeType::HealthBar) {
            // Health bars are drawn as two stacked bars: health (left) and a
            // shaded remainder so the fraction is easy to read.
            FilledRotatedRect(dl, min, ImVec2(min.x + w * 0.5f * fraction, max.y), radians, pivot, fill, h * 0.35f);
            FilledRotatedRect(dl, ImVec2(min.x + w * 0.5f, min.y), ImVec2(min.x + w * 0.5f + w * 0.5f * fraction, max.y),
                              radians, pivot, WithOpacity(fill, opacity * 0.55f), h * 0.35f);
        } else {
            FilledRotatedRect(dl, min, fillMax, radians, pivot, fill, h * 0.35f);
        }
        StrokedRotatedRect(dl, min, max, radians, pivot, borderCol, 1.0f);
        break;
    }
    case CanvasNodeType::Image: {
        widgets::DrawCheckerboard(dl, min, max, 10.0f, IM_COL32(60, 62, 70, 255), IM_COL32(78, 80, 90, 255));
        FilledRotatedRect(dl, min, max, radians, pivot, WithOpacity(fill, opacity * 0.85f), rounding);
        StrokedRotatedRect(dl, min, max, radians, pivot, borderCol, 1.0f);
        if (drawText && !node.text.empty())
            icons::Draw(dl, icons::Icon::Image, ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f - 6.0f),
                        ImMin(w, h) * 0.35f, textCol, 1.6f);
        break;
    }
    case CanvasNodeType::InventorySlot: {
        FilledRotatedRect(dl, min, max, radians, pivot, fill, rounding);
        StrokedRotatedRect(dl, min, max, radians, pivot,
                           hovered ? WithOpacity(ImGui::GetColorU32(ImGuiCol_ButtonActive), opacity) : borderCol,
                           1.2f);
        if (drawText) {
            const icons::Icon ic = kSlotIcons[static_cast<size_t>(node.iconIndex) % 4];
            icons::Draw(dl, ic, ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f - 3.0f), ImMin(w, h) * 0.45f,
                        textCol, 1.6f);
        }
        break;
    }
    default:
        FilledRotatedRect(dl, min, max, radians, pivot, fill, rounding);
        break;
    }

    // ---- labels ----
    // Text scales with the zoom so a shrunken preview keeps the design's
    // spacing; unscaled glyphs would collide inside small nodes.
    if (drawText) {
        const std::string text = node.text;
        if (!text.empty()) {
            ImFont* font = ImGui::GetFont();
            const float fontSize = ImGui::GetFontSize() * zoom;
            const ImVec2 ts = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text.c_str());
            const bool legible = fontSize >= 5.0f;
            const auto Draw = [&](const ImVec2& at, ImU32 col, const char* s) {
                if (legible)
                    dl->AddText(font, fontSize, at, col, s);
            };
            ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
            if (node.type == CanvasNodeType::Panel && h > 60.0f) {
                // Panels draw their title at the top-left, like a widget header.
                Draw(ImVec2(min.x + 8.0f * zoom, min.y + 6.0f * zoom), textCol, text.c_str());
            } else if (node.type == CanvasNodeType::ProgressBar) {
                const std::string label = text + "  " + widgets::Ellipsize(
                                                       std::to_string(static_cast<int>(node.value * 100.0f)) + "%",
                                                       44.0f * zoom);
                Draw(ImVec2(center.x - ts.x * 0.5f * 0.7f, center.y - ts.y * 0.5f),
                     WithOpacity(IM_COL32(255, 255, 255, 220), opacity), label.c_str());
            } else if (node.type == CanvasNodeType::Label) {
                // Labels are placed at the top-left of their rect so anchoring is visible.
                Draw(min, textCol, text.c_str());
            } else if (node.type == CanvasNodeType::InventorySlot) {
                Draw(ImVec2(center.x - ts.x * 0.5f, max.y - ts.y - 2.0f * zoom),
                     WithOpacity(ImGui::GetColorU32(ImGuiCol_TextDisabled), opacity), text.c_str());
            } else {
                Draw(ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f), textCol, text.c_str());
            }
        }
    }

    // ---- selection ----
    if (selected) {
        const ImU32 sel = ImGui::GetColorU32(ImGuiCol_ButtonActive);
        StrokedRotatedRect(dl, ImVec2(min.x - 1.0f, min.y - 1.0f), ImVec2(max.x + 1.0f, max.y + 1.0f), radians, pivot,
                           sel, 2.0f);
        const ImVec2 corners[4] = { min, ImVec2(max.x, min.y), max, ImVec2(min.x, max.y) };
        for (const ImVec2& c : corners) {
            ImVec2 r;
            RotatePoint(c, pivot, radians, r);
            dl->AddRectFilled(ImVec2(r.x - 3.0f, r.y - 3.0f), ImVec2(r.x + 3.0f, r.y + 3.0f), sel);
        }
        // Pivot marker
        dl->AddCircle(pivot, 5.0f, ImGui::GetColorU32(ImGuiCol_Text), 0, 1.5f);
        dl->AddCircle(pivot, 1.5f, ImGui::GetColorU32(ImGuiCol_Text), 0, 1.0f);
    }
}

CanvasFrameResult DrawCanvas(AppContext& ctx, CanvasView& view, const CanvasRenderOptions& options) {
    CanvasFrameResult result;

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    if (avail.x < 32.0f || avail.y < 32.0f) {
        ImGui::TextDisabled("Not enough space to draw the canvas.");
        return result;
    }

    ImGui::PushID(options.surfaceId);
    ImGui::InvisibleButton("##surface", avail,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool surfaceHovered = ImGui::IsItemHovered();
    const bool surfaceActive = ImGui::IsItemActive();
    ImGuiIO& io = ImGui::GetIO();

    // ---- auto fit ----
    if (view.autoFit) {
        const float fitX = (avail.x - 48.0f) / ImMax(1.0f, view.resolution.x);
        const float fitY = (avail.y - 48.0f) / ImMax(1.0f, view.resolution.y);
        view.zoom = ImClamp(ImMin(fitX, fitY), 0.15f, 3.0f);
        view.pan = ImVec2(0.0f, 0.0f);
    }

    // ---- zoom with the mouse wheel ----
    if (surfaceHovered && io.MouseWheel != 0.0f && (io.KeyCtrl || io.KeyShift)) {
        view.autoFit = false;
        const float oldZoom = view.zoom;
        view.zoom = ImClamp(view.zoom * (1.0f + io.MouseWheel * 0.12f), 0.2f, 4.0f);
        // Keep the point under the cursor stable.
        const ImVec2 center(pos.x + avail.x * 0.5f, pos.y + avail.y * 0.5f);
        const ImVec2 mouse = io.MousePos;
        view.pan.x -= (mouse.x - center.x - view.pan.x) * (view.zoom / oldZoom - 1.0f);
        view.pan.y -= (mouse.y - center.y - view.pan.y) * (view.zoom / oldZoom - 1.0f);
        ctx.settings.canvasZoom = view.zoom;
    }

    // ---- panning with the middle mouse button ----
    if (surfaceActive && ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
        view.autoFit = false;
        view.pan.x += io.MouseDelta.x;
        view.pan.y += io.MouseDelta.y;
    }

    const ImVec2 canvasSize(view.resolution.x * view.zoom, view.resolution.y * view.zoom);
    const ImVec2 origin(pos.x + (avail.x - canvasSize.x) * 0.5f + view.pan.x,
                        pos.y + (avail.y - canvasSize.y) * 0.5f + view.pan.y);

    ctx.scene.ComputeLayout(view.resolution);

    // ---- input ----
    int hovered = -1;
    int clicked = -1;
    const ImVec2 mouse = io.MousePos;
    const ImVec2 canvasMouse = ScreenToCanvas(mouse, origin, view.zoom);

    if (options.interactive && surfaceHovered) {
        hovered = ctx.scene.HitTest(canvasMouse);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            clicked = hovered;
    }

    // Drag handling (selection must exist and the pointer must be over the surface).
    CanvasNode* selectedNode = ctx.scene.Find(ctx.selection.sceneNodeId);
    static int draggingNode = -1;
    static ImVec2 dragOffset;

    if (options.interactive && surfaceActive) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hovered >= 0 && hovered != ctx.scene.RootId()) {
            draggingNode = hovered;
            CanvasNode* node = ctx.scene.Find(draggingNode);
            if (node)
                dragOffset = ImVec2(node->anchoredPosition.x - canvasMouse.x,
                                    node->anchoredPosition.y - canvasMouse.y);
            result.dragStarted = true;
        }
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && draggingNode >= 0) {
            CanvasNode* node = ctx.scene.Find(draggingNode);
            if (node) {
                ImVec2 target(canvasMouse.x + dragOffset.x, canvasMouse.y + dragOffset.y);
                if (node->layoutDriven) {
                    // Nodes controlled by a layout group keep their position.
                } else {
                    if (ctx.settings.snapToGrid && ctx.settings.gridSize > 1.0f) {
                        target.x = std::round(target.x / ctx.settings.gridSize) * ctx.settings.gridSize;
                        target.y = std::round(target.y / ctx.settings.gridSize) * ctx.settings.gridSize;
                    }
                    if (std::fabs(target.x - node->anchoredPosition.x) > 0.001f ||
                        std::fabs(target.y - node->anchoredPosition.y) > 0.001f) {
                        node->anchoredPosition = target;
                        result.dragMoved = true;
                        result.dragActive = true;
                        result.dragDelta = io.MouseDelta;
                    }
                }
            }
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && draggingNode >= 0) {
            if (result.dragMoved) {
                const CanvasNode* node = ctx.scene.Find(draggingNode);
                if (node) {
                    char buf[160];
                    std::snprintf(buf, sizeof(buf), "moved to (%.0f, %.0f)", static_cast<double>(node->anchoredPosition.x),
                                  static_cast<double>(node->anchoredPosition.y));
                    ctx.Interaction(node->name, buf);
                }
            }
            result.dropped = true;
            result.dragActive = false;
            draggingNode = -1;
        }
        if (ImGui::IsItemDeactivated())
            draggingNode = -1;
    } else if (!options.interactive) {
        draggingNode = -1;
    }

    if (clicked >= 0 && selectedNode && selectedNode->id == clicked) {
        // second click on the same node: report a double click through ImGui state
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            result.activatedButton = clicked;
    }
    if (clicked >= 0)
        result.clickedNode = clicked;

    // A single click on a button node triggers the button action.
    if (clicked >= 0) {
        const CanvasNode* node = ctx.scene.Find(clicked);
        if (node && node->type == CanvasNodeType::Button) {
            result.activatedButton = clicked;
            result.hoveredButton = true;
        }
    }

    // ---- draw ----
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(pos, ImVec2(pos.x + avail.x, pos.y + avail.y), true);

    // Background
    dl->AddRectFilled(pos, ImVec2(pos.x + avail.x, pos.y + avail.y),
                      ImGui::ColorConvertFloat4ToU32(options.background));
    const ImVec2 canvasMin = origin;
    const ImVec2 canvasMax(origin.x + canvasSize.x, origin.y + canvasSize.y);
    dl->AddRectFilled(canvasMin, canvasMax, ImGui::GetColorU32(ImGuiCol_FrameBg), 4.0f);

    if (options.showGrid) {
        widgets::DrawGrid(dl, canvasMin, canvasMax,
                          ImMax(4.0f, options.gridStep * view.zoom),
                          ImGui::GetColorU32(ImGuiCol_Separator, 0.35f), 1.0f);
    }

    // Scene nodes, back to front.
    const std::vector<int> order = ctx.scene.DrawOrder();
    for (int id : order) {
        if (id == ctx.scene.RootId())
            continue;
        const CanvasNode* node = ctx.scene.Find(id);
        if (!node)
            continue;
        const bool isSelected = id == ctx.selection.sceneNodeId;
        const bool isHovered = id == hovered;
        DrawCanvasNode(dl, *node, origin, view.zoom, isSelected, isHovered);
    }

    // Root canvas outline.
    dl->AddRect(canvasMin, canvasMax, ImGui::GetColorU32(ImGuiCol_Border), 4.0f, 0, 1.5f);

    // Debug overlays.
    if (options.showBounds || options.showAnchors) {
        for (int id : order) {
            const CanvasNode* node = ctx.scene.Find(id);
            if (!node || id == ctx.scene.RootId())
                continue;
            ImVec2 nmin;
            ImVec2 nmax;
            NodeScreenRect(*node, origin, view.zoom, nmin, nmax);
            if (options.showBounds)
                widgets::DrawDashedRect(dl, nmin, nmax, ImGui::GetColorU32(ImGuiCol_TextDisabled), 4.0f, 3.0f, 1.0f);
            if (options.showAnchors) {
                const ImU32 anchorCol = ImGui::GetColorU32(ImGuiCol_ButtonActive, 0.9f);
                const ImVec2 aMin = CanvasToScreen(node->anchoredRect.min, origin, view.zoom);
                const ImVec2 aMax = CanvasToScreen(node->anchoredRect.max, origin, view.zoom);
                dl->AddCircle(aMin, 4.0f, anchorCol, 0, 1.5f);
                dl->AddCircle(aMax, 4.0f, anchorCol, 0, 1.5f);
                dl->AddLine(ImVec2(aMin.x, aMin.y), ImVec2(aMin.x + 8.0f, aMin.y), anchorCol, 1.0f);
                dl->AddLine(ImVec2(aMin.x, aMin.y), ImVec2(aMin.x, aMin.y + 8.0f), anchorCol, 1.0f);
            }
            if (options.showBounds) {
                const std::string label = node->name + " " + std::to_string(RoundInt(node->computed.Width())) + "x" +
                                          std::to_string(RoundInt(node->computed.Height()));
                dl->AddText(ImVec2(nmin.x + 3.0f, nmin.y + 3.0f), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                            label.c_str());
            }
        }
    }

    // Hint overlay.
    if (surfaceHovered) {
        const char* hint = "click: select/drag  |  middle drag: pan  |  ctrl+wheel: zoom";
        const ImVec2 ts = ImGui::CalcTextSize(hint);
        dl->AddText(ImVec2(pos.x + avail.x - ts.x - 10.0f, pos.y + avail.y - ts.y - 8.0f),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), hint);
    }

    dl->PopClipRect();
    ImGui::PopID();

    result.hoveredNode = hovered;
    result.selectedNode = ctx.selection.sceneNodeId;
    result.surfaceMin = pos;
    result.surfaceMax = ImVec2(pos.x + avail.x, pos.y + avail.y);
    return result;
}

} // namespace uilab
