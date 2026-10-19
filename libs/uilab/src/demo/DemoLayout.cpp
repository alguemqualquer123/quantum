// DemoLayout.cpp - the Layout page: structural containers (boxes, grids, stacks,
// splitters, nested/responsive panels, anchors and spacing) plus the two windows
// the page owns: a splitter demo window and a free-floating dockable window.
//
// Everything renders from the bound WidgetState of the layout.* components, so
// resizing handles, re-anchoring and metric changes appear live.
#include "demo/DemoHost.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "core/AppContext.h"
#include "core/Types.h"
#include "panels/Panels.h"
#include "ui/Icons.h"
#include "ui/Widgets.h"

namespace uilab {
namespace demo {

namespace {

ImVec4 TileFill(const WidgetInstance& w, int index) {
    return index == 0 ? w.state.color : widgets::ColorFromIndex(index);
}
int TileCount(const WidgetInstance& w) { return std::max(1, std::min(8, w.state.intValue)); }

// A row / column of coloured tiles driven by the component spacing metrics.
void TileRow(WidgetInstance& w, bool vertical, float availW) {
    ImGui::PushID(w.id.c_str());
    const int count = TileCount(w);
    const float tileW = 84.0f;
    const float tileH = 34.0f;
    const float gap = vertical ? std::max(0.0f, w.state.spacingY) : std::max(0.0f, w.state.spacingX);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    auto drawTile = [&](int i) {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const ImVec2 max(pos.x + tileW, pos.y + tileH);
        dl->AddRectFilled(pos, max, ImGui::GetColorU32(TileFill(w, i)), 4.0f);
        char num[16];
        std::snprintf(num, sizeof(num), "%d", i + 1);
        const ImVec2 ts = ImGui::CalcTextSize(num);
        dl->AddText(ImVec2(pos.x + tileW * 0.5f - ts.x * 0.5f, pos.y + tileH * 0.5f - ts.y * 0.5f),
                    ImGui::GetColorU32(ImGuiCol_Text), num);
        ImGui::Dummy(ImVec2(tileW, tileH));
    };

    if (vertical) {
        for (int i = 0; i < count; ++i) {
            drawTile(i);
            if (i + 1 < count)
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + gap);
        }
        widgets::TextMuted("alignment: %s   spacingY: %.0f px", AlignName(w.state.alignment),
                           static_cast<double>(w.state.spacingY));
    } else {
        int perRow = 0;
        float acc = 0.0f;
        for (int n = 1; n <= count; ++n) {
            if (acc + tileW > availW && n > 1)
                break;
            perRow = n;
            acc += tileW + gap;
        }
        if (perRow <= 0)
            perRow = 1;
        const float rowW = perRow * tileW + (perRow - 1) * gap;
        const float factor = w.state.alignment == 0 ? 0.0f : (w.state.alignment == 2 ? 1.0f : 0.5f);
        const float startOffset = ImGui::GetCursorPosX() + std::max(0.0f, availW - rowW) * factor;
        for (int i = 0; i < count; ++i) {
            if (i % perRow == 0)
                ImGui::SetCursorPosX(startOffset);
            else
                ImGui::SameLine(0.0f, gap);
            drawTile(i);
        }
        widgets::TextMuted("alignment: %s   spacingX: %.0f px   tiles/row: %d", AlignName(w.state.alignment),
                           static_cast<double>(w.state.spacingX), perRow);
    }
    ImGui::PopID();
}

void DrawCollapsible(AppContext& ctx, WidgetInstance& w) {
    ImGui::PushID(w.id.c_str());
    static int s_last = -1;
    const bool frameChanged = (s_last != ctx.frameIndex);
    if (frameChanged)
        ImGui::SetNextItemOpen(w.state.toggled);

    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.33f, 0.78f, 0.42f, 0.16f));
    const bool open = ImGui::CollapsingHeader(w.state.text.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);
    ImGui::PopStyleColor();
    if (frameChanged)
        s_last = ctx.frameIndex;

