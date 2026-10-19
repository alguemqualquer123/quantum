// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#include "NotificationsWindow.h"
#include "external/IconsFontAwesome6.h"
#include "App.h"
#include "Backend.h"

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "imgui.h"

using namespace doriax::editor;

static ImVec4 GetLevelColor(NotificationLevel level) {
    switch (level) {
        case NotificationLevel::Success: return ImVec4(0.40f, 0.80f, 0.45f, 1.00f);
        case NotificationLevel::Warning: return ImVec4(0.95f, 0.75f, 0.35f, 1.00f);
        case NotificationLevel::Error:   return ImVec4(0.95f, 0.45f, 0.40f, 1.00f);
        case NotificationLevel::Info:
        default:                         return ImVec4(0.55f, 0.75f, 0.95f, 1.00f);
    }
}

static const char* GetLevelIcon(NotificationLevel level) {
    switch (level) {
        case NotificationLevel::Success: return ICON_FA_CIRCLE_CHECK;
        case NotificationLevel::Warning: return ICON_FA_TRIANGLE_EXCLAMATION;
        case NotificationLevel::Error:   return ICON_FA_CIRCLE_XMARK;
        case NotificationLevel::Info:
        default:                         return ICON_FA_CIRCLE_INFO;
    }
}

static std::string FormatTimestamp() {
    std::time_t now = std::time(nullptr);
    std::tm tmBuf{};
#ifdef _WIN32
    localtime_s(&tmBuf, &now);
#else
    localtime_r(&now, &tmBuf);
#endif
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", tmBuf.tm_hour, tmBuf.tm_min, tmBuf.tm_sec);
    return buffer;
}

NotificationsWindow::NotificationsWindow() {
    windowOpen = true;
    focusRequested = false;
    hasNotification = false;
}

void NotificationsWindow::push(NotificationLevel level, const std::string& title, const std::string& message) {
    NotificationEntry entry;
    entry.level = level;
    entry.title = title;
    entry.message = message;
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        entry.timestamp = FormatTimestamp();
        history.push_back(entry);
        if (history.size() > MAX_HISTORY) {
            history.erase(history.begin(), history.begin() + (history.size() - MAX_HISTORY));
        }
        incoming.push_back(entry);
        hasNotification = true;
    }
    Backend::getApp().requestRedraw();
}

void NotificationsWindow::drainIncoming() {
    std::vector<NotificationEntry> fresh;
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        fresh.swap(incoming);
    }
    for (auto& entry : fresh) {
        Toast toast;
        toast.entry = std::move(entry);
        toast.expireTime = ImGui::GetTime() + TOAST_LIFETIME;
        toasts.push_back(std::move(toast));
    }
    if (toasts.size() > MAX_TOASTS) {
        toasts.erase(toasts.begin(), toasts.begin() + (toasts.size() - MAX_TOASTS));
    }
}

void NotificationsWindow::clearHistory() {
    std::lock_guard<std::mutex> lock(dataMutex);
    history.clear();
}

void NotificationsWindow::setOpen(bool open) {
    if (open) {
        if (!windowOpen) {
            focusRequested = true;
        }
        windowOpen = true;
        return;
    }

    windowOpen = false;
    focusRequested = false;
}

bool NotificationsWindow::isOpen() const {
    return windowOpen;
}

void NotificationsWindow::show() {
    drainIncoming();

    const float now = ImGui::GetTime();
    toasts.erase(std::remove_if(toasts.begin(), toasts.end(),
        [now](const Toast& t) { return t.expireTime <= now; }), toasts.end());

    drawHistoryWindow();
    drawToasts();
}

