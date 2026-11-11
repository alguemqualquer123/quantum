#include "ui/Widgets.h"

#include "core/AppContext.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <unordered_map>

namespace uilab {
namespace widgets {

// ---------------------------------------------------------------------------
// Inputs
// ---------------------------------------------------------------------------
bool SearchField(const char* id, std::string& text, const char* hint, float width, bool* clearedOut) {
    ImGui::PushID(id);
    const float h = ImGui::GetFrameHeight();
    bool changed = false;
    bool cleared = false;

    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::SetNextItemWidth(width);
    // Reserve room for the icon on the left and the clear button on the right.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(h * 0.9f, ImGui::GetStyle().FramePadding.y));
    changed = ImGui::InputTextWithHint("##search", hint ? hint : "Search...", &text);
    ImGui::PopStyleVar();

    const ImVec2 min = pos;
    const ImVec2 max = ImVec2(pos.x + width, pos.y + h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    icons::Draw(dl, icons::Icon::Search, ImVec2(min.x + h * 0.42f, (min.y + max.y) * 0.5f), h * 0.62f,
                ImGui::GetColorU32(ImGuiCol_TextDisabled), 1.4f);

    if (!text.empty()) {
        const bool hoveredClear = ImGui::IsMouseHoveringRect(ImVec2(max.x - h * 0.85f, min.y),
                                                             ImVec2(max.x, max.y));
        if (hoveredClear && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            text.clear();
            changed = true;
            cleared = true;
        }
        icons::Draw(dl, icons::Icon::Close, ImVec2(max.x - h * 0.42f, (min.y + max.y) * 0.5f), h * 0.42f,
                    hoveredClear ? ImGui::GetColorU32(ImGuiCol_Text) : ImGui::GetColorU32(ImGuiCol_TextDisabled),
                    1.4f);
    }

    ImGui::PopID();
    if (clearedOut)
        *clearedOut = cleared;
    return changed;
}

bool NumericIntField(const char* id, int* value, int minValue, int maxValue, int step, float width) {
    ImGui::PushID(id);
    bool changed = false;
    const float h = ImGui::GetFrameHeight();
    const float btn = h;
    bool minus = false;
    bool plus = false;

    if (ImGui::Button("-", ImVec2(btn, h)))
        minus = true;
    ImGui::SameLine(0.0f, 4.0f);
    const float fieldW = ImMax(40.0f, width - (btn + 4.0f) * 2.0f);
    ImGui::SetNextItemWidth(fieldW);
    if (ImGui::DragInt("##value", value, static_cast<float>(step), minValue, maxValue, "%d",
                       ImGuiSliderFlags_AlwaysClamp))
        changed = true;
    ImGui::SameLine(0.0f, 4.0f);
    if (ImGui::Button("+", ImVec2(btn, h)))
        plus = true;

    if (minus) {
        *value = ImMax(minValue, *value - step);
        changed = true;
    }
    if (plus) {
        *value = ImMin(maxValue, *value + step);
        changed = true;
    }
    *value = ImClamp(*value, minValue, maxValue);
    ImGui::PopID();
    return changed;
}

bool NumericFloatField(const char* id, float* value, float minValue, float maxValue, float step, float width,
                       const char* format) {
    ImGui::PushID(id);
    bool changed = false;
    const float h = ImGui::GetFrameHeight();
    const float btn = h;

    if (ImGui::Button("-##dec", ImVec2(btn, h))) {
        *value = ImMax(minValue, *value - step);
        changed = true;
    }
    ImGui::SameLine(0.0f, 4.0f);
    const float fieldW = ImMax(48.0f, width - (btn + 4.0f) * 2.0f);
    ImGui::SetNextItemWidth(fieldW);
    if (ImGui::DragFloat("##value", value, step, minValue, maxValue, format, ImGuiSliderFlags_AlwaysClamp))
        changed = true;
    ImGui::SameLine(0.0f, 4.0f);
    if (ImGui::Button("+##inc", ImVec2(btn, h))) {
        *value = ImMin(maxValue, *value + step);
        changed = true;
    }
    *value = ImClamp(*value, minValue, maxValue);
    ImGui::PopID();
    return changed;
}

bool ToggleButton(const char* id, const char* label, bool* value, ImVec2 size, const char* tooltip) {
    ImGui::PushID(id);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    if (size.x <= 0.0f)
        size.x = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 4.0f + 26.0f;
    if (size.y <= 0.0f)
        size.y = ImGui::GetFrameHeight();

    const bool clicked = ImGui::InvisibleButton("##toggle", size);
    const bool hovered = ImGui::IsItemHovered();
    if (clicked)
        *value = !*value;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 min = pos;
    const ImVec2 max = ImVec2(pos.x + size.x, pos.y + size.y);
    const float rounding = size.y * 0.5f;
    const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);

    ImU32 bg;
    if (*value)
        bg = ImGui::GetColorU32(hovered ? ImGuiCol_ButtonActive : ImGuiCol_ButtonActive, hovered ? 1.0f : 0.9f);
    else
        bg = ImGui::GetColorU32(hovered ? ImGuiCol_ButtonHovered : ImGuiCol_FrameBg);
    dl->AddRectFilled(min, max, bg, rounding);
    dl->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_Border), rounding, 0, 1.0f);