    if (open) {
        ImGui::Indent();
        widgets::TextMuted("%s", w.state.hint.c_str());
        widgets::KeyValueRow("Binding", "layout.collapsible.section");
        ImGui::Unindent();
    }
    ImGui::PopID();
}

void DrawSplitter(AppContext& ctx, WidgetInstance& w, float availW) {
    ImGui::PushID(w.id.c_str());
    const float thickness = 6.0f;
    float ratio = std::clamp(w.state.value, w.state.minValue, w.state.maxValue);
    const float leftW = std::max(40.0f, (availW - thickness) * ratio);
    const float paneH = 150.0f;
    const float start = ImGui::GetCursorPosY();

    if (ImGui::BeginChild("##left", ImVec2(leftW, paneH), ImGuiChildFlags_Borders)) {
        const std::string wtxt = std::to_string(static_cast<int>(leftW));
        widgets::StatCard("Left pane", wtxt.c_str(), "px", w.state.color, ImVec2(0.0f, 0.0f));
        widgets::TextMuted("width %.0f px", static_cast<double>(leftW));
    }
    ImGui::EndChild();

    ImGui::SetCursorPos(ImVec2(leftW, start));
    const bool dragging = widgets::Splitter("##split", true, &ratio, w.state.minValue, w.state.maxValue, thickness);
    if (dragging)
        w.state.value = ratio;
    widgets::TrackEdit(ctx, w, "Split ratio");

    ImGui::SetCursorPos(ImVec2(leftW + thickness, start));
    const float rightW = std::max(40.0f, availW - leftW - thickness);
    if (ImGui::BeginChild("##right", ImVec2(rightW, paneH), ImGuiChildFlags_Borders)) {
        const std::string wtxt = std::to_string(static_cast<int>(rightW));
        const std::string ov = "ratio " + FormatFloat(ratio, "%.2f");
        widgets::StatCard("Right pane", wtxt.c_str(), "px", ImVec4(0.62f, 0.47f, 0.96f, 1.0f), ImVec2(0.0f, 0.0f));
        widgets::ProgressBarEx("##ratio", ratio, ImVec2(0.0f, 0.0f), ov.c_str(), w.state.color, false);
    }
    ImGui::EndChild();

    ImGui::SetCursorPosX(0.0f);
    ImGui::SetCursorPosY(start + paneH);
    widgets::TextMuted("drag the divider - bound to state.value (%.2f)", static_cast<double>(ratio));
    ImGui::PopID();
}

void DrawNestedPanels(WidgetInstance& w, float availW) {
    ImGui::PushID(w.id.c_str());
    const float outerH = 172.0f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 outerMax(pos.x + availW, pos.y + outerH);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, outerMax, ImGui::GetColorU32(ImGuiCol_FrameBg), 4.0f);
    dl->AddRect(pos, outerMax, ImGui::GetColorU32(ImGuiCol_Border), 4.0f, 0, 1.0f);

    const float padX = std::max(0.0f, w.state.spacingX);
    const float padY = std::max(0.0f, w.state.spacingY);
    const float gap = 10.0f;
    const float innerLeft = pos.x + padX;
    const float innerRight = outerMax.x - padX;
    const float childY = pos.y + padY + ImGui::GetTextLineHeight();
    const float childH = std::max(28.0f, outerMax.y - padY - childY);
    const int n = 3;
    const float childW = std::max(20.0f, (innerRight - innerLeft - gap * (n - 1)) / n);
    for (int i = 0; i < n; ++i) {
        const float x0 = innerLeft + i * (childW + gap);
        const ImVec2 cmin(x0, childY);
        const ImVec2 cmax(x0 + childW, childY + childH);
        dl->AddRectFilled(cmin, cmax, ImGui::GetColorU32(widgets::ColorFromIndex(i)), 3.0f);
        dl->AddRect(cmin, cmax, ImGui::GetColorU32(ImGuiCol_Border), 3.0f, 0, 1.0f);
        const std::string lbl = w.state.text + " " + std::to_string(i + 1);
        dl->AddText(ImVec2(cmin.x + 6.0f, cmin.y + 6.0f), ImGui::GetColorU32(ImGuiCol_Text), lbl.c_str());
    }
    ImGui::Dummy(ImVec2(availW, outerH));
    widgets::TextMuted("spacingX: %.0f   spacingY: %.0f", static_cast<double>(w.state.spacingX),
                       static_cast<double>(w.state.spacingY));
    ImGui::PopID();
}

