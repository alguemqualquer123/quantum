// DemoImmediate.cpp - Unity IMGUI / classic debug window page: everything is
// rebuilt from scratch every frame, so "order" and "existence" are implicit.
#include "demo/DemoHost.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include "core/AppContext.h"
#include "panels/Panels.h"
#include "ui/Icons.h"
#include "ui/Widgets.h"

namespace uilab {
namespace demo {

namespace {

// File-scope storage for the dynamic controls demo: which control type each row
// uses plus the value that control edits.
struct DynRow {
    int type = 0;
    float fval = 0.5f;
    int ival = 5;
    bool bval = true;
    ImVec4 color = ImVec4(0.26f, 0.59f, 0.98f, 1.0f);
    std::string text = "Value";
};
std::vector<DynRow> g_dynRows;
std::vector<int> g_condFlags;

// Draws the mini widget that a conditional flag creates, or states that the
// widget is simply not submitted at all when the flag is off.
void DrawConditionalWidget(AppContext& ctx, int index, bool enabled) {
    if (!enabled) {
        ImGui::TextDisabled("(widget not submitted this frame)");
        return;
    }
    ImGui::PushID(index);
    static float s_slider = 0.4f;
    switch (index % 5) {
    case 0:
        if (ImGui::SliderFloat("##w", &s_slider, 0.0f, 1.0f))
            ++ctx.demo.sliderChanges;
        break;
    case 1:
        widgets::ProgressBarEx("##w", 0.3f + 0.4f * static_cast<float>(std::sin(ImGui::GetTime())),
                               ImVec2(140.0f, 0.0f), nullptr, ctx.theme.Active().accent);
        break;
    case 2: {
        static bool s_check = true;
        if (ImGui::Checkbox("##w", &s_check))
            ++ctx.demo.checkboxToggles;
        break;
    }
    case 3:
        if (ImGui::Button("Press"))
            ++ctx.demo.buttonClicks;
        break;
    default:
        widgets::StatusDot(ctx.theme.Active().success, "alive");
        break;
    }
    ImGui::PopID();
}

void DrawDynamicControlRow(AppContext& ctx, int index, DynRow& row, const std::vector<std::string>& typeNames) {
    ImGui::PushID(index);
    int type = row.type;
    if (!typeNames.empty())
        type = std::clamp(type, 0, static_cast<int>(typeNames.size()) - 1);
    char label[32];
    std::snprintf(label, sizeof(label), "Property %d", index + 1);
    ImGui::TextUnformatted(label);
    ImGui::SameLine(96.0f);
    if (!typeNames.empty()) {
        std::vector<const char*> namePtrs;
        namePtrs.reserve(typeNames.size());
        for (const std::string& n : typeNames)
            namePtrs.push_back(n.c_str());
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::Combo("##type", &type, namePtrs.data(), static_cast<int>(namePtrs.size()))) {
            if (row.type != type) {
                row.type = type;
                const int safe = std::clamp(type, 0, static_cast<int>(typeNames.size()) - 1);
                ctx.Interaction("Dynamic Controls",
                                std::string("row ") + std::to_string(index + 1) + " -> " + typeNames[safe]);
            }
        }
    } else {
        ImGui::TextDisabled("(no types)");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    switch (row.type) {
    case 0:
        if (ImGui::SliderFloat("##f", &row.fval, 0.0f, 1.0f))
            ++ctx.demo.sliderChanges;
        break;
    case 1:
        if (ImGui::DragInt("##i", &row.ival, 1.0f, 0, 999))
            ++ctx.demo.sliderChanges;
        break;
    case 2:
        if (ImGui::Checkbox("##b", &row.bval))
            ++ctx.demo.checkboxToggles;
        break;
    case 3:
        ImGui::ColorEdit4("##c", &row.color.x, ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_AlphaBar);
        break;
    default:
        ImGui::InputText("##t", &row.text);
        break;
    }
    ImGui::PopID();
}

} // namespace

// ---------------------------------------------------------------------------
// Page
// ---------------------------------------------------------------------------
void DrawImmediatePage(AppContext& ctx) {
    const ImVec4 accent = panels::AccentForCategory(Category::Immediate);
    const ThemePalette& pal = ctx.theme.Active();

    widgets::TextMuted("Nothing is retained: every widget below is recreated from the state each frame and "
                       "identified only by its declaration order.");
    ImGui::Separator();

    WidgetInstance* winTrig = Bind(ctx, "im.window.floating");
    WidgetInstance* cond = Bind(ctx, "im.conditional.flags");
    WidgetInstance* dyn = Bind(ctx, "im.dynamic.controls");
    WidgetInstance* dbg = Bind(ctx, "im.debug.overlay");
    WidgetInstance* statsW = Bind(ctx, "im.stats.graphs");
    WidgetInstance* anim = Bind(ctx, "im.update.animation");

    // ---- (a) Window trigger ---------------------------------------------
    if (widgets::SectionHeader("Immediate Mode Window", icons::Icon::Dock)) {
        if (winTrig)
            winTrig->state.toggled = ctx.showImmediateWindow;    // mirror the real window state
        if (winTrig && widgets::ToggleButton("##imwin", winTrig->state.text.c_str(), &ctx.showImmediateWindow,
                                             ImVec2(0.0f, 0.0f), "Spawns a floating window drawn every frame")) {
            ctx.Interaction(winTrig->displayName, ctx.showImmediateWindow ? "window opened" : "window closed");
            ctx.LogEvent("IMGUI", ctx.showImmediateWindow ? "Debug window shown" : "Debug window hidden");
        }
        ImGui::SameLine();
        if (widgets::ToggleButton("##imoverlay", "Debug Overlay", &ctx.showDebugOverlayWindow, ImVec2(0.0f, 0.0f),
                                  "Borderless on-screen overlay")) {
            ctx.Interaction("Debug Overlay", ctx.showDebugOverlayWindow ? "shown" : "hidden");
        }
        if (winTrig) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "Window rect: (%.0f, %.0f)  %.0f x %.0f - editable in the Inspector",
                          static_cast<double>(winTrig->state.position.x), static_cast<double>(winTrig->state.position.y),
                          static_cast<double>(winTrig->state.size.x), static_cast<double>(winTrig->state.size.y));
            widgets::TextMuted("%s", buf);
        }
    }

