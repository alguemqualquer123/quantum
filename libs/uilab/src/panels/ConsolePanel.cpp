// ConsolePanel.cpp - bottom dock: Console / Events / Debug channels with
// filters, counting, auto-scroll and text export.
#include "panels/Panels.h"

#include <cstdio>
#include <string>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "core/AppContext.h"
#include "core/EventLog.h"
#include "ui/Icons.h"
#include "ui/Widgets.h"

namespace uilab {
namespace panels {

namespace {

ImVec4 LevelColor(const AppContext& ctx, LogLevel level) {
    const ThemePalette& p = ctx.theme.Active();
    switch (level) {
    case LogLevel::Trace:   return p.textDisabled;
    case LogLevel::Info:    return p.info;
    case LogLevel::Success: return p.success;
    case LogLevel::Warning: return p.warning;
    case LogLevel::Error:   return p.error;
    default:                return p.text;
    }
}

ImVec4 ChannelColor(const AppContext& ctx, LogChannel channel) {
    const ThemePalette& p = ctx.theme.Active();
    switch (channel) {
    case LogChannel::Console: return p.text;
    case LogChannel::Event:   return p.accent;
    case LogChannel::Debug:   return p.warning;
    default:                  return p.text;
    }
}

void DrawFilters(AppContext& ctx, LogChannel channel) {
    // Level toggles (coloured chips) --------------------------------------
    for (int i = 0; i < static_cast<int>(LogLevel::Count); ++i) {
        const LogLevel level = static_cast<LogLevel>(i);
        ImGui::PushID(i);
        if (widgets::ChipSelectable(LogLevelName(level), ctx.log.showLevel[i], LevelColor(ctx, level), 20.0f)) {
            ctx.log.showLevel[i] = !ctx.log.showLevel[i];
            ctx.LogDebug("Console", std::string("Level filter ") + LogLevelName(level) +
                                       (ctx.log.showLevel[i] ? " on" : " off"));
        }
        ImGui::PopID();
        ImGui::SameLine();
    }
    ImGui::NewLine();

    // Text + source filters ------------------------------------------------
    widgets::SearchField("##log_filter", ctx.log.filter, "Search messages...", 260.0f);
    ImGui::SameLine();
    const std::vector<std::string> sources = ctx.log.Sources();
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::BeginCombo("##source", ctx.log.sourceFilter.empty() ? "(all sources)" : ctx.log.sourceFilter.c_str())) {
        if (ImGui::Selectable("(all sources)", ctx.log.sourceFilter.empty()))
            ctx.log.sourceFilter.clear();
        for (const std::string& s : sources) {
            if (ImGui::Selectable(s.c_str(), ctx.log.sourceFilter == s))
                ctx.log.sourceFilter = s;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::Checkbox("auto-scroll", &ctx.log.autoScroll);
    ImGui::SameLine();
    ImGui::Checkbox("wrap", &ctx.log.wrapText);

    // Actions ---------------------------------------------------------------
    // The channel counts already live in the tab label and the status bar. The
    // buttons get their own row: the filter row is full at typical dock widths,
    // and sharing it made the right-aligned buttons land on top of the labels.
    const float pad = ImGui::GetStyle().FramePadding.x * 2.0f;
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float buttonsWidth = ImGui::CalcTextSize("Clear").x + pad + gap +
                               ImGui::CalcTextSize("Clear All").x + pad + gap +
                               ImGui::CalcTextSize("Export").x + pad;
    ImGui::NewLine();
    const float buttonsX = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - buttonsWidth;
    ImGui::SetCursorPosX(buttonsX > 0.0f ? buttonsX : 0.0f);

    if (ImGui::SmallButton("Clear")) {
        ctx.log.ClearChannel(channel);
        ctx.LogDebug("Console", std::string("Cleared ") + LogChannelName(channel) + " channel");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear All"))
        ctx.log.ClearAll();
    ImGui::SameLine();
    if (ImGui::SmallButton("Export")) {
        std::string error;
        if (ctx.log.ExportToFile(ctx.settings.logExportPath, true, error))
            ctx.Notify(ToastKind::Success, "Log exported", ctx.settings.logExportPath);
        else
            ctx.Notify(ToastKind::Error, "Export failed", error);
    }
}

void DrawChannel(AppContext& ctx, LogChannel channel) {
    ctx.log.MarkSeen(channel);
    DrawFilters(ctx, channel);
    ImGui::Separator();

    const float footerHeight = ImGui::GetFrameHeightWithSpacing() + 4.0f;
    if (ImGui::BeginChild("##log_scroll", ImVec2(0.0f, -footerHeight), ImGuiChildFlags_None)) {
        if (ctx.log.wrapText)
            ImGui::PushTextWrapPos(0.0f);

        int shown = 0;
        for (const LogEntry& e : ctx.log.Entries()) {
            if (e.channel != channel)
                continue;
            if (!ctx.log.PassesFilter(e))
                continue;
            ++shown;

            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.48f, 0.51f, 0.57f, 1.0f));
            ImGui::TextUnformatted(e.Timestamp().c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ChannelColor(ctx, e.channel));
            ImGui::Text("[%s]", e.source.c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, LevelColor(ctx, e.level));
            icons::IconText(icons::Icon::Point, e.message.c_str(), ImGui::GetColorU32(ImGuiCol_Text));
            ImGui::PopStyleColor();
        }

        if (shown == 0)
            ImGui::TextDisabled("No entries match the current filters.");
        if (ctx.log.autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
            ImGui::SetScrollHereY(1.0f);
        if (ctx.log.wrapText)
            ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();

    // Quick input line: mimics a command/log entry box.
    static std::string input;
    ImGui::SetNextItemWidth(-90.0f);
    const std::string id = std::string("##log_input_") + LogChannelName(channel);
    if (ImGui::InputText(id.c_str(), &input, ImGuiInputTextFlags_EnterReturnsTrue) && !input.empty()) {
        switch (channel) {
        case LogChannel::Console: ctx.LogConsole(LogLevel::Info, "User", input); break;
        case LogChannel::Event:   ctx.LogEvent("User", input); break;
        case LogChannel::Debug:   ctx.LogDebug("User", input); break;
        default: break;
        }
        input.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Log it")) {
        if (!input.empty()) {
            ctx.LogConsole(LogLevel::Info, "User", input);
            input.clear();
        }
    }
}

} // namespace

void DrawConsole(AppContext& ctx) {
    if (!ImGui::Begin(kConsoleTitle)) {
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("##console_tabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
        for (int i = 0; i < static_cast<int>(LogChannel::Count); ++i) {
            const LogChannel channel = static_cast<LogChannel>(i);
            const int count = ctx.log.CountChannel(channel);
            const int unseen = ctx.log.Unseen(channel);
            char label[96];
            std::snprintf(label, sizeof(label), "%s (%d)%s", LogChannelName(channel), count,
                          unseen > 0 ? " *" : "");
            if (ImGui::BeginTabItem(label)) {
                DrawChannel(ctx, channel);
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

} // namespace panels
} // namespace uilab