void DrawResponsive(WidgetInstance& w, float availW) {
    ImGui::PushID(w.id.c_str());
    const int minTile = std::max(40, w.state.intValue);
    const float gap = std::max(0.0f, w.state.spacingX);
    const int cols = std::max(1, static_cast<int>((availW + gap) / (minTile + gap)));
    const float tileW = (availW - (cols - 1) * gap) / cols;
    const float tileH = 56.0f;
    const int rows = 3;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const float x = pos.x + c * (tileW + gap);
            const float y = pos.y + r * (tileH + gap);
            const ImVec2 min(x, y);
            const ImVec2 max(x + tileW, y + tileH);
            dl->AddRectFilled(min, max, ImGui::GetColorU32(widgets::ColorFromIndex((r * cols + c) % 8)), 4.0f);
            dl->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_Border), 4.0f, 0, 1.0f);
        }
    }
    ImGui::Dummy(ImVec2(availW, rows * tileH + (rows - 1) * gap));
    widgets::TextMuted("min tile %dpx -> %d columns", minTile, cols);
    ImGui::PopID();
}

void DrawAnchors(AppContext& ctx, WidgetInstance& w, float availW) {
    ImGui::PushID(w.id.c_str());
    const float areaH = 210.0f;
    if (ImGui::BeginChild("##anchor_area", ImVec2(availW, areaH), ImGuiChildFlags_Borders)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 aMin = ImGui::GetCursorScreenPos();
        const ImVec2 aMax(aMin.x + ImGui::GetContentRegionAvail().x, aMin.y + 150.0f);
        dl->AddRectFilled(aMin, aMax, ImGui::GetColorU32(ImGuiCol_FrameBg), 3.0f);
        dl->AddRect(aMin, aMax, ImGui::GetColorU32(ImGuiCol_Border), 3.0f, 0, 1.0f);

        const ImVec2 anchor = AnchorVec(static_cast<AnchorPreset>(w.state.anchorMin));
        float bx = aMin.x + anchor.x * (aMax.x - aMin.x) + w.state.position.x;
        float by = aMin.y + anchor.y * (aMax.y - aMin.y) + w.state.position.y;
        bx = std::min(bx, aMax.x - 8.0f);
        by = std::min(by, aMax.y - 8.0f);
        const ImVec2 bmax(std::max(bx + 12.0f, bx + w.state.size.x), std::max(by + 12.0f, by + w.state.size.y));
        dl->AddRectFilled(ImVec2(bx, by), bmax, ImGui::GetColorU32(w.state.color), 3.0f);
        dl->AddRect(ImVec2(bx, by), bmax, ImGui::GetColorU32(ImGuiCol_Text), 3.0f, 0, 1.0f);
        dl->AddText(ImVec2(bx + 6.0f, by + 6.0f), ImGui::GetColorU32(ImGuiCol_Text), w.state.text.c_str());
        ImGui::Dummy(ImVec2(std::max(1.0f, availW - 16.0f), 150.0f));

        int preset = std::clamp(w.state.anchorMin, 0, static_cast<int>(AnchorPreset::Count) - 1);
        const char* names[static_cast<int>(AnchorPreset::Count)];
        for (int i = 0; i < static_cast<int>(AnchorPreset::Count); ++i)
            names[i] = AnchorName(static_cast<AnchorPreset>(i));
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::Combo("##anchor", &preset, names, static_cast<int>(AnchorPreset::Count))) {
            w.state.anchorMin = preset;
            w.state.anchorMax = preset;
            w.state.pivot = preset;
            widgets::TrackEdit(ctx, w, "Anchor preset");
            ctx.Interaction(w.displayName, std::string("anchored ") + AnchorName(static_cast<AnchorPreset>(preset)));
        }
        widgets::TextMuted("anchor: %s   offset: (%.0f, %.0f)   size: (%.0f, %.0f)",
                           AnchorName(static_cast<AnchorPreset>(w.state.anchorMin)),
                           static_cast<double>(w.state.position.x), static_cast<double>(w.state.position.y),
                           static_cast<double>(w.state.size.x), static_cast<double>(w.state.size.y));
    }
    ImGui::EndChild();
    ImGui::PopID();
}