    // ---- (b) Conditional widgets -----------------------------------------
    if (widgets::SectionHeader("Conditional Widgets", icons::Icon::Eye)) {
        if (!cond) {
            widgets::TextMuted("Component im.conditional.flags is missing.");
        } else {
            WidgetState& s = cond->state;
            g_condFlags.resize(s.items.size(), 1);
            ImGui::TextWrapped("In immediate mode a widget exists exactly as long as it is submitted. Toggling a "
                               "flag below creates or destroys the widget on the very next frame - no animation, "
                               "no layout pass, and the "
                               "submission order defines identity.");
            if (ImGui::Checkbox(s.text.c_str(), &s.toggled)) {
                ++ctx.demo.checkboxToggles;
                ctx.Interaction(cond->displayName, s.toggled ? "advanced shown" : "advanced hidden");
            }
            ImGui::Indent();
            if (!s.toggled) {
                widgets::TextMuted("(hidden section - the rows below are simply not drawn)");
            } else {
                for (int i = 0; i < static_cast<int>(s.items.size()); ++i) {
                    const bool before = g_condFlags[i] != 0;
                    bool now = before;
                    if (ImGui::Checkbox(s.items[i].c_str(), &now)) {
                        g_condFlags[i] = now ? 1 : 0;
                        ++ctx.demo.checkboxToggles;
                        ctx.Interaction("Conditional", s.items[i] + (now ? std::string(" on") : std::string(" off")));
                    }
                    ImGui::SameLine(180.0f);
                    DrawConditionalWidget(ctx, i, now);
                }
            }
            ImGui::Unindent();
        }
    }

