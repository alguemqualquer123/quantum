// Widgets.h - reusable UI building blocks shared by the panels and the demo
// pages (all built on top of Dear ImGui but with the lab's own look and feel).
#pragma once

#include <string>
#include <vector>

#include <imgui.h>

#include "core/Types.h"
#include "ui/Icons.h"

namespace uilab {

class AppContext;
struct WidgetInstance;

namespace widgets {

// ---- inputs ---------------------------------------------------------------
bool SearchField(const char* id, std::string& text, const char* hint, float width, bool* clearedOut = nullptr);
bool NumericIntField(const char* id, int* value, int minValue, int maxValue, int step, float width);
bool NumericFloatField(const char* id, float* value, float minValue, float maxValue, float step, float width,
                       const char* format = "%.2f");
bool ToggleButton(const char* id, const char* label, bool* value, ImVec2 size, const char* tooltip = nullptr);
bool ColorChip(const char* id, ImVec4* color, float size, const char* tooltip);
bool ChipSelectable(const char* label, bool selected, ImVec4 accent, float height = 0.0f);

// ---- display --------------------------------------------------------------
void ProgressBarEx(const char* id, float fraction, ImVec2 size, const char* overlay, ImVec4 color,
                   bool pulse = false);
void Badge(const char* text, ImVec4 color);
void StatusDot(ImVec4 color, const char* text);
void TextMuted(const char* fmt, ...);
// Same as TextMuted but wraps at the available width instead of clipping.
void TextMutedWrapped(const char* fmt, ...);
void TextAccent(const char* fmt, ...);
void HelpMarker(const char* text);
void KeyValueRow(const char* key, const char* value);
void SeparatorText(const char* text);
bool SectionHeader(const char* label, icons::Icon icon, bool defaultOpen = true);
void StatCard(const char* label, const char* value, const char* unit, ImVec4 color, ImVec2 size);
void DrawCheckerboard(ImDrawList* dl, ImVec2 min, ImVec2 max, float cell, ImU32 colA, ImU32 colB);
void DrawGrid(ImDrawList* dl, ImVec2 min, ImVec2 max, float step, ImU32 color, float thickness = 1.0f);
void DrawDashedRect(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 color, float dash = 6.0f, float gap = 4.0f,
                    float thickness = 1.0f);
void DrawArrow(ImDrawList* dl, ImVec2 from, ImVec2 to, ImU32 color, float thickness = 1.5f, float headSize = 8.0f);

// ---- structure ------------------------------------------------------------
// Draggable splitter. 'ratio' is the fraction of the available space given to
// the first pane. Returns true while being dragged.
bool Splitter(const char* id, bool vertical, float* ratio, float minRatio, float maxRatio, float thickness = 6.0f);

// Rounded translucent panel used for the demo pages.
void BeginCard(const char* id, ImVec2 size, ImVec4 color, float rounding = 6.0f, bool border = true);
void EndCard();

// Row helper: icon + selectable with hover colour.
bool SelectableRow(const char* label, bool selected, icons::Icon icon, ImVec4 iconColor, const char* tooltip = nullptr);

// Wraps ImGui::Selectable with a multi-select style highlight and double click detection.
bool ListRow(const char* label, bool selected, float height = 0.0f, bool* doubleClicked = nullptr);

// ---- edit tracking (undo/redo integration) --------------------------------
// BeginEditFrame() must be called once per frame before the Inspector draws:
// it snapshots the state of every component so "before" values are exact.
void BeginEditFrame(AppContext& ctx);
// Call right after every widget that edits a WidgetState field.
void TrackEdit(AppContext& ctx, WidgetInstance& inst, const char* label);

// ---- misc -----------------------------------------------------------------
std::string Ellipsize(const std::string& text, float maxWidth);
ImVec4 ColorFromIndex(int index);
const char* ShortcutText(const char* id);

} // namespace widgets
} // namespace uilab