void DrawSpacing(AppContext& ctx, WidgetInstance& w) {
    ImGui::PushID(w.id.c_str());
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::DragFloat2("Spacing X/Y", &w.state.spacingX, 0.2f, 0.0f, 100.0f, "%.0f"))
        widgets::TrackEdit(ctx, w, "Spacing");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::DragFloat2("Padding X/Y", &w.state.paddingX, 0.2f, 0.0f, 100.0f, "%.0f"))
        widgets::TrackEdit(ctx, w, "Padding");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::DragFloat2("Margin X/Y", &w.state.marginX, 0.2f, 0.0f, 100.0f, "%.0f"))
        widgets::TrackEdit(ctx, w, "Margin");

    const float availW = ImGui::GetContentRegionAvail().x;
    const float boxW = 64.0f;
    const float boxH = 30.0f;
    const float gap = std::max(0.0f, w.state.spacingX);
    const float padX = std::max(0.0f, w.state.paddingX);
    const float padY = std::max(0.0f, w.state.paddingY);
    const float marX = std::max(0.0f, w.state.marginX);
    const float marY = std::max(0.0f, w.state.marginY);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int tiles = std::max(1, static_cast<int>((availW + gap) / (boxW + gap)));
    const float contentW = padX + tiles * boxW + (tiles - 1) * gap + padX;
    const float contentH = padY + boxH + padY;
    const ImVec2 padMin(pos.x + marX, pos.y + marY);
    dl->AddRect(ImVec2(pos.x, pos.y), ImVec2(pos.x + marX * 2.0f + contentW, pos.y + marY * 2.0f + contentH),
                ImGui::GetColorU32(ImGuiCol_TextDisabled), 4.0f, 0, 1.0f);
    dl->AddRect(padMin, ImVec2(padMin.x + contentW, padMin.y + contentH), ImGui::GetColorU32(ImGuiCol_Border), 3.0f,
                0, 1.0f);
    for (int i = 0; i < tiles; ++i) {
        const float x0 = padMin.x + padX + i * (boxW + gap);
        const ImVec2 bmin(x0, padMin.y + padY);
        const ImVec2 bmax(x0 + boxW, bmin.y + boxH);
        dl->AddRectFilled(bmin, bmax, ImGui::GetColorU32(widgets::ColorFromIndex(i % 8)), 3.0f);
    }
    ImGui::Dummy(ImVec2(std::max(availW, marX * 2.0f + contentW), marY * 2.0f + contentH));
    widgets::TextMuted("spacing %.0f   padding %.0f/%.0f   margin %.0f/%.0f", static_cast<double>(w.state.spacingX),
                       static_cast<double>(w.state.paddingX), static_cast<double>(w.state.paddingY),
                       static_cast<double>(w.state.marginX), static_cast<double>(w.state.marginY));
    ImGui::PopID();
}