    // ---- (c) Dynamic controls ----------------------------------------------
    if (widgets::SectionHeader("Dynamic Controls", icons::Icon::Sliders)) {
        if (!dyn) {
            widgets::TextMuted("Component im.dynamic.controls is missing.");
        } else {
            WidgetState& s = dyn->state;
            const int lo = std::max(1, s.intMin);
            const int hi = std::max(lo, s.intMax);
            s.intValue = std::clamp(s.intValue, lo, hi);
            g_dynRows.resize(static_cast<size_t>(s.intValue));
            for (size_t i = 0; i < g_dynRows.size(); ++i)
                g_dynRows[i].type = std::clamp(g_dynRows[i].type, 0,
                                               s.items.empty() ? 0 : static_cast<int>(s.items.size()) - 1);

            if (ImGui::SliderInt("##count", &s.intValue, lo, hi, "Rows: %d")) {
                widgets::TrackEdit(ctx, *dyn, "Row count");
                ++ctx.demo.sliderChanges;
            }
            ImGui::SameLine();
            if (ImGui::Button("+")) {
                const WidgetState before = s;
                s.intValue = std::min(s.intValue + 1, hi);
                ctx.RecordEdit(*dyn, "Add row", before);
                ctx.Interaction("Dynamic Controls", "row added");
            }
            ImGui::SameLine();
            if (ImGui::Button("-")) {
                const WidgetState before = s;
                s.intValue = std::max(s.intValue - 1, lo);
                ctx.RecordEdit(*dyn, "Remove row", before);
                ctx.Interaction("Dynamic Controls", "row removed");
            }
            ImGui::SameLine();
            char rowBadge[32];
            std::snprintf(rowBadge, sizeof(rowBadge), "%d rows", s.intValue);
            widgets::Badge(rowBadge, accent);

            if (ImGui::BeginChild("##dynrows", ImVec2(0.0f, 200.0f), ImGuiChildFlags_Borders)) {
                for (int i = 0; i < static_cast<int>(g_dynRows.size()); ++i)
                    DrawDynamicControlRow(ctx, i, g_dynRows[i], s.items);
                if (g_dynRows.empty())
                    widgets::TextMuted("(no rows - raise the count)");
            }
            ImGui::EndChild();
            widgets::TextMuted("Types come from state.items: %s",
                               s.items.empty() ? "(empty)" : s.items[0].c_str());
        }
    }

    // ---- (d) Debug panel ----------------------------------------------------
    if (widgets::SectionHeader("Debug Panel", icons::Icon::Bug)) {
        if (!dbg) {
            widgets::TextMuted("Component im.debug.overlay is missing.");
        } else {
            WidgetState& s = dbg->state;
            bool* settingsFlags[4] = { &ctx.settings.drawBoundsOverlay, &ctx.settings.drawAnchorMarkers,
                                       &ctx.settings.showCanvasGrid, &ctx.settings.logInteractions };
            static bool s_extra = false;
            for (int i = 0; i < static_cast<int>(s.items.size()); ++i) {
                bool* target = i < 4 ? settingsFlags[i] : &s_extra;
                if (ImGui::Checkbox(s.items[i].c_str(), target)) {
                    ctx.Interaction("Debug Panel", s.items[i] + (*target ? std::string(" on") : std::string(" off")));
                }
                ImGui::SameLine();
                widgets::StatusDot(*target ? pal.success : pal.textDisabled, *target ? "on" : "off");
            }
            ImGui::TextDisabled("These toggles write straight into AppSettings - the canvas and the console react "
                                "immediately. Events this second: %d", ctx.demo.eventsThisSecond);
        }
    }