    // Knob
    const float knobR = size.y * 0.5f - 3.0f;
    const float knobX = *value ? (max.x - size.y * 0.5f) : (min.x + size.y * 0.5f);
    dl->AddCircleFilled(ImVec2(knobX, (min.y + max.y) * 0.5f), knobR,
                        ImGui::GetColorU32(*value ? ImGuiCol_Text : ImGuiCol_TextDisabled));
    if (*value)
        dl->AddCircleFilled(ImVec2(knobX, (min.y + max.y) * 0.5f), knobR * 0.55f, bg);

    if (label && *label) {
        const ImVec2 ts = ImGui::CalcTextSize(label);
        const float textX = *value ? (min.x + rounding + 2.0f) : (max.x - rounding - ts.x - 2.0f);
        dl->AddText(ImVec2(textX, (min.y + max.y) * 0.5f - ts.y * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), label);
    }
    if (tooltip && *tooltip && hovered)
        ImGui::SetTooltip("%s", tooltip);
    (void)accent;
    ImGui::PopID();
    return clicked;
}

bool ColorChip(const char* id, ImVec4* color, float size, const char* tooltip) {
    ImGui::PushID(id);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##chip", ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 min = pos;
    const ImVec2 max = ImVec2(pos.x + size, pos.y + size);
    DrawCheckerboard(dl, min, max, 5.0f, IM_COL32(90, 90, 90, 255), IM_COL32(140, 140, 140, 255));
    dl->AddRectFilled(min, max, ImGui::ColorConvertFloat4ToU32(*color), 3.0f);
    dl->AddRect(min, max, ImGui::GetColorU32(hovered ? ImGuiCol_Text : ImGuiCol_Border), 3.0f, 0, 1.5f);
    if (tooltip && *tooltip && hovered)
        ImGui::SetTooltip("%s", tooltip);
    ImGui::PopID();
    return clicked;
}

bool ChipSelectable(const char* label, bool selected, ImVec4 accent, float height) {
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    const ImVec2 pad = ImGui::GetStyle().FramePadding;
    const ImVec2 size(textSize.x + pad.x * 2.6f, height > 0.0f ? height : textSize.y + pad.y * 0.9f);
    ImGui::PushStyleColor(ImGuiCol_Button, selected ? accent : ImGui::GetStyleColorVec4(ImGuiCol_Button));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImVec4(accent.x * 1.1f, accent.y * 1.1f, accent.z * 1.1f, accent.w));
    ImGui::PushStyleColor(ImGuiCol_Text, selected ? ImVec4(1, 1, 1, 1) : ImGui::GetStyleColorVec4(ImGuiCol_Text));
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return clicked;
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------
void ProgressBarEx(const char* id, float fraction, ImVec2 size, const char* overlay, ImVec4 color, bool pulse) {
    ImGui::PushID(id);
    if (size.x == 0.0f)
        size.x = ImGui::GetContentRegionAvail().x;
    if (size.y == 0.0f)
        size.y = 18.0f;
    if (pulse) {
        const float t = static_cast<float>(ImGui::GetTime()) * 2.0f;
        const float k = 0.75f + 0.25f * std::sin(t);
        color.x = ImMin(1.0f, color.x * (0.85f + k * 0.25f));
        color.y = ImMin(1.0f, color.y * (0.85f + k * 0.25f));
        color.z = ImMin(1.0f, color.z * (0.85f + k * 0.25f));
    }
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, size.y * 0.4f);
    ImGui::ProgressBar(ImClamp(fraction, 0.0f, 1.0f), size, overlay ? overlay : "");
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    ImGui::PopID();
}

