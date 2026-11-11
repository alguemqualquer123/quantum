// DemoBasics.cpp - the Basic Widgets page: a live gallery where every widget is
// rendered from the WidgetState of its bound component, so an Inspector edit (or
// an edit made right here) is reflected in both the preview and the Inspector.
#include "demo/DemoHost.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <imgui.h>

#include "core/AppContext.h"
#include "core/Types.h"
#include "panels/Panels.h"
#include "ui/Icons.h"
#include "ui/Widgets.h"

namespace uilab {
namespace demo {

namespace {

ImVec4 Hovered(const ImVec4& c) { return LerpColor(c, ImVec4(1.0f, 1.0f, 1.0f, 1.0f), 0.14f); }
ImVec4 Pressed(const ImVec4& c) { return LerpColor(c, ImVec4(0.0f, 0.0f, 0.0f, 1.0f), 0.16f); }

std::vector<const char*> ItemPtrs(const std::vector<std::string>& items) {
    std::vector<const char*> out;
    out.reserve(items.size());
    for (const std::string& it : items)
        out.push_back(it.c_str());
    return out;
}

// ---- buttons --------------------------------------------------------------
void DrawButton(AppContext& ctx, WidgetInstance& w, icons::Icon icon) {
    ImGui::PushID(w.id.c_str());
    ImVec2 size(w.state.size.x > 0.0f ? w.state.size.x : 160.0f,
                 w.state.size.y > 0.0f ? w.state.size.y : 32.0f);
    const ImVec4 base = w.state.color;
    ImGui::PushStyleColor(ImGuiCol_Button, base);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Hovered(base));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Pressed(base));
    ImGui::BeginDisabled(!w.state.enabled);
    const bool clicked = (icon != icons::Icon::None)
                             ? icons::IconTextButton("##btn", icon, w.state.text.c_str(), size)
                             : ImGui::Button(w.state.text.c_str(), size);
    ImGui::EndDisabled();
    ImGui::PopStyleColor(3);
    if (!w.state.tooltip.empty())
        ImGui::SetItemTooltip("%s", w.state.tooltip.c_str());
    if (clicked) {
        ++ctx.demo.buttonClicks;
        ctx.Interaction(w.displayName, "clicked");
    }
    ImGui::PopID();
}

void DrawToggleButton(AppContext& ctx, WidgetInstance& w) {
    ImGui::PushID(w.id.c_str());
    const ImVec2 size(w.state.size.x > 0.0f ? w.state.size.x : 150.0f, 26.0f);
    ImGui::BeginDisabled(!w.state.enabled);
    if (widgets::ToggleButton("##tgl", w.state.text.c_str(), &w.state.toggled, size, w.state.tooltip.c_str())) {
        widgets::TrackEdit(ctx, w, "Toggled");
        ctx.demo.checkboxToggles++;
        ctx.Interaction(w.displayName, w.state.toggled ? "on" : "off");
    }
    ImGui::EndDisabled();
    ImGui::PopID();
}

void DrawCheckbox(AppContext& ctx, WidgetInstance& w) {
    ImGui::PushID(w.id.c_str());
    ImGui::BeginDisabled(!w.state.enabled || w.state.readOnly);
    if (ImGui::Checkbox(w.state.text.c_str(), &w.state.checked)) {
        ++ctx.demo.checkboxToggles;
        widgets::TrackEdit(ctx, w, "Checked");
        ctx.Interaction(w.displayName, w.state.checked ? "checked" : "unchecked");
    }
    ImGui::EndDisabled();
    ImGui::PopID();
}

void DrawRadioGroup(AppContext& ctx, WidgetInstance& w) {
    ImGui::PushID(w.id.c_str());
    widgets::TextMuted("%s", w.state.text.c_str());
    ImGui::BeginDisabled(!w.state.enabled || w.state.readOnly);
    const int count = static_cast<int>(w.state.items.size());
    for (int i = 0; i < count; ++i) {
        if (i != 0)
            ImGui::SameLine();
        if (ImGui::RadioButton(w.state.items[i].c_str(), w.state.selectedIndex == i)) {
            w.state.selectedIndex = i;
            widgets::TrackEdit(ctx, w, "Selection");
            ctx.Interaction(w.displayName, "selected " + w.state.items[i]);
        }
    }
    ImGui::EndDisabled();
    ImGui::PopID();
}

// ---- values ---------------------------------------------------------------
void DrawSlider(AppContext& ctx, WidgetInstance& w, const char* orientation) {
    ImGui::PushID(w.id.c_str());
    widgets::TextMuted("%s  (%s)", w.state.text.c_str(), orientation);
    const float step = w.state.step > 0.0f ? w.state.step : 0.01f;
    ImGui::BeginDisabled(!w.state.enabled || w.state.readOnly);
    ImGui::SetNextItemWidth(-160.0f);
    if (ImGui::SliderFloat("##val", &w.state.value, w.state.minValue, w.state.maxValue, w.state.format.c_str())) {
        ++ctx.demo.sliderChanges;
        ctx.Interaction(w.displayName, "value changed");
    }
    widgets::TrackEdit(ctx, w, "Value");
    ImGui::EndDisabled();
    ImGui::SameLine();
    float v = w.state.value;
    if (widgets::NumericFloatField("##num", &v, w.state.minValue, w.state.maxValue, step, 140.0f,
                                   w.state.format.c_str()) &&
        v != w.state.value) {
        w.state.value = v;
        ++ctx.demo.sliderChanges;
        widgets::TrackEdit(ctx, w, "Value");
        ctx.Interaction(w.displayName, "value nudged");
    }
    ImGui::PopID();
}

void DrawProgressBar(AppContext& ctx, WidgetInstance& w) {
    ImGui::PushID(w.id.c_str());
    widgets::TextMuted("%s", w.state.text.c_str());
    const float span = w.state.maxValue - w.state.minValue;
    float frac = span > 1e-6f ? (w.state.value - w.state.minValue) / span : w.state.value;
    frac = std::clamp(frac, 0.0f, 1.0f);
    const bool pulse = PageClock(ctx, 0.5f) < 0.5f;
    char overlay[32];
    std::snprintf(overlay, sizeof(overlay), "%d%%", static_cast<int>(frac * 100.0f + 0.5f));
    widgets::ProgressBarEx("##pb", frac, ImVec2(0.0f, 20.0f), overlay, w.state.color, pulse);
    ImGui::PopID();
}

// ---- inputs ---------------------------------------------------------------
void DrawTextInput(AppContext& ctx, WidgetInstance& w) {
    ImGui::PushID(w.id.c_str());
    widgets::TextMuted("%s", w.state.text.c_str());
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s", w.state.text.c_str());
    ImGuiInputTextFlags flags = w.state.readOnly ? ImGuiInputTextFlags_ReadOnly : 0;
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##txt", buf, sizeof(buf), flags))
        w.state.text = buf;
    widgets::TrackEdit(ctx, w, "Text");
    if (!w.state.hint.empty())
        widgets::TextMuted("hint: %s   rule: %s", w.state.hint.c_str(), w.state.validationRule.c_str());
    ImGui::PopID();
}

void DrawMultilineInput(AppContext& ctx, WidgetInstance& w) {
    ImGui::PushID(w.id.c_str());
    widgets::TextMuted("%s", w.state.text.c_str());
    std::vector<char> buf(w.state.multilineText.begin(), w.state.multilineText.end());
    buf.push_back('\0');
    ImGuiInputTextFlags flags = w.state.readOnly ? ImGuiInputTextFlags_ReadOnly : 0;
    if (ImGui::InputTextMultiline("##ml", buf.data(), buf.size(), ImVec2(-1.0f, 100.0f), flags))
        w.state.multilineText.assign(buf.data());
    widgets::TrackEdit(ctx, w, "Multiline text");
    const int lines = static_cast<int>(std::count(w.state.multilineText.begin(), w.state.multilineText.end(), '\n')) + 1;
    widgets::TextMuted("%d chars, %d lines", static_cast<int>(w.state.multilineText.size()), lines);
    ImGui::PopID();
}

void DrawCombo(AppContext& ctx, WidgetInstance& w) {
    ImGui::PushID(w.id.c_str());
    widgets::TextMuted("%s", w.state.text.c_str());
    const std::vector<const char*> ptrs = ItemPtrs(w.state.items);
    int idx = w.state.selectedIndex;
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::BeginDisabled(!w.state.enabled || w.state.readOnly);
    if (!ptrs.empty() && ImGui::Combo("##combo", &idx, ptrs.data(), static_cast<int>(ptrs.size()))) {
        w.state.selectedIndex = idx;
        ctx.Interaction(w.displayName, "combo " + w.state.items[idx]);
    }
    widgets::TrackEdit(ctx, w, "Selection");
    ImGui::EndDisabled();
    ImGui::PopID();
}

void DrawNumericInput(AppContext& ctx, WidgetInstance& w) {
    ImGui::PushID(w.id.c_str());
    widgets::TextMuted("%s", w.state.text.c_str());
    const int step = w.state.step > 0.0f ? static_cast<int>(w.state.step) : 1;
    ImGui::BeginDisabled(!w.state.enabled || w.state.readOnly);
    if (widgets::NumericIntField("##num", &w.state.intValue, w.state.intMin, w.state.intMax, step, 160.0f)) {
        widgets::TrackEdit(ctx, w, "Integer value");
        ctx.Interaction(w.displayName, "value = " + std::to_string(w.state.intValue));
    }
    ImGui::EndDisabled();
    ImGui::PopID();
}

void DrawSearchField(AppContext& ctx, WidgetInstance& w) {
    ImGui::PushID(w.id.c_str());
    ImGui::BeginDisabled(!w.state.enabled || w.state.readOnly);
    if (widgets::SearchField("##search", w.state.text, w.state.hint.c_str(), -1.0f))
        widgets::TrackEdit(ctx, w, "Search");
    ImGui::EndDisabled();
    int hits = 0;
    for (const std::string& it : w.state.items) {
        if (ContainsNoCase(it, w.state.text)) {
            ImGui::BulletText("%s", it.c_str());
            ++hits;
        }
    }
    widgets::TextMuted("%d hits of %d", hits, static_cast<int>(w.state.items.size()));
    ImGui::PopID();
}

// ---- collections ----------------------------------------------------------
void DrawListBox(AppContext& ctx, WidgetInstance& w) {
    ImGui::PushID(w.id.c_str());
    widgets::TextMuted("%s", w.state.text.c_str());
    const std::vector<const char*> ptrs = ItemPtrs(w.state.items);
    int idx = w.state.selectedIndex;
    ImGui::BeginDisabled(!w.state.enabled || w.state.readOnly);
    if (!ptrs.empty() && ImGui::ListBox("##list", &idx, ptrs.data(), static_cast<int>(ptrs.size()), 5)) {
        w.state.selectedIndex = idx;
        ctx.Interaction(w.displayName, "list " + w.state.items[idx]);
    }
    widgets::TrackEdit(ctx, w, "Selection");
    ImGui::EndDisabled();
    ImGui::PopID();
}

void DrawTreeView(AppContext& ctx, WidgetInstance& w) {
    ImGui::PushID(w.id.c_str());
    if (ImGui::TreeNode(w.state.text.c_str())) {
        const int count = static_cast<int>(w.state.items.size());
        for (int i = 0; i < count; ++i) {
            ImGui::PushID(i);
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
            if (w.state.selectedIndex == i)
                flags |= ImGuiTreeNodeFlags_Selected;
            const bool node = ImGui::TreeNodeEx(w.state.items[i].c_str(), flags);
            if (node && w.state.selectedIndex != i) {
                w.state.selectedIndex = i;
                widgets::TrackEdit(ctx, w, "Selection");
                ctx.Interaction(w.displayName, "tree " + w.state.items[i]);
            }
            (void)node;
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void DrawTabs(AppContext& ctx, WidgetInstance& w) {
    ImGui::PushID(w.id.c_str());
    // s_lastSelected lets an Inspector change re-sync the open tab exactly once,
    // without stealing the click when the user picks a different tab here.
    static int s_lastSelected = -1;
    const bool sync = (s_lastSelected != w.state.selectedIndex);
    if (ImGui::BeginTabBar("##tabs")) {
        const int count = static_cast<int>(w.state.items.size());
        int shown = -1;
        for (int i = 0; i < count; ++i) {
            ImGuiTabItemFlags flags = (sync && i == w.state.selectedIndex) ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(w.state.items[i].c_str(), nullptr, flags)) {
                shown = i;
                ImGui::TextUnformatted(w.state.items[i].c_str());
                ImGui::EndTabItem();
            }
        }
        if (shown >= 0 && shown != w.state.selectedIndex) {
            w.state.selectedIndex = shown;
            ctx.Interaction(w.displayName, "tab " + w.state.items[shown]);
        }
        s_lastSelected = w.state.selectedIndex;
        ImGui::EndTabBar();
    }
    ImGui::PopID();
}

// ---- bars -----------------------------------------------------------------
void DrawSeparator(AppContext& ctx, WidgetInstance& w) {
    (void)ctx;
    ImGui::SeparatorText(w.state.text.c_str());
}

void DrawMenuBarSample(AppContext& ctx, WidgetInstance& w) {
    ImGui::PushID(w.id.c_str());
    static std::string lastAction;
    const float availW = ImGui::GetContentRegionAvail().x;
    ImGui::BeginChild("##mb", ImVec2(availW, ImGui::GetFrameHeight() + 34.0f), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_MenuBar);
    if (ImGui::BeginMenuBar()) {
        for (const std::string& menu : w.state.items) {
            if (ImGui::BeginMenu(menu.c_str())) {
                if (ImGui::MenuItem(std::string("Do - " + menu).c_str())) {
                    lastAction = menu + " / Do";
                    ctx.Interaction(w.displayName, "menu " + menu);
                }
                if (ImGui::MenuItem(std::string("Settings... ").c_str())) {
                    lastAction = menu + " / Settings";
                    ctx.Interaction(w.displayName, "menu " + menu);
                }
                ImGui::EndMenu();
            }
        }
        ImGui::EndMenuBar();
    }
    widgets::TextMuted("last action: %s", lastAction.empty() ? "(none)" : lastAction.c_str());
    ImGui::EndChild();
    ImGui::PopID();
}

icons::Icon ToolbarIconFor(const std::string& name) {
    std::string n = ToLower(name);
    if (n.find("new") != std::string::npos)
        return icons::Icon::New;
    if (n.find("open") != std::string::npos)
        return icons::Icon::Open;
    if (n.find("save") != std::string::npos)
        return icons::Icon::Save;
    if (n.find("play") != std::string::npos)
        return icons::Icon::Play;
    if (n.find("pause") != std::string::npos)
        return icons::Icon::Pause;
    if (n.find("stop") != std::string::npos)
        return icons::Icon::Stop;
    return icons::Icon::Widget;
}

void DrawToolbar(AppContext& ctx, WidgetInstance& w) {
    ImGui::PushID(w.id.c_str());
    const float spacing = std::max(0.0f, w.state.spacingX);
    const float bh = 28.0f;
    const float iconSize = ImGui::GetFontSize() * 0.95f;
    for (size_t i = 0; i < w.state.items.size(); ++i) {
        if (i != 0)
            ImGui::SameLine(0.0f, spacing);
        const std::string& item = w.state.items[i];
        const float textW = ImGui::CalcTextSize(item.c_str()).x;
        const float bw = iconSize + 6.0f + textW + ImGui::GetStyle().FramePadding.x * 2.0f;
        if (icons::IconTextButton(item.c_str(), ToolbarIconFor(item), item.c_str(), ImVec2(bw, bh))) {
            ++ctx.demo.buttonClicks;
            ctx.Interaction(w.displayName, "toolbar " + item);
        }
    }
    ImGui::PopID();
}

// ---- feedback -------------------------------------------------------------
void DrawTooltipButton(AppContext& ctx, WidgetInstance& w) {
    ImGui::PushID(w.id.c_str());
    ImGui::BeginDisabled(!w.state.enabled);
    ImGui::Button(w.state.text.c_str(), ImVec2(0.0f, 28.0f));
    ImGui::EndDisabled();
    std::string tip = w.state.tooltip;
    if (!w.state.hint.empty())
        tip += "   (" + w.state.hint + ")";
    ImGui::SetItemTooltip("%s", tip.c_str());
    ImGui::PopID();
    (void)ctx;
}

void DrawContextMenu(AppContext& ctx, WidgetInstance& w) {
    ImGui::PushID(w.id.c_str());
    if (widgets::SelectableRow(w.state.text.c_str(), false, icons::Icon::Filter,
                               panels::AccentForCategory(Category::Basic))) {
        ctx.Interaction(w.displayName, "selected row");
    }
    ImGui::OpenPopupOnItemClick("##ctx", ImGuiPopupFlags_MouseButtonRight);
    if (ImGui::BeginPopup("##ctx")) {
        for (const std::string& item : w.state.items) {
            if (ImGui::MenuItem(item.c_str()))
                ctx.Interaction(w.displayName, "menu " + item);
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

void DrawScrollView(AppContext& ctx, WidgetInstance& w) {
    ImGui::PushID(w.id.c_str());
    const int rows = std::max(1, std::min(200, w.state.intValue));
    const float availW = ImGui::GetContentRegionAvail().x;
    if (ImGui::BeginChild("##sv", ImVec2(availW, 150.0f), ImGuiChildFlags_Borders)) {
        for (int i = 0; i < rows; ++i)
            ImGui::TextDisabled("%s %d: activity recorded", w.state.text.c_str(), i);
    }
    ImGui::EndChild();
    widgets::TextMuted("%d rows (state.intValue)", rows);
    ImGui::PopID();
    (void)ctx;
}

void DrawImageView(AppContext& ctx, WidgetInstance& w) {
    ImGui::PushID(w.id.c_str());
    const ImVec2 size(w.state.size.x > 0.0f ? w.state.size.x : 280.0f,
                      w.state.size.y > 0.0f ? w.state.size.y : 160.0f);
    if (ctx.textures && ctx.textures->Checker().Valid()) {
        ImGui::Image(ctx.textures->Checker().Ref(), size);
    } else {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        widgets::DrawCheckerboard(ImGui::GetWindowDrawList(), pos, ImVec2(pos.x + size.x, pos.y + size.y), 16.0f,
                                  IM_COL32(60, 66, 78, 255), IM_COL32(92, 100, 116, 255));
        ImGui::Dummy(size);
    }
    widgets::TextMuted("%s", w.state.text.c_str());
    ImGui::PopID();
}

void DrawColorPicker(AppContext& ctx, WidgetInstance& w) {
    ImGui::PushID(w.id.c_str());
    widgets::TextMuted("%s", w.state.text.c_str());
    if (ImGui::ColorEdit4("##c1", &w.state.color.x)) {
        widgets::TrackEdit(ctx, w, "Primary colour");
        ctx.Interaction(w.displayName, "colour changed");
    }
    ImGui::SameLine();
    if (ImGui::ColorEdit4("##c2", &w.state.color2.x))
        widgets::TrackEdit(ctx, w, "Secondary colour");
    ImGui::PopID();
}

void DrawModalDialogTrigger(AppContext& ctx, WidgetInstance& w) {
    ImGui::PushID(w.id.c_str());
    const bool enabled = w.state.enabled;
    const std::string name = w.displayName;
    const std::string title = w.state.text;
    const std::string body = w.state.hint;
    ImGui::BeginDisabled(!w.state.enabled);
    const bool pressed = ImGui::Button(w.state.text.c_str(), ImVec2(0.0f, 30.0f));
    ImGui::EndDisabled();
    if (enabled && pressed) {
        ctx.dialogs.Confirm(title, body, true, [&ctx, name](bool confirmed) {
            if (confirmed) {
                ctx.LogEvent(name, "modal confirmed (destructive action accepted)");
                ctx.Notify(ToastKind::Warning, "Action confirmed", name);
            } else {
                ctx.LogEvent(name, "modal cancelled");
            }
        });
        ctx.Interaction(name, "opened modal dialog");
    }
    ImGui::PopID();
}

void DrawToastTrigger(AppContext& ctx, WidgetInstance& w) {
    ImGui::PushID(w.id.c_str());
    const int kindCount = static_cast<int>(ToastKind::Count);
    int idx = std::clamp(w.state.selectedIndex, 0, kindCount - 1);
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::Combo("##kind", &idx, "Info\0Success\0Warning\0Error\0")) {
        w.state.selectedIndex = idx;
        widgets::TrackEdit(ctx, w, "Selection");
    }
    const std::string name = w.displayName;
    const std::string title = w.state.text;
    const std::string body = w.state.hint;
    if (ImGui::Button("Fire Toast", ImVec2(0.0f, 28.0f))) {
        const ToastKind kind = static_cast<ToastKind>(idx);
        ctx.Notify(kind, title, body);
        ctx.Interaction(name, std::string("toast: ") + ToastKindName(kind));
    }
    ImGui::PopID();
}

} // namespace

// ---------------------------------------------------------------------------
// Page
// ---------------------------------------------------------------------------
void DrawBasicPage(AppContext& ctx) {
    if (widgets::SectionHeader("Buttons", icons::Icon::Point, true)) {
        if (WidgetInstance* w = Bind(ctx, "basic.button.primary"); w && w->state.visible)
            DrawButton(ctx, *w, icons::Icon::None);
        if (WidgetInstance* w = Bind(ctx, "basic.button.secondary"); w && w->state.visible)
            DrawButton(ctx, *w, icons::Icon::None);
        if (WidgetInstance* w = Bind(ctx, "basic.button.icon"); w && w->state.visible)
            DrawButton(ctx, *w, icons::Icon::Save);
        if (WidgetInstance* w = Bind(ctx, "basic.button.disabled"); w && w->state.visible)
            DrawButton(ctx, *w, icons::Icon::None);
    }

    if (widgets::SectionHeader("Toggles", icons::Icon::Check, true)) {
        if (WidgetInstance* w = Bind(ctx, "basic.toggle.play"); w && w->state.visible)
            DrawToggleButton(ctx, *w);
        if (WidgetInstance* w = Bind(ctx, "basic.checkbox.grid"); w && w->state.visible)
            DrawCheckbox(ctx, *w);
        if (WidgetInstance* w = Bind(ctx, "basic.checkbox.snap"); w && w->state.visible)
            DrawCheckbox(ctx, *w);
        if (WidgetInstance* w = Bind(ctx, "basic.radio.quality"); w && w->state.visible)
            DrawRadioGroup(ctx, *w);
    }

    if (widgets::SectionHeader("Values", icons::Icon::Sliders, true)) {
        if (WidgetInstance* w = Bind(ctx, "basic.slider.horizontal"); w && w->state.visible)
            DrawSlider(ctx, *w, "horizontal");
        if (WidgetInstance* w = Bind(ctx, "basic.slider.vertical"); w && w->state.visible)
            DrawSlider(ctx, *w, "vertical");
        if (WidgetInstance* w = Bind(ctx, "basic.progress.compile"); w && w->state.visible)
            DrawProgressBar(ctx, *w);
    }

    if (widgets::SectionHeader("Inputs", icons::Icon::File, true)) {
        if (WidgetInstance* w = Bind(ctx, "basic.input.name"); w && w->state.visible)
            DrawTextInput(ctx, *w);
        if (WidgetInstance* w = Bind(ctx, "basic.input.multiline"); w && w->state.visible)
            DrawMultilineInput(ctx, *w);
        if (WidgetInstance* w = Bind(ctx, "basic.combo.quality"); w && w->state.visible)
            DrawCombo(ctx, *w);
        if (WidgetInstance* w = Bind(ctx, "basic.numeric.amount"); w && w->state.visible)
            DrawNumericInput(ctx, *w);
        if (WidgetInstance* w = Bind(ctx, "basic.search.filter"); w && w->state.visible)
            DrawSearchField(ctx, *w);
    }

    if (widgets::SectionHeader("Collections", icons::Icon::List, true)) {
        if (WidgetInstance* w = Bind(ctx, "basic.list.levels"); w && w->state.visible)
            DrawListBox(ctx, *w);
        if (WidgetInstance* w = Bind(ctx, "basic.tree.content"); w && w->state.visible)
            DrawTreeView(ctx, *w);
        if (WidgetInstance* w = Bind(ctx, "basic.tabs.inspector"); w && w->state.visible)
            DrawTabs(ctx, *w);
        if (WidgetInstance* w = Bind(ctx, "basic.scroll.panel"); w && w->state.visible)
            DrawScrollView(ctx, *w);
    }

    if (widgets::SectionHeader("Bars", icons::Icon::Gear, true)) {
        if (WidgetInstance* w = Bind(ctx, "basic.menubar.doc"); w && w->state.visible)
            DrawMenuBarSample(ctx, *w);
        if (WidgetInstance* w = Bind(ctx, "basic.toolbar.commands"); w && w->state.visible)
            DrawToolbar(ctx, *w);
        if (WidgetInstance* w = Bind(ctx, "basic.separator.line"); w && w->state.visible)
            DrawSeparator(ctx, *w);
    }

    if (widgets::SectionHeader("Media", icons::Icon::Image, true)) {
        if (WidgetInstance* w = Bind(ctx, "basic.image.view"); w && w->state.visible)
            DrawImageView(ctx, *w);
        if (WidgetInstance* w = Bind(ctx, "basic.color.picker"); w && w->state.visible)
            DrawColorPicker(ctx, *w);
    }

    if (widgets::SectionHeader("Feedback", icons::Icon::Info, true)) {
        if (WidgetInstance* w = Bind(ctx, "basic.tooltip.save"); w && w->state.visible)
            DrawTooltipButton(ctx, *w);
        if (WidgetInstance* w = Bind(ctx, "basic.contextmenu.row"); w && w->state.visible)
            DrawContextMenu(ctx, *w);
        if (WidgetInstance* w = Bind(ctx, "basic.dialog.trigger"); w && w->state.visible)
            DrawModalDialogTrigger(ctx, *w);
        if (WidgetInstance* w = Bind(ctx, "basic.toast.trigger"); w && w->state.visible)
            DrawToastTrigger(ctx, *w);
    }

    DrawUnboundList(ctx, Category::Basic);
}

} // namespace demo
} // namespace uilab