    // ---- (e) Real-time statistics -------------------------------------------
    if (widgets::SectionHeader("Real-time Statistics", icons::Icon::Chart)) {
        WidgetState* s = statsW ? &statsW->state : nullptr;
        const char* budget = ctx.stats.FrameBudgetLabel();
        char fpsBuf[32];
        std::snprintf(fpsBuf, sizeof(fpsBuf), "%.0f", static_cast<double>(ctx.stats.currentFps));
        char msBuf[32];
        std::snprintf(msBuf, sizeof(msBuf), "%.2f", static_cast<double>(ctx.stats.currentFrameMs));
        const float cardW = 130.0f;
        widgets::StatCard("FPS", fpsBuf, "Hz", ctx.stats.currentFps > 50.0f ? pal.success : pal.warning,
                          ImVec2(cardW, 56.0f));
        ImGui::SameLine();
        widgets::StatCard("Frame", msBuf, "ms", pal.accent, ImVec2(cardW, 56.0f));
        ImGui::SameLine();
        widgets::StatCard("Budget", budget, "", pal.info, ImVec2(cardW, 56.0f));
        ImGui::SameLine();
        widgets::StatCard("Draw Calls", std::to_string(ctx.stats.uiDrawCalls).c_str(), "", pal.warning,
                          ImVec2(cardW, 56.0f));

        int window = 60;
        if (s) {
            if (ImGui::SliderFloat("##window", &s->value, s->minValue, s->maxValue, "Window: %.0f frames")) {
                widgets::TrackEdit(ctx, *statsW, "Graph window");
                ++ctx.demo.sliderChanges;
            }
            window = std::clamp(static_cast<int>(s->value), 1, static_cast<int>(FrameStats::kMaxSamples));
        }

        if (ctx.stats.frameMs.empty()) {
            widgets::TextMuted("(no samples yet)");
        } else if (ImGui::BeginTable("##im_graphs", 2,
                                     ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoPadInnerX)) {
            ImGui::TableNextColumn();
            ImGui::TextDisabled("Frame time (ms)");
            const int total = static_cast<int>(ctx.stats.frameMs.size());
            const int count = std::min(window, total);
            const float* msData = ctx.stats.frameMs.data() + (total - count);
            float msMax = 33.4f;
            for (int i = 0; i < count; ++i)
                msMax = std::max(msMax, msData[i]);
            ImGui::PlotLines("##ms", msData, count, 0, nullptr, 0.0f, msMax * 1.1f, ImVec2(-FLT_MIN, 80.0f));
            ImGui::TextDisabled("min %.2f / max %.2f ms", static_cast<double>(ctx.stats.minFrameMs),
                                static_cast<double>(ctx.stats.maxFrameMs));

            ImGui::TableNextColumn();
            ImGui::TextDisabled("FPS");
            const int fpsTotal = static_cast<int>(ctx.stats.fpsHistory.size());
            const int fpsCount = std::min(window, fpsTotal);
            if (fpsCount > 0)
                ImGui::PlotLines("##fps", ctx.stats.fpsHistory.data() + (fpsTotal - fpsCount), fpsCount, 0, nullptr,
                                 0.0f, 120.0f, ImVec2(-FLT_MIN, 80.0f));
            ImGui::TextDisabled("average %.1f fps", static_cast<double>(ctx.stats.averageFps));

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("Draw calls");
            const int dcTotal = static_cast<int>(ctx.stats.drawCalls.size());
            const int dcCount = std::min(window, dcTotal);
            if (dcCount > 0)
                ImGui::PlotHistogram("##dc", ctx.stats.drawCalls.data() + (dcTotal - dcCount), dcCount, 0, nullptr,
                                     0.0f, FLT_MAX, ImVec2(-FLT_MIN, 60.0f));

            ImGui::TableNextColumn();
            ImGui::TextDisabled("Vertices");
            const int vTotal = static_cast<int>(ctx.stats.vertices.size());
            const int vCount = std::min(window, vTotal);
            if (vCount > 0)
                ImGui::PlotHistogram("##verts", ctx.stats.vertices.data() + (vTotal - vCount), vCount, 0, nullptr,
                                     0.0f, FLT_MAX, ImVec2(-FLT_MIN, 60.0f));
            ImGui::EndTable();
        }
    }