void Badge(const char* text, ImVec4 color) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const ImVec2 pad(6.0f, 2.0f);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size(ts.x + pad.x * 2.0f, ts.y + pad.y * 2.0f);
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), ImGui::ColorConvertFloat4ToU32(color),
                      size.y * 0.45f);
    dl->AddText(ImVec2(pos.x + pad.x, pos.y + pad.y), IM_COL32(255, 255, 255, 235), text);
    ImGui::Dummy(size);
}

void StatusDot(ImVec4 color, const char* text) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight();
    dl->AddCircleFilled(ImVec2(pos.x + h * 0.35f, pos.y + h * 0.5f), h * 0.28f,
                        ImGui::ColorConvertFloat4ToU32(color));
    ImGui::Dummy(ImVec2(h * 0.8f, 0.0f));
    ImGui::SameLine(0.0f, 6.0f);
    ImGui::TextUnformatted(text);
}

void TextMuted(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextV(fmt, args);
    ImGui::PopStyleColor();
    va_end(args);
}

void TextMutedWrapped(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    // Wrap at the content region, not the window edge: these lines also sit in
    // columns and table cells, where the window is far wider than the cell.
    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + ImGui::GetContentRegionAvail().x);
    ImGui::TextV(fmt, args);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    va_end(args);
}

void TextAccent(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const ImVec4 c = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
    ImGui::PushStyleColor(ImGuiCol_Text, c);
    ImGui::TextV(fmt, args);
    ImGui::PopStyleColor();
    va_end(args);
}

void HelpMarker(const char* text) {
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void KeyValueRow(const char* key, const char* value) {
    ImGui::TextDisabled("%s", key);
    ImGui::SameLine(120.0f);
    ImGui::TextUnformatted(value);
}

void SeparatorText(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::SeparatorText(text);
    ImGui::PopStyleColor();
}

bool SectionHeader(const char* label, icons::Icon icon, bool defaultOpen) {
    ImGui::PushID(label);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding;
    if (defaultOpen)
        flags |= ImGuiTreeNodeFlags_DefaultOpen;

    const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(accent.x, accent.y, accent.z, 0.18f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(accent.x, accent.y, accent.z, 0.28f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(accent.x, accent.y, accent.z, 0.36f));

    const bool open = ImGui::CollapsingHeader(label, flags);
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    if (icon != icons::Icon::None) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float h = max.y - min.y;
        dl->AddRectFilled(ImVec2(min.x + 1.0f, min.y + 1.0f), ImVec2(min.x + 3.0f, max.y - 1.0f),
                          ImGui::GetColorU32(ImGuiCol_ButtonActive), 1.0f);
        icons::Draw(dl, icon, ImVec2(min.x + 16.0f, (min.y + max.y) * 0.5f), h * 0.6f,
                    ImGui::GetColorU32(ImGuiCol_Text, 0.85f), 1.4f);
    }
    ImGui::PopStyleColor(3);
    ImGui::PopID();
    return open;
}

void StatCard(const char* label, const char* value, const char* unit, ImVec4 color, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    if (size.x <= 0.0f)
        size.x = 116.0f;
    if (size.y <= 0.0f)
        size.y = 52.0f;
    const ImVec2 max(pos.x + size.x, pos.y + size.y);
    dl->AddRectFilled(pos, max, ImGui::GetColorU32(ImGuiCol_FrameBg), 5.0f);
    dl->AddRectFilled(ImVec2(pos.x, pos.y + 3.0f), ImVec2(pos.x + 3.0f, max.y - 3.0f),
                      ImGui::ColorConvertFloat4ToU32(color), 2.0f);
    dl->AddText(ImVec2(pos.x + 10.0f, pos.y + 6.0f), ImGui::GetColorU32(ImGuiCol_TextDisabled), label);
    const ImVec2 vs = ImGui::CalcTextSize(value);
    if (unit && *unit) {
        const ImVec2 us = ImGui::CalcTextSize(unit);
        dl->AddText(ImVec2(pos.x + 10.0f, max.y - vs.y - 8.0f), ImGui::GetColorU32(ImGuiCol_Text), value);
        dl->AddText(ImVec2(pos.x + 12.0f + vs.x, max.y - vs.y - 8.0f),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), unit);
        (void)us;
    } else {
        dl->AddText(ImVec2(pos.x + 10.0f, max.y - vs.y - 8.0f), ImGui::GetColorU32(ImGuiCol_Text), value);
    }
    ImGui::Dummy(size);
}