void NotificationsWindow::drawHistoryWindow() {
    if (!windowOpen) {
        return;
    }

    if (focusRequested) {
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }

    bool wasOpen = windowOpen;
    if (hasNotification) {
        App::pushTabNotificationStyle();
    }
    if (!ImGui::Begin(WINDOW_NAME, &windowOpen, hasNotification ? ImGuiWindowFlags_UnsavedDocument : 0)) {
        if (hasNotification) {
            App::popTabNotificationStyle();
        }
        ImGui::End();
        if (wasOpen && !windowOpen) {
            setOpen(false);
        }
        return;
    }
    if (hasNotification) {
        App::popTabNotificationStyle();
    }

    hasNotification = false;

    std::deque<NotificationEntry> snapshot;
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        snapshot = history;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 4.0f));

    const float bellWidth = ImGui::CalcTextSize(ICON_FA_BELL).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + bellWidth * 0.25f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::Text(ICON_FA_BELL);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextUnformatted(WINDOW_NAME);
    const float clearWidth = ImGui::CalcTextSize(ICON_FA_TRASH).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - clearWidth);
    if (ImGui::SmallButton(ICON_FA_TRASH "##clear_notifications")) {
        clearHistory();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Clear history");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::BeginChild("##notification_list", ImVec2(0, 0), false);

    if (snapshot.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextDisabled("No notifications yet");
        ImGui::PopStyleColor();
    }

    const float wrapWidth = 320.0f;
    size_t index = snapshot.size();
    for (auto it = snapshot.rbegin(); it != snapshot.rend(); ++it, --index) {
        const NotificationEntry& entry = *it;
        ImGui::PushID(static_cast<int>(index));

        ImGui::PushStyleColor(ImGuiCol_Text, GetLevelColor(entry.level));
        ImGui::TextUnformatted(GetLevelIcon(entry.level));
        ImGui::PopStyleColor();
        ImGui::SameLine();

        const ImVec2 timeSize = ImGui::CalcTextSize(entry.timestamp.c_str());
        ImGui::TextUnformatted(entry.title.c_str());
        if (!entry.title.empty()) {
            ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - timeSize.x);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextUnformatted(entry.timestamp.c_str());
            ImGui::PopStyleColor();
        }

        if (!entry.message.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::PushTextWrapPos(ImGui::GetCursorScreenPos().x + wrapWidth);
            ImGui::TextWrapped("%s", entry.message.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }

        ImGui::PopID();
        ImGui::EndGroup();
        ImGui::Spacing();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::End();
}

void NotificationsWindow::drawToasts() {
    if (toasts.empty()) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float margin = 16.0f;
    const float maxWidth = 320.0f;

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoBringToFrontOnFocus |
                             ImGuiWindowFlags_NoInputs |
                             ImGuiWindowFlags_AlwaysAutoResize;

    const ImVec2 anchor(viewport->WorkPos.x + viewport->WorkSize.x - margin,
                        viewport->WorkPos.y + viewport->WorkSize.y - margin);
    ImGui::SetNextWindowPos(anchor, ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.95f);

    if (!ImGui::Begin("##notifications_toasts", nullptr, flags)) {
        ImGui::End();
        return;
    }

    const float stripWidth = 4.0f;
    std::vector<std::pair<float, float>> bands;
    bands.reserve(toasts.size());

    for (size_t i = 0; i < toasts.size(); ++i) {
        const Toast& toast = toasts[i];
        const float bandTop = ImGui::GetCursorScreenPos().y;

        ImGui::PushStyleColor(ImGuiCol_Text, GetLevelColor(toast.entry.level));
        ImGui::TextUnformatted(GetLevelIcon(toast.entry.level));
        ImGui::PopStyleColor();
        ImGui::SameLine();

        const float textLeft = ImGui::GetCursorScreenPos().x;
        ImGui::PushTextWrapPos(textLeft + maxWidth);
        ImGui::TextUnformatted(toast.entry.title.c_str());
        if (!toast.entry.message.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", toast.entry.message.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::PopTextWrapPos();

        bands.push_back({bandTop, ImGui::GetCursorScreenPos().y});

        if (i + 1 < toasts.size()) {
            const ImVec4 border = ImGui::GetStyleColorVec4(ImGuiCol_Border);
            ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(border.x, border.y, border.z, border.w * 0.5f));
            ImGui::Separator();
            ImGui::PopStyleColor();
        }
    }

    const ImVec2 windowPos = ImGui::GetWindowPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImU32 rounding = static_cast<ImU32>(style.WindowRounding);
    for (size_t i = 0; i < bands.size(); ++i) {
        const ImVec4 color = GetLevelColor(toasts[i].entry.level);
        drawList->AddRectFilled(ImVec2(windowPos.x, bands[i].first),
                                ImVec2(windowPos.x + stripWidth, bands[i].second),
                                ImGui::GetColorU32(color), style.WindowRounding,
                                ImDrawFlags_RoundCornersLeft);
    }

    ImGui::End();
}