    // ---- (f) Per-frame update -----------------------------------------------
    if (widgets::SectionHeader("Per-frame Update", icons::Icon::Bolt)) {
        float speed = 1.0f;
        if (anim) {
            if (ImGui::SliderFloat("##speed", &anim->state.value, anim->state.minValue, anim->state.maxValue,
                                   "Speed: %.2f cycles/s")) {
                widgets::TrackEdit(ctx, *anim, "Animation speed");
                ++ctx.demo.sliderChanges;
            }
            speed = std::max(0.0f, anim->state.value);
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Playing", &ctx.demo.playing)) {
            ctx.Interaction("Per-frame Update", ctx.demo.playing ? "play" : "pause");
            ctx.LogEvent("IMGUI", std::string("playing = ") + BoolLabel(ctx.demo.playing));
        }
        const float clock = PageClock(ctx, speed);

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float width = avail.x;
        const float height = 80.0f;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), ImGui::GetColorU32(ImGuiCol_FrameBg),
                          4.0f);
        const int bars = 48;
        const float barW = width / static_cast<float>(bars);
        for (int i = 0; i < bars; ++i) {
            const float phase = clock * 2.0f * 3.14159265f + static_cast<float>(i) * 0.35f;
            const float amp = 0.5f + 0.5f * std::sin(phase);
            const float h = std::max(2.0f, amp * (height - 8.0f));
            const ImVec2 bMin(origin.x + i * barW + 1.0f, origin.y + height - 4.0f - h);
            const ImVec2 bMax(origin.x + (i + 1) * barW - 1.0f, origin.y + height - 4.0f);
            dl->AddRectFilled(bMin, bMax, ImGui::GetColorU32(ImLerp(pal.accent, pal.info, amp)), 2.0f);
        }
        ImGui::Dummy(ImVec2(width, height));
        char phaseBuf[96];
        std::snprintf(phaseBuf, sizeof(phaseBuf), "clock phase: %.2f - redrawn from scratch every frame",
                      static_cast<double>(clock));
        widgets::TextMuted("%s", phaseBuf);
        if (anim)
            widgets::TextMuted("State text (live-bound): %s", anim->state.text.c_str());
    }

    DrawUnboundList(ctx, Category::Immediate);
}

