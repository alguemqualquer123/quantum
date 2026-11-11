#include "core/Notifications.h"

#include "core/Types.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>

namespace uilab {

const char* ToastKindName(ToastKind k) {
    switch (k) {
    case ToastKind::Info:    return "Info";
    case ToastKind::Success: return "Success";
    case ToastKind::Warning: return "Warning";
    case ToastKind::Error:   return "Error";
    default:                 return "?";
    }
}

namespace {
ImVec4 AccentFor(ToastKind kind) {
    switch (kind) {
    case ToastKind::Success: return ImVec4(0.30f, 0.78f, 0.42f, 1.00f);
    case ToastKind::Warning: return ImVec4(0.95f, 0.72f, 0.20f, 1.00f);
    case ToastKind::Error:   return ImVec4(0.92f, 0.36f, 0.32f, 1.00f);
    case ToastKind::Info:
    default:                 return ImVec4(0.31f, 0.63f, 0.98f, 1.00f);
    }
}
const char* GlyphFor(ToastKind kind) {
    switch (kind) {
    case ToastKind::Success: return "[OK]";
    case ToastKind::Warning: return "[!]";
    case ToastKind::Error:   return "[x]";
    case ToastKind::Info:
    default:                 return "[i]";
    }
}
} // namespace

void NotificationCenter::Push(ToastKind kind, std::string title, std::string body, float ttl) {
    Toast t;
    t.id = nextId_++;
    t.kind = kind;
    t.title = std::move(title);
    t.body = std::move(body);
    t.ttl = ttl;
    items_.push_back(std::move(t));
    if (static_cast<int>(items_.size()) > 32)
        items_.erase(items_.begin());
}

void NotificationCenter::Dismiss(uint64_t id) {
    for (auto it = items_.begin(); it != items_.end(); ++it) {
        if (it->id == id) {
            items_.erase(it);
            return;
        }
    }
}

void NotificationCenter::Clear() { items_.clear(); }

void NotificationCenter::Update(float dt) {
    if (pauseOnHover && hovered_)
        return;
    for (Toast& t : items_)
        t.age += dt;
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [](const Toast& t) { return t.age >= t.ttl; }),
                 items_.end());
}

size_t NotificationCenter::CountKind(ToastKind kind) const {
    size_t n = 0;
    for (const Toast& t : items_)
        if (t.kind == kind)
            ++n;
    return n;
}

int NotificationCenter::Draw(float dt) {
    (void)dt;
    if (items_.empty()) {
        hovered_ = false;
        return 0;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float margin = 14.0f;
    const float alphaFade = 0.45f;

    int dismissed = 0;
    int drawn = 0;
    float y = vp->WorkPos.y + vp->WorkSize.y - margin;

    const int first = std::max(0, static_cast<int>(items_.size()) - maxVisible);
    // Newest on the bottom of the stack.
    for (int i = static_cast<int>(items_.size()) - 1; i >= first; --i) {
        Toast& t = items_[i];
        const float remain = t.ttl - t.age;
        const float fade = remain < alphaFade ? std::max(0.0f, remain / alphaFade) : 1.0f;
        const float slide = remain < alphaFade ? (1.0f - fade) * 24.0f : 0.0f;

        const std::string win = "##toast_" + std::to_string(t.id);
        const ImVec4 accent = AccentFor(t.kind);

        ImGui::SetNextWindowBgAlpha(0.94f * fade);
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - margin + slide, y),
                                ImGuiCond_Always, ImVec2(1.0f, 1.0f));
        ImGui::SetNextWindowSize(ImVec2(maxWidth, 0.0f), ImGuiCond_Always);

        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking |
                                       ImGuiWindowFlags_AlwaysAutoResize;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
        ImGui::PushStyleColor(ImGuiCol_Border, accent);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);

        if (ImGui::Begin(win.c_str(), nullptr, flags)) {
            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
                hovered_ = true;

            ImGui::PushStyleColor(ImGuiCol_Text, accent);
            ImGui::TextUnformatted(GlyphFor(t.kind));
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextUnformatted(t.title.c_str());
            if (!t.body.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped("%s", t.body.c_str());
                ImGui::PopStyleColor();
            }

            // progress bar showing remaining lifetime
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, accent);
            ImGui::ProgressBar(std::max(0.0f, remain / t.ttl), ImVec2(-1.0f, 3.0f), "");
            ImGui::PopStyleColor();

            if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                Dismiss(t.id);
                ++dismissed;
            }
            y -= ImGui::GetWindowSize().y + 8.0f;
            ++drawn;
        }
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor();
    }

    if (drawn == 0)
        hovered_ = false;
    (void)ImGui::GetCurrentContext();
    return dismissed;
}

} // namespace uilab