void DrawCheckerboard(ImDrawList* dl, ImVec2 min, ImVec2 max, float cell, ImU32 colA, ImU32 colB) {
    dl->AddRectFilled(min, max, colA);
    int row = 0;
    for (float y = min.y; y < max.y; y += cell, ++row) {
        int col = 0;
        for (float x = min.x; x < max.x; x += cell, ++col) {
            if ((row + col) % 2 == 0)
                continue;
            dl->AddRectFilled(ImVec2(x, y), ImVec2(ImMin(x + cell, max.x), ImMin(y + cell, max.y)), colB);
        }
    }
}

void DrawGrid(ImDrawList* dl, ImVec2 min, ImVec2 max, float step, ImU32 color, float thickness) {
    if (step <= 1.0f)
        return;
    for (float x = min.x; x < max.x; x += step)
        dl->AddLine(ImVec2(std::floor(x) + 0.5f, min.y), ImVec2(std::floor(x) + 0.5f, max.y), color, thickness);
    for (float y = min.y; y < max.y; y += step)
        dl->AddLine(ImVec2(min.x, std::floor(y) + 0.5f), ImVec2(max.x, std::floor(y) + 0.5f), color, thickness);
}

void DrawDashedRect(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 color, float dash, float gap,
                    float thickness) {
    auto dashLine = [&](ImVec2 a, ImVec2 b) {
        const ImVec2 d(b.x - a.x, b.y - a.y);
        const float len = std::sqrt(d.x * d.x + d.y * d.y);
        if (len <= 0.001f)
            return;
        const ImVec2 dir(d.x / len, d.y / len);
        float t = 0.0f;
        while (t < len) {
            const float t2 = ImMin(t + dash, len);
            dl->AddLine(ImVec2(a.x + dir.x * t, a.y + dir.y * t), ImVec2(a.x + dir.x * t2, a.y + dir.y * t2), color,
                        thickness);
            t = t2 + gap;
        }
    };
    dashLine(min, ImVec2(max.x, min.y));
    dashLine(ImVec2(max.x, min.y), max);
    dashLine(max, ImVec2(min.x, max.y));
    dashLine(ImVec2(min.x, max.y), min);
}

void DrawArrow(ImDrawList* dl, ImVec2 from, ImVec2 to, ImU32 color, float thickness, float headSize) {
    dl->AddLine(from, to, color, thickness);
    const ImVec2 d(to.x - from.x, to.y - from.y);
    const float len = std::sqrt(d.x * d.x + d.y * d.y);
    if (len <= 0.001f)
        return;
    const ImVec2 dir(d.x / len, d.y / len);
    const ImVec2 perp(-dir.y, dir.x);
    const ImVec2 tip = to;
    const ImVec2 base(tip.x - dir.x * headSize, tip.y - dir.y * headSize);
    dl->AddTriangleFilled(tip, ImVec2(base.x + perp.x * headSize * 0.5f, base.y + perp.y * headSize * 0.5f),
                          ImVec2(base.x - perp.x * headSize * 0.5f, base.y - perp.y * headSize * 0.5f), color);
}