// ---------------------------------------------------------------------------
// Floating debug window
// ---------------------------------------------------------------------------
void DrawImmediateFloatingWindow(AppContext& ctx) {
    if (!ctx.showImmediateWindow)
        return;

    WidgetInstance* w = Bind(ctx, "im.window.floating");
    if (w) {
        ImGui::SetNextWindowPos(w->state.position, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(w->state.size, ImGuiCond_FirstUseEver);
    }
    const ThemePalette& pal = ctx.theme.Active();
    if (!ImGui::Begin("Debug (Immediate Mode)", &ctx.showImmediateWindow, ImGuiWindowFlags_NoDocking)) {
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("Rebuilt from scratch every frame - no retained widgets.");
    widgets::StatusDot(ctx.demo.playing ? pal.success : pal.warning, ctx.demo.playing ? "simulating" : "paused");

    char overlay[64];
    std::snprintf(overlay, sizeof(overlay), "%.2f ms", static_cast<double>(ctx.stats.currentFrameMs));
    const int total = static_cast<int>(ctx.stats.frameMs.size());
    if (total > 0) {
        const int count = std::min(total, 90);
        ImGui::PlotLines("##mini", ctx.stats.frameMs.data() + (total - count), count, 0, overlay, 0.0f,
                        FLT_MAX, ImVec2(-FLT_MIN, 60.0f));
    } else {
        widgets::TextMuted("(waiting for samples)");
    }

    ImGui::SeparatorText("Counters");
    widgets::KeyValueRow("FPS", std::to_string(static_cast<int>(ctx.stats.currentFps)).c_str());
    char msBuf[32];
    std::snprintf(msBuf, sizeof(msBuf), "%.2f ms", static_cast<double>(ctx.stats.currentFrameMs));
    widgets::KeyValueRow("Frame time", msBuf);
    widgets::KeyValueRow("Budget", ctx.stats.FrameBudgetLabel());
    widgets::KeyValueRow("Frames", std::to_string(ctx.stats.frameCount).c_str());
    widgets::KeyValueRow("Uptime", std::to_string(static_cast<int>(ctx.stats.uptime)).c_str());
    widgets::KeyValueRow("Draw calls", std::to_string(ctx.stats.uiDrawCalls).c_str());
    widgets::KeyValueRow("Vertices", std::to_string(ctx.stats.uiVertices).c_str());
    widgets::KeyValueRow("Events / s", std::to_string(ctx.demo.eventsThisSecond).c_str());
    widgets::KeyValueRow("Clicks", std::to_string(ctx.demo.buttonClicks).c_str());

    ImGui::Separator();
    if (ImGui::Button("Press me")) {
        ++ctx.demo.buttonClicks;
        ctx.Interaction("Debug Window", "button pressed");
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear counters")) {
        ctx.demo.buttonClicks = 0;
        ctx.demo.sliderChanges = 0;
        ctx.demo.checkboxToggles = 0;
        ctx.LogEvent("IMGUI", "Counters cleared");
    }
    ImGui::SameLine();
    if (ImGui::Button("Close"))
        ctx.showImmediateWindow = false;

    ImGui::End();
}

// ---------------------------------------------------------------------------
// On-screen overlay window
// ---------------------------------------------------------------------------
void DrawDebugOverlayWindow(AppContext& ctx) {
    if (!ctx.showDebugOverlayWindow)
        return;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 268.0f, vp->WorkPos.y + 64.0f),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(250.0f, 170.0f), ImGuiCond_FirstUseEver);
    // NoTitleBar + NoBackground + NoResize, but the window stays movable and
    // closable through &ctx.showDebugOverlayWindow.
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBackground |
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoNavInputs |
                                   ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (!ImGui::Begin("##immediate_debug_overlay", &ctx.showDebugOverlayWindow, flags)) {
        ImGui::End();
        return;
    }

    const ThemePalette& pal = ctx.theme.Active();
    ImGui::PushStyleColor(ImGuiCol_Text, pal.warning);
    ImGui::TextUnformatted("DEBUG OVERLAY");
    ImGui::PopStyleColor();
    char line[96];
    std::snprintf(line, sizeof(line), "%5.0f fps   %6.2f ms   %s", static_cast<double>(ctx.stats.currentFps),
                  static_cast<double>(ctx.stats.currentFrameMs), ctx.stats.FrameBudgetLabel());
    ImGui::TextColored(pal.accent, "%s", line);
    std::snprintf(line, sizeof(line), "draw %d   verts %d   wins %d", ctx.stats.uiDrawCalls, ctx.stats.uiVertices,
                  ctx.stats.uiWindows);
    ImGui::TextDisabled("%s", line);
    std::snprintf(line, sizeof(line), "frames %d   events/s %d", ctx.stats.frameCount, ctx.demo.eventsThisSecond);
    ImGui::TextDisabled("%s", line);
    if (ImGui::SmallButton("hide")) {
        ctx.showDebugOverlayWindow = false;
        ctx.Interaction("Debug Overlay", "hidden");
    }

    // Decoration: dashed frame + arrows pointing at the two stat lines.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wPos = ImGui::GetWindowPos();
    const ImVec2 wSize = ImGui::GetWindowSize();
    widgets::DrawDashedRect(dl, ImVec2(wPos.x + 2.0f, wPos.y + 2.0f),
                            ImVec2(wPos.x + wSize.x - 2.0f, wPos.y + wSize.y - 2.0f),
                            ImGui::GetColorU32(WithAlpha(pal.accent, 0.8f)), 6.0f, 4.0f, 1.0f);
    const ImVec2 fpsRect = ImGui::GetItemRectMin();    // the "hide" button row
    const ImVec2 from(wPos.x + wSize.x - 14.0f, wPos.y + wSize.y - 14.0f);
    widgets::DrawArrow(dl, from, ImVec2(fpsRect.x + 8.0f, fpsRect.y + 4.0f), ImGui::GetColorU32(pal.warning), 1.5f,
                       7.0f);
    const ImVec2 headerMin = ImVec2(wPos.x + 10.0f, wPos.y + 8.0f);
    widgets::DrawArrow(dl, ImVec2(wPos.x + 14.0f, wPos.y + wSize.y * 0.5f), headerMin,
                       ImGui::GetColorU32(WithAlpha(pal.success, 0.9f)), 1.2f, 6.0f);

    ImGui::End();
}

} // namespace demo
} // namespace uilab