void DrawDockableTrigger(AppContext& ctx, WidgetInstance& w) {
    ImGui::PushID(w.id.c_str());
    char label[96];
    std::snprintf(label, sizeof(label), "%s##docktgl", w.state.toggled ? "Close window" : "Spawn window");
    const float h = ImGui::GetFrameHeight();
    if (widgets::ToggleButton("##toggle", label, &w.state.toggled, ImVec2(180.0f, h))) {
        widgets::TrackEdit(ctx, w, "Toggled");
        ctx.Interaction(w.displayName, w.state.toggled ? "opened window" : "closed window");
    }
    ImGui::SameLine();
    widgets::StatusDot(w.state.toggled ? ImVec4(0.33f, 0.78f, 0.42f, 1.0f) : ImVec4(0.45f, 0.47f, 0.52f, 1.0f),
                       w.state.toggled ? "floating window open" : "window closed");

    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputText("##title", &w.state.text))
        widgets::TrackEdit(ctx, w, "Text");
    ImGui::PopID();
}

} // namespace

// ---------------------------------------------------------------------------
// Page
// ---------------------------------------------------------------------------
void DrawLayoutPage(AppContext& ctx) {
    const float availW = ImGui::GetContentRegionAvail().x;

    if (widgets::SectionHeader("Boxes & Grids", icons::Icon::Grid, true)) {
        if (WidgetInstance* w = Bind(ctx, "layout.box.horizontal"); w && w->state.visible) {
            widgets::SeparatorText("Horizontal box");
            TileRow(*w, false, availW);
        }
        ImGui::Spacing();
        if (WidgetInstance* w = Bind(ctx, "layout.box.vertical"); w && w->state.visible) {
            widgets::SeparatorText("Vertical box");
            TileRow(*w, true, availW);
        }
        ImGui::Spacing();
        if (WidgetInstance* w = Bind(ctx, "layout.grid.stats"); w && w->state.visible) {
            widgets::SeparatorText("Grid layout");
            ImGui::PushID(w->id.c_str());
            const int cols = std::max(1, std::min(6, w->state.intValue));
            const int count = TileCount(*w);
            char tlabel[64];
            std::snprintf(tlabel, sizeof(tlabel), "##grid_%s", w->id.c_str());
            if (ImGui::BeginTable(tlabel, cols, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchSame)) {
                for (int i = 0; i < count; ++i) {
                    if (i % cols == 0)
                        ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    const std::string num = std::to_string(i + 1);
                    widgets::StatCard(w->state.text.c_str(), num.c_str(), "#", widgets::ColorFromIndex(i),
                                      ImVec2(0.0f, 0.0f));
                }
                ImGui::EndTable();
            }
            widgets::TextMuted("%d columns x %d tiles   gap %.0f/%.0f px", cols, count,
                               static_cast<double>(w->state.spacingX), static_cast<double>(w->state.spacingY));
            ImGui::PopID();
        }
        ImGui::Spacing();
        if (WidgetInstance* w = Bind(ctx, "layout.stack.cards"); w && w->state.visible) {
            widgets::SeparatorText("Stack / overlay layout");
            ImGui::PushID(w->id.c_str());
            const int count = std::max(1, std::min(10, w->state.intValue));
            const float offset = std::max(0.0f, w->state.value);
            const float tileW = 200.0f;
            const float tileH = 44.0f;
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            for (int i = 0; i < count; ++i) {
                const float off = i * offset;
                const ImVec2 min(pos.x + off, pos.y + off);
                const ImVec2 max(min.x + tileW, min.y + tileH);
                const float shade = std::min(1.0f, 0.12f * (count - i));
                dl->AddRectFilled(min, max,
                                  ImGui::GetColorU32(LerpColor(w->state.color, ImVec4(0.0f, 0.0f, 0.0f, 1.0f), shade)),
                                  4.0f);
                dl->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_Border), 4.0f, 0, 1.0f);
                const std::string lbl = w->state.text + " " + std::to_string(count - i);
                dl->AddText(ImVec2(min.x + 8.0f, min.y + 8.0f), ImGui::GetColorU32(ImGuiCol_Text), lbl.c_str());
            }
            ImGui::Dummy(ImVec2(tileW + (count - 1) * offset, tileH + (count - 1) * offset));
            widgets::TextMuted("%d cards, each offset %.0f px (bound to state.value)", count,
                               static_cast<double>(offset));
            ImGui::PopID();
        }
    }

    if (widgets::SectionHeader("Containers", icons::Icon::Layers, true)) {
        if (WidgetInstance* w = Bind(ctx, "layout.splitter.main"); w && w->state.visible) {
            widgets::SeparatorText("Splitter");
            DrawSplitter(ctx, *w, availW);
        }
        ImGui::Spacing();
        if (WidgetInstance* w = Bind(ctx, "layout.collapsible.section"); w && w->state.visible)
            DrawCollapsible(ctx, *w);
        ImGui::Spacing();
        if (WidgetInstance* w = Bind(ctx, "layout.scroll.panel"); w && w->state.visible) {
            widgets::SeparatorText("Scrollable panel");
            ImGui::PushID(w->id.c_str());
            const int rows = std::max(1, std::min(400, w->state.intValue));
            const std::string listId = std::string("##rows_") + w->id;
            if (ImGui::BeginChild(listId.c_str(), ImVec2(availW, 170.0f), ImGuiChildFlags_Borders)) {
                for (int i = 0; i < rows; ++i) {
                    ImGui::PushID(i);
                    const std::string rowLabel = w->state.text + " " + std::to_string(i);
                    if (widgets::ListRow(rowLabel.c_str(), w->state.selectedIndex == i)) {
                        w->state.selectedIndex = i;
                        ctx.Interaction(w->displayName, "row " + std::to_string(i));
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
            widgets::TextMuted("%d rows   selected: %d", rows, w->state.selectedIndex);
            ImGui::PopID();
        }
        ImGui::Spacing();
        if (WidgetInstance* w = Bind(ctx, "layout.nested.panels"); w && w->state.visible) {
            widgets::SeparatorText("Nested panels");
            DrawNestedPanels(*w, availW);
        }
        ImGui::Spacing();
        if (WidgetInstance* w = Bind(ctx, "layout.responsive.grid"); w && w->state.visible) {
            widgets::SeparatorText("Responsive layout");
            DrawResponsive(*w, availW);
        }
    }

    if (widgets::SectionHeader("Alignment & Anchors", icons::Icon::Point, true)) {
        if (WidgetInstance* w = Bind(ctx, "layout.anchors.box"); w && w->state.visible) {
            widgets::SeparatorText("Anchors");
            DrawAnchors(ctx, *w, availW);
        }
        ImGui::Spacing();
        if (WidgetInstance* w = Bind(ctx, "layout.spacing.controls"); w && w->state.visible) {
            widgets::SeparatorText("Spacing & padding");
            DrawSpacing(ctx, *w);
        }
    }

    if (widgets::SectionHeader("Windows", icons::Icon::Dock, true)) {
        if (WidgetInstance* w = Bind(ctx, "layout.dockable.window"); w && w->state.visible) {
            widgets::SeparatorText("Dockable window");
            DrawDockableTrigger(ctx, *w);
        }
        ImGui::Spacing();
        const float bh = ImGui::GetFrameHeight();
        if (ImGui::Button(ctx.showSplitterDemoWindow ? "Close Splitter Window" : "Open Splitter Window",
                          ImVec2(0.0f, bh))) {
            ctx.showSplitterDemoWindow = !ctx.showSplitterDemoWindow;
            ctx.Interaction("Layout", ctx.showSplitterDemoWindow ? "opened splitter window"
                                                                 : "closed splitter window");
        }
    }

    DrawUnboundList(ctx, Category::Layout);
}

// ---------------------------------------------------------------------------
// Floating windows owned by this page
// ---------------------------------------------------------------------------
void DrawSplitterFloatingWindow(AppContext& ctx) {
    if (!ImGui::Begin("Splitter Demo", &ctx.showSplitterDemoWindow)) {
        ImGui::End();
        return;
    }
    static std::vector<std::string> items = { "Alpha", "Bravo", "Charlie", "Delta" };
    static int selected = 0;
    static float ratio = 0.42f;
    const int maxIndex = std::max(0, static_cast<int>(items.size()) - 1);
    selected = std::clamp(selected, 0, maxIndex);

    widgets::TextMuted("Drag the divider to resize the two panes; the list is editable.");
    ImGui::Spacing();
    if (icons::IconButton("##add", icons::Icon::Plus, "Add item", ImVec2(24.0f, 24.0f))) {
        items.push_back("Item " + std::to_string(items.size() + 1));
        ctx.Interaction("Splitter Window", "added item");
    }
    ImGui::SameLine();
    if (icons::IconButton("##del", icons::Icon::Trash, "Remove selected", ImVec2(24.0f, 24.0f), false,
                          !items.empty())) {
        if (!items.empty()) {
            items.erase(items.begin() + selected);
            ctx.Interaction("Splitter Window", "removed item");
        }
    }

    const float availW = ImGui::GetContentRegionAvail().x;
    const float availH = ImGui::GetContentRegionAvail().y;
    const float thickness = 6.0f;
    const float leftW = std::max(120.0f, (availW - thickness) * ratio);
    const float top = ImGui::GetCursorPosY();

    if (ImGui::BeginChild("##sleft", ImVec2(leftW, availH), ImGuiChildFlags_Borders)) {
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            ImGui::PushID(i);
            if (widgets::ListRow(items[i].c_str(), i == selected))
                selected = i;
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    ImGui::SetCursorPos(ImVec2(leftW, top));
    widgets::Splitter("##fsplit", true, &ratio, 0.15f, 0.85f, thickness);

    ImGui::SetCursorPos(ImVec2(leftW + thickness, top));
    if (ImGui::BeginChild("##sright", ImVec2(0.0f, availH), ImGuiChildFlags_Borders)) {
        if (!items.empty()) {
            widgets::TextMuted("Editing item %d", selected);
            if (ImGui::InputText("##itemtext", &items[selected]))
                ctx.Interaction("Splitter Window", "renamed an item");
        } else {
            widgets::TextMuted("(empty list - add an item)");
        }
        widgets::TextMuted("left width: %.0f px", static_cast<double>(leftW));
    }
    ImGui::EndChild();

    ImGui::End();
}

void DrawDockableFloatingWindow(AppContext& ctx) {
    WidgetInstance* w = Bind(ctx, "layout.dockable.window");
    const std::string display = (w && !w->state.text.empty()) ? w->state.text : std::string("Floating Window");
    const std::string winLabel = display + "###uicomp_layout_dockable_window";

    if (w) {
        ImGui::SetNextWindowPos(ImVec2(w->state.position.x, w->state.position.y), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(std::max(260.0f, w->state.size.x), std::max(180.0f, w->state.size.y)),
                                 ImGuiCond_FirstUseEver);
    }
    bool open = w ? w->state.toggled : true;
    // NoDock flag stays OFF so the window can be dragged back into the dockspace.
    const bool visible = ImGui::Begin(winLabel.c_str(), &open, ImGuiWindowFlags_None);
    if (visible) {
        ImGui::TextWrapped("A free-floating window whose title is bound to layout.dockable.window's state.text. "
                           "Drag its tab onto a dock node to re-dock it.");
        ImGui::Spacing();
        const float frac = PageClock(ctx, 0.3f);
        widgets::ProgressBarEx("##dockclock", frac, ImVec2(0.0f, 0.0f), "activity",
                               panels::AccentForCategory(Category::Layout), true);
        if (ImGui::Button("Say hello")) {
            ctx.Interaction(display, "hello");
            ctx.Notify(ToastKind::Info, display, "Hello from the dockable window.");
        }
        widgets::TextMuted("Close it with the title-bar X to reset the component's Toggled flag.");
    }
    ImGui::End();

    if (w) {
        if (w->state.toggled && !open)
            ctx.LogEvent("Layout", "closed floating window: " + display);
        w->state.toggled = open;
    }
}

} // namespace demo
} // namespace uilab