// ---------------------------------------------------------------------------
// Structure
// ---------------------------------------------------------------------------
bool Splitter(const char* id, bool vertical, float* ratio, float minRatio, float maxRatio, float thickness) {
    ImGui::PushID(id);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 size = vertical ? ImVec2(thickness, avail.y) : ImVec2(avail.x, thickness);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##splitter", size);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();

    if (hovered || active)
        ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);

    // Remember the ratio and mouse position when the drag starts so the ratio
    // follows the pointer instead of accumulating per-frame deltas.
    static std::unordered_map<std::string, std::pair<float, float>> dragState;
    const std::string key = std::string(id) + "##splitter";
    if (ImGui::IsItemActivated()) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        dragState[key] = { *ratio, vertical ? mouse.x : mouse.y };
    }
    if (active) {
        const float total = vertical ? avail.x : avail.y;
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const float now = vertical ? mouse.x : mouse.y;
        auto it = dragState.find(key);
        if (it != dragState.end() && total > 1.0f) {
            const float delta = (now - it->second.second) / total;
            *ratio = ImClamp(it->second.first + delta, minRatio, maxRatio);
        }
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 min = pos;
    const ImVec2 max = ImVec2(pos.x + size.x, pos.y + size.y);
    if (vertical)
        dl->AddRectFilled(ImVec2(min.x + thickness * 0.5f - 1.0f, min.y), ImVec2(min.x + thickness * 0.5f + 1.0f, max.y),
                          ImGui::GetColorU32(active ? ImGuiCol_ButtonActive
                                                    : (hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Border)));
    else
        dl->AddRectFilled(ImVec2(min.x, min.y + thickness * 0.5f - 1.0f), ImVec2(max.x, min.y + thickness * 0.5f + 1.0f),
                          ImGui::GetColorU32(active ? ImGuiCol_ButtonActive
                                                    : (hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Border)));
    ImGui::PopID();
    return active;
}

