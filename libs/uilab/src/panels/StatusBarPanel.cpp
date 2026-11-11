// StatusBarPanel.cpp - thin bottom strip: session status, selection, play state and per-frame stats.
#include "panels/Panels.h"

#include <cstdio>
#include <string>

#include <imgui.h>

#include "core/AppContext.h"
#include "ui/Icons.h"
#include "ui/Widgets.h"

namespace uilab {
namespace panels {

void DrawStatusBar(AppContext& ctx) {
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse;
    if (!ImGui::Begin(kStatusBarTitle, nullptr, flags)) {
        ImGui::End();
        return;
    }

    if (ctx.host.statusBar) {
        ctx.host.statusBar(ctx);
        ImGui::End();
        return;
    }

    const ThemePalette& palette = ctx.theme.Active();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 4.0f));

    // ---- left: session status line ----------------------------------------
    // Kept short on purpose: the middle and right groups carry the rest, and the
    // whole bar has to fit on one line.
    char session[160];
    std::snprintf(session, sizeof(session), "%s v%s | %s %s", ctx.appName.c_str(), ctx.appVersion.c_str(),
                  ctx.renderer.c_str(), ctx.rendererVersion.c_str());
    widgets::TextMuted("%s", session);

    // ---- middle: page / selection / play state / theme --------------------
    ImGui::SameLine();
    widgets::Badge(CategoryShortName(ctx.activeCategory), AccentForCategory(ctx.activeCategory));
    ImGui::SameLine();
    if (const WidgetInstance* sel = ctx.SelectedWidget())
        widgets::StatusDot(palette.accent, sel->displayName.c_str());
    else
        widgets::StatusDot(palette.textDisabled, "(none)");
    ImGui::SameLine();
    const bool playing = ctx.demo.playing;
    icons::IconText(playing ? icons::Icon::Play : icons::Icon::Pause, playing ? "Playing" : "Paused",
                    ImGui::ColorConvertFloat4ToU32(playing ? palette.success : palette.warning));
    ImGui::SameLine();
    const icons::Icon themeIcon = ctx.theme.mode == ThemeMode::Light
                                      ? icons::Icon::Sun
                                      : (ctx.theme.mode == ThemeMode::Custom ? icons::Icon::Palette : icons::Icon::Moon);
    icons::IconText(themeIcon, ctx.theme.ActiveName(), ImGui::GetColorU32(ImGuiCol_Text));

    // ---- right: performance / stats ---------------------------------------
    // One measured string so the block can be right-aligned exactly; the channel
    // counts live in the console panel's tabs.
    char stats[192];
    std::snprintf(stats, sizeof(stats), "%.0f fps | %.1f ms | %d draws | %d verts | %d comp / %d live",
                  static_cast<double>(ctx.stats.currentFps), static_cast<double>(ctx.stats.currentFrameMs),
                  ctx.stats.uiDrawCalls, ctx.stats.uiVertices, static_cast<int>(ctx.registry.Count()),
                  ctx.registry.CountLive());

    const float statsWidth = ImGui::CalcTextSize(stats).x;
    // 8px clear of the content edge: TextUnformatted wraps at the exact boundary.
    const float posX = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - statsWidth - 8.0f;
    if (posX <= ImGui::GetCursorPosX() + 16.0f) {
        // Narrow window: the stats would wrap onto a second line the bar cannot show.
        ImGui::PopStyleVar();
        ImGui::End();
        return;
    }
    ImGui::SameLine(posX);

    const float fpsValue = ctx.stats.currentFps;
    const ImVec4 fpsColor = fpsValue >= 55.0f ? palette.success : (fpsValue >= 30.0f ? palette.warning : palette.error);
    ImGui::PushStyleColor(ImGuiCol_Text, fpsColor);
    ImGui::TextUnformatted(stats);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%.1f ms is %s\nClick to %s the console panel", static_cast<double>(ctx.stats.currentFrameMs),
                          ctx.stats.FrameBudgetLabel(), ctx.settings.showConsole ? "hide" : "show");
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            ctx.settings.showConsole = !ctx.settings.showConsole;
            ctx.Interaction("Status Bar", ctx.settings.showConsole ? "Console shown" : "Console hidden");
        }
    }

    ImGui::PopStyleVar();
    ImGui::End();
}

} // namespace panels
} // namespace uilab