void BeginCard(const char* id, ImVec2 size, ImVec4 color, float rounding, bool border) {
    ImGui::PushID(id);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    if (size.x <= 0.0f)
        size.x = ImGui::GetContentRegionAvail().x;
    if (size.y <= 0.0f)
        size.y = ImGui::GetContentRegionAvail().y;
    const ImVec2 max(pos.x + size.x, pos.y + size.y);
    dl->AddRectFilled(pos, max, ImGui::ColorConvertFloat4ToU32(color), rounding);
    if (border)
        dl->AddRect(pos, max, ImGui::GetColorU32(ImGuiCol_Border), rounding, 0, 1.0f);
    ImGui::SetCursorScreenPos(ImVec2(pos.x + 10.0f, pos.y + 10.0f));
    const ImVec2 childSize(ImMax(1.0f, size.x - 20.0f), ImMax(1.0f, size.y - 20.0f));
    ImGui::BeginChild("##card", childSize, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
}

void EndCard() {
    ImGui::EndChild();
    ImGui::PopID();
}

bool SelectableRow(const char* label, bool selected, icons::Icon icon, ImVec4 iconColor, const char* tooltip) {
    ImGui::PushID(label);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight() + ImGui::GetStyle().FramePadding.y * 2.0f;
    const bool clicked = ImGui::Selectable("##row", selected, ImGuiSelectableFlags_AllowDoubleClick,
                                           ImVec2(0.0f, h));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 max(pos.x + ImGui::GetItemRectSize().x, pos.y + h);
    if (icon != icons::Icon::None) {
        const ImU32 col = hovered ? ImGui::GetColorU32(ImGuiCol_Text) : ImGui::ColorConvertFloat4ToU32(iconColor);
        icons::Draw(dl, icon, ImVec2(pos.x + 9.0f, (pos.y + max.y) * 0.5f), h * 0.55f, col, 1.4f);
    }
    dl->AddText(ImVec2(pos.x + 22.0f, (pos.y + max.y) * 0.5f - ImGui::GetTextLineHeight() * 0.5f),
                ImGui::GetColorU32(ImGuiCol_Text), label);
    if (tooltip && *tooltip && hovered)
        ImGui::SetTooltip("%s", tooltip);
    ImGui::PopID();
    return clicked;
}

bool ListRow(const char* label, bool selected, float height, bool* doubleClicked) {
    ImGui::PushID(label);
    const float h = height > 0.0f ? height : ImGui::GetTextLineHeight() + ImGui::GetStyle().FramePadding.y * 2.0f;
    const bool clicked = ImGui::Selectable("##row", selected, ImGuiSelectableFlags_AllowDoubleClick,
                                           ImVec2(0.0f, h));
    if (doubleClicked)
        *doubleClicked = clicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetItemRectMin();
    dl->AddText(ImVec2(pos.x + ImGui::GetStyle().FramePadding.x, pos.y + h * 0.5f - ImGui::GetTextLineHeight() * 0.5f),
                ImGui::GetColorU32(ImGuiCol_Text), label);
    ImGui::PopID();
    return clicked;
}

// ---------------------------------------------------------------------------
// Edit tracking
// ---------------------------------------------------------------------------
namespace {
std::unordered_map<std::string, WidgetState>& SnapshotMap() {
    static std::unordered_map<std::string, WidgetState> map;
    return map;
}
} // namespace

void BeginEditFrame(AppContext& ctx) {
    // While an item is active (dragging a slider, typing in a text field) the
    // stored "before" values must be preserved. When idle we refresh them so
    // edits made by the demo pages are picked up as well.
    if (ImGui::IsAnyItemActive())
        return;
    auto& map = SnapshotMap();
    for (const auto& item : ctx.registry.All())
        map[item->id] = item->state;
}

void TrackEdit(AppContext& ctx, WidgetInstance& inst, const char* label) {
    auto& map = SnapshotMap();
    auto it = map.find(inst.id);
    if (it == map.end())
        it = map.emplace(inst.id, inst.state).first;

    if (ImGui::IsItemDeactivatedAfterEdit()) {
        ctx.undo.Push(inst.id, inst.displayName, label ? label : "Property", it->second, inst.state);
        it->second = inst.state;
        if (ctx.settings.logAllWidgetEdits)
            ctx.LogDebug("Inspector", inst.displayName + ": " + (label ? label : "property") + " changed");
    }
}

// ---------------------------------------------------------------------------
// Misc helpers
// ---------------------------------------------------------------------------
std::string Ellipsize(const std::string& text, float maxWidth) {
    if (ImGui::CalcTextSize(text.c_str()).x <= maxWidth)
        return text;
    std::string out = text;
    while (!out.empty() && ImGui::CalcTextSize((out + "...").c_str()).x > maxWidth)
        out.pop_back();
    return out + "...";
}

ImVec4 ColorFromIndex(int index) {
    static const ImVec4 palette[] = {
        ImVec4(0.26f, 0.59f, 0.98f, 1.00f), ImVec4(0.33f, 0.78f, 0.42f, 1.00f),
        ImVec4(0.95f, 0.62f, 0.24f, 1.00f), ImVec4(0.62f, 0.47f, 0.96f, 1.00f),
        ImVec4(0.92f, 0.36f, 0.32f, 1.00f), ImVec4(0.30f, 0.80f, 0.82f, 1.00f),
        ImVec4(0.88f, 0.72f, 0.32f, 1.00f), ImVec4(0.70f, 0.70f, 0.76f, 1.00f)
    };
    const int count = static_cast<int>(sizeof(palette) / sizeof(palette[0]));
    if (count == 0)
        return ImVec4(1, 1, 1, 1);
    const int i = ((index % count) + count) % count;
    return palette[i];
}

const char* ShortcutText(const char* id) {
    // Small lookup table of shortcuts shown in tooltips/menus.
    struct Entry { const char* id; const char* text; };
    static const Entry entries[] = {
        { "new", "Ctrl+N" }, { "open", "Ctrl+O" }, { "save", "Ctrl+S" },
        { "save_layout", "Ctrl+Shift+S" }, { "undo", "Ctrl+Z" }, { "redo", "Ctrl+Y" },
        { "duplicate", "Ctrl+D" }, { "delete", "Del" }, { "copy", "Ctrl+C" }, { "paste", "Ctrl+V" },
        { "play", "F5" }, { "pause", "F6" }, { "stop", "F7" }, { "reset_layout", "Ctrl+L" },
        { "theme", "F2" }, { "search", "Ctrl+F" }, { "toggle_console", "Ctrl+`" }
    };
    for (const Entry& e : entries)
        if (std::string(e.id) == id)
            return e.text;
    return "";
}

} // namespace widgets
} // namespace uilab
