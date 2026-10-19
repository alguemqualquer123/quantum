// InspectorPanel.cpp - property editor for the selected component.
//
// Every widget here edits the WidgetState of the selected instance, and the
// demo pages render from that same state, so changes are visible immediately.
// Continuous widgets are tracked through widgets::TrackEdit (one history entry
// per edit), discrete widgets push an explicit "before" snapshot.
#include "panels/Panels.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <string>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "core/AppContext.h"
#include "core/Dialogs.h"
#include "core/Validation.h"
#include "ui/Icons.h"
#include "ui/Widgets.h"

namespace uilab {
namespace panels {

namespace {

constexpr float kLabelWidth = 116.0f;

void PropTableBegin(const char* id) {
    ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadInnerX);
    ImGui::TableSetupColumn("##key", ImGuiTableColumnFlags_WidthFixed, kLabelWidth);
    ImGui::TableSetupColumn("##val", ImGuiTableColumnFlags_WidthStretch);
}

void PropRow(const char* label, const char* tooltip = nullptr) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (tooltip && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tooltip);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
}

void CommitDiscrete(AppContext& ctx, WidgetInstance& inst, const char* label, const WidgetState& before) {
    ctx.undo.Push(inst.id, inst.displayName, label, before, inst.state);
    if (ctx.settings.logAllWidgetEdits)
        ctx.LogDebug("Inspector", inst.displayName + ": " + label);
}

void DrawAnchorPreview(const WidgetState& s) {
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = 110.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 boxMin = ImVec2(origin.x + 6.0f, origin.y + 6.0f);
    const ImVec2 boxMax = ImVec2(origin.x + width - 6.0f, origin.y + height - 6.0f);
    dl->AddRectFilled(boxMin, boxMax, ImGui::GetColorU32(ImGuiCol_FrameBg), 4.0f);
    dl->AddRect(boxMin, boxMax, ImGui::GetColorU32(ImGuiCol_Border), 4.0f);

    const ImVec2 aMin = AnchorVec(static_cast<AnchorPreset>(s.anchorMin));
    const ImVec2 aMax = AnchorVec(static_cast<AnchorPreset>(s.anchorMax));
    const ImVec2 pv = AnchorVec(static_cast<AnchorPreset>(s.pivot));
    const ImVec2 size(boxMax.x - boxMin.x, boxMax.y - boxMin.y);
    const auto toScreen = [&](const ImVec2& n) {
        return ImVec2(boxMin.x + n.x * size.x, boxMin.y + n.y * size.y);
    };
    const ImU32 anchorCol = ImGui::GetColorU32(ImGuiCol_TextDisabled);

    const ImVec2 amin = toScreen(aMin);
    const ImVec2 amax = toScreen(aMax);
    dl->AddLine(ImVec2(amin.x - 6, amin.y), ImVec2(amin.x + 6, amin.y), anchorCol);
    dl->AddLine(ImVec2(amin.x, amin.y - 6), ImVec2(amin.x, amin.y + 6), anchorCol);
    dl->AddLine(ImVec2(amax.x - 6, amax.y), ImVec2(amax.x + 6, amax.y), anchorCol);
    dl->AddLine(ImVec2(amax.x, amax.y - 6), ImVec2(amax.x, amax.y + 6), anchorCol);

    // Widget rect: anchored position inside the anchor rect, offset by the pivot.
    const ImVec2 anchorPoint(amin.x + (amax.x - amin.x) * 0.5f, amin.y + (amax.y - amin.y) * 0.5f);
    const ImVec2 wSize(std::max(6.0f, s.size.x * s.scale.x * 0.35f),
                       std::max(6.0f, s.size.y * s.scale.y * 0.35f));
    const ImVec2 center(anchorPoint.x + s.position.x * 0.35f + (0.5f - pv.x) * wSize.x,
                        anchorPoint.y + s.position.y * 0.35f + (0.5f - pv.y) * wSize.y);
    const ImVec2 wMin(center.x - wSize.x * 0.5f, center.y - wSize.y * 0.5f);
    const ImVec2 wMax(center.x + wSize.x * 0.5f, center.y + wSize.y * 0.5f);
    dl->AddRectFilled(wMin, wMax, ImGui::GetColorU32(s.color), 3.0f);
    dl->AddRect(wMin, wMax, ImGui::GetColorU32(ImGuiCol_Text), 3.0f);
    dl->AddCircleFilled(ImVec2(wMin.x + pv.x * wSize.x, wMin.y + pv.y * wSize.y), 3.0f,
                        ImGui::GetColorU32(ImGuiCol_Text));

    ImGui::Dummy(ImVec2(width, height));
}

void DrawItemsEditor(AppContext& ctx, WidgetInstance& inst) {
    WidgetState& s = inst.state;
    char addLabel[128];
    std::snprintf(addLabel, sizeof(addLabel), "##add_item_%s", inst.id.c_str());

    if (ImGui::Button("Add Item")) {
        const WidgetState before = s;
        s.items.push_back("Item " + std::to_string(s.items.size() + 1));
        CommitDiscrete(ctx, inst, "Add item", before);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear Items")) {
        const WidgetState before = s;
        s.items.clear();
        s.selectedIndex = 0;
        CommitDiscrete(ctx, inst, "Clear items", before);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%d items", static_cast<int>(s.items.size()));

    const float listHeight = std::min(180.0f, 28.0f * static_cast<float>(s.items.size()) + 8.0f);
    if (ImGui::BeginChild("##items", ImVec2(0.0f, listHeight), ImGuiChildFlags_Borders)) {
        for (int i = 0; i < static_cast<int>(s.items.size()); ++i) {
            ImGui::PushID(i);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "##item%d", i);
            if (ImGui::InputText(buf, &s.items[i])) {
                widgets::TrackEdit(ctx, inst, "Item text");
            }
            ImGui::SameLine(0.0f, 4.0f);
            if (icons::IconButton("##up", icons::Icon::ChevronUp, "Move up", ImVec2(18, 18), false,
                                  i > 0, 0.8f)) {
                const WidgetState before = s;
                std::swap(s.items[i], s.items[i - 1]);
                CommitDiscrete(ctx, inst, "Reorder item", before);
            }
            ImGui::SameLine(0.0f, 2.0f);
            if (icons::IconButton("##down", icons::Icon::ChevronDown, "Move down", ImVec2(18, 18), false,
                                  i + 1 < static_cast<int>(s.items.size()), 0.8f)) {
                const WidgetState before = s;
                std::swap(s.items[i], s.items[i + 1]);
                CommitDiscrete(ctx, inst, "Reorder item", before);
            }
            ImGui::SameLine(0.0f, 2.0f);
            if (icons::IconButton("##del", icons::Icon::Trash, "Remove item", ImVec2(18, 18), false, true, 0.8f)) {
                const WidgetState before = s;
                s.items.erase(s.items.begin() + i);
                s.selectedIndex = std::min(s.selectedIndex, std::max(0, static_cast<int>(s.items.size()) - 1));
                CommitDiscrete(ctx, inst, "Remove item", before);
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    static std::string newItem = "New Item";
    if (ImGui::InputText(addLabel, &newItem, ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (!newItem.empty()) {
            const WidgetState before = s;
            s.items.push_back(newItem);
            CommitDiscrete(ctx, inst, "Add item", before);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("+")) {
        if (!newItem.empty()) {
            const WidgetState before = s;
            s.items.push_back(newItem);
            CommitDiscrete(ctx, inst, "Add item", before);
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("type + Enter to add");
}

void DrawValidationSection(AppContext& ctx, WidgetInstance& inst) {
    WidgetState& s = inst.state;

    static int ruleIndex = -1;
    static std::string ruleOwner;
    int ruleCount = 0;
    const char* const* rules = ValidationRules(ruleCount);
    if (ruleOwner != inst.id) {
        ruleOwner = inst.id;
        ruleIndex = -1;
    }
    if (ruleIndex < 0) {
        for (int i = 0; i < ruleCount; ++i)
            if (s.validationRule == rules[i])
                ruleIndex = i;
        if (ruleIndex < 0)
            ruleIndex = 0;
    }

    if (ImGui::Combo("##rule", &ruleIndex, rules, ruleCount)) {
        const WidgetState before = s;
        s.validationRule = rules[ruleIndex];
        CommitDiscrete(ctx, inst, "Validation rule", before);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", ValidationRuleHint(s.validationRule));

    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputText("##pattern", &s.pattern))
        widgets::TrackEdit(ctx, inst, "Pattern");

    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputText("##hint", &s.hint))
        widgets::TrackEdit(ctx, inst, "Hint");

    // Live validation feedback -------------------------------------------------
    std::string error;
    const std::string probe = s.multilineText.empty() ? s.text : s.multilineText;
    const bool valid = ValidateText(probe, s.validationRule, s.pattern, error);
    s.validationMessage = valid ? std::string() : error;
    const ImVec4 statusColor = valid ? ctx.theme.Active().success : ctx.theme.Active().error;
    ImGui::PushStyleColor(ImGuiCol_Text, statusColor);
    if (valid)
        icons::IconText(icons::Icon::Check, "  value is valid", ImGui::GetColorU32(ImGuiCol_Text));
    else
        icons::IconText(icons::Icon::Warning, ("  " + error).c_str(), ImGui::GetColorU32(ImGuiCol_Text));
    ImGui::PopStyleColor();
}

void DrawSelectedBody(AppContext& ctx, WidgetInstance& inst) {
    WidgetState& s = inst.state;
    const WidgetTraits& t = inst.Traits();

    // ---- identity -------------------------------------------------------
    if (ImGui::CollapsingHeader("Identity", ImGuiTreeNodeFlags_DefaultOpen)) {
        PropTableBegin("identity");
        PropRow("Name");
        if (ImGui::InputText("##name", &inst.displayName))
            widgets::TrackEdit(ctx, inst, "Name");
        if (ImGui::IsItemDeactivatedAfterEdit())
            ctx.LogEvent("Inspector", "Renamed component to " + inst.displayName);

        PropRow("Kind");
        ImGui::TextUnformatted(KindName(inst.kind));
        PropRow("Id", "Stable identifier used by the demos and the registry");
        ImGui::TextDisabled("%s", inst.id.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("copy")) {
            ImGui::SetClipboardText(inst.id.c_str());
            ctx.LogEvent("Inspector", "Copied id " + inst.id);
        }

        PropRow("Category");
        static int catIndex = 0;
        catIndex = static_cast<int>(inst.category);
        const char* catNames[static_cast<int>(Category::Count) + 1] = {};
        for (int i = 0; i < static_cast<int>(Category::Count); ++i)
            catNames[i] = CategoryName(static_cast<Category>(i));
        if (ImGui::Combo("##category", &catIndex, catNames, static_cast<int>(Category::Count))) {
            const WidgetState before = s;
            inst.category = static_cast<Category>(catIndex);
            CommitDiscrete(ctx, inst, "Category", before);
            ctx.LogEvent("Inspector", inst.displayName + " moved to " + CategoryName(inst.category));
        }

        PropRow("Group");
        if (ImGui::InputText("##group", &inst.group))
            widgets::TrackEdit(ctx, inst, "Group");

        PropRow("Demo binding");
        ImGui::TextDisabled("%s", inst.liveBound ? "live (rendered by a demo page)" : "not bound (inspector only)");
        ImGui::EndTable();
    }

    // ---- content --------------------------------------------------------
    if (ImGui::CollapsingHeader("Content / Style", ImGuiTreeNodeFlags_DefaultOpen)) {
        PropTableBegin("content");
        PropRow("Text");
        if (ImGui::InputText("##text", &s.text))
            widgets::TrackEdit(ctx, inst, "Text");
        PropRow("Tooltip");
        if (ImGui::InputText("##tooltip", &s.tooltip))
            widgets::TrackEdit(ctx, inst, "Tooltip");
        PropRow("Hint");
        if (ImGui::InputText("##hintText", &s.hint))
            widgets::TrackEdit(ctx, inst, "Hint");
        PropRow("Style class", "Used by the UI Toolkit page (USS class names)");
        if (ImGui::InputText("##class", &s.styleClass))
            widgets::TrackEdit(ctx, inst, "Style class");
        PropRow("Style id", "Matches #id selectors in the mini stylesheet");
        if (ImGui::InputText("##styleid", &s.styleId))
            widgets::TrackEdit(ctx, inst, "Style id");
        ImGui::EndTable();
    }

    // ---- state ----------------------------------------------------------
    if (ImGui::CollapsingHeader("State", ImGuiTreeNodeFlags_DefaultOpen)) {
        const auto flagRow = [&](const char* label, bool& flag, const char* undoLabel) {
            WidgetState before = s;
            if (ImGui::Checkbox(label, &flag))
                CommitDiscrete(ctx, inst, undoLabel, before);
        };
        flagRow("Visible", s.visible, "Visible");
        ImGui::SameLine();
        flagRow("Enabled", s.enabled, "Enabled");
        if (t.toggled) {
            flagRow("Toggled", s.toggled, "Toggled");
            ImGui::SameLine();
        }
        if (t.validation) {
            flagRow("Read only", s.readOnly, "Read only");
        } else {
            ImGui::NewLine();
        }
        if (t.toggled || t.validation) {
            ImGui::SameLine();
            flagRow("Checked", s.checked, "Checked");
        }
    }

    // ---- numeric values -------------------------------------------------
    if (t.value) {
        if (ImGui::CollapsingHeader("Value", ImGuiTreeNodeFlags_DefaultOpen)) {
            PropTableBegin("value");
            PropRow("Value");
            if (ImGui::SliderFloat("##value", &s.value, s.minValue, s.maxValue, s.format.c_str()))
                widgets::TrackEdit(ctx, inst, "Value");
            PropRow("Range");
            float range[2] = { s.minValue, s.maxValue };
            if (ImGui::DragFloat2("##range", range, 0.05f))
                widgets::TrackEdit(ctx, inst, "Range");
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                if (range[1] > range[0]) {
                    s.minValue = range[0];
                    s.maxValue = range[1];
                }
            }
            PropRow("Step");
            if (ImGui::DragFloat("##step", &s.step, 0.005f, 0.0001f, 10.0f, "%.4f"))
                widgets::TrackEdit(ctx, inst, "Step");
            PropRow("Format");
            if (ImGui::InputText("##format", &s.format))
                widgets::TrackEdit(ctx, inst, "Format");
            ImGui::EndTable();
        }
    }

    if (t.integer) {
        if (ImGui::CollapsingHeader("Integer Value", ImGuiTreeNodeFlags_DefaultOpen)) {
            PropTableBegin("intvalue");
            PropRow("Value");
            if (ImGui::SliderInt("##intvalue", &s.intValue, s.intMin, s.intMax))
                widgets::TrackEdit(ctx, inst, "Integer value");
            PropRow("Range");
            int range[2] = { s.intMin, s.intMax };
            if (ImGui::DragInt2("##intrange", range))
                widgets::TrackEdit(ctx, inst, "Integer range");
            if (ImGui::IsItemDeactivatedAfterEdit() && range[1] > range[0]) {
                s.intMin = range[0];
                s.intMax = range[1];
            }
            PropRow("Selected index");
            if (ImGui::DragInt("##selidx", &s.selectedIndex, 0.1f))
                widgets::TrackEdit(ctx, inst, "Selected index");
            ImGui::EndTable();
        }
    }

    // ---- items ----------------------------------------------------------
    if (t.items) {
        if (ImGui::CollapsingHeader("Items", ImGuiTreeNodeFlags_DefaultOpen))
            DrawItemsEditor(ctx, inst);
    }

    // ---- multiline text -------------------------------------------------
    if (inst.kind == WidgetKind::MultilineInput) {
        if (ImGui::CollapsingHeader("Multiline Text", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::InputTextMultiline("##multiline", &s.multilineText, ImVec2(-FLT_MIN, 110.0f)))
                widgets::TrackEdit(ctx, inst, "Multiline text");
            ImGui::TextDisabled("%d characters, %d lines", static_cast<int>(s.multilineText.size()),
                                static_cast<int>(std::count(s.multilineText.begin(), s.multilineText.end(), '\n')) + 1);
        }
    }

    // ---- colours --------------------------------------------------------
    if (t.color) {
        if (ImGui::CollapsingHeader("Colours", ImGuiTreeNodeFlags_DefaultOpen)) {
            PropTableBegin("colors");
            PropRow("Primary");
            if (ImGui::ColorEdit4("##color", &s.color.x))
                widgets::TrackEdit(ctx, inst, "Primary colour");
            PropRow("Secondary");
            if (ImGui::ColorEdit4("##color2", &s.color2.x))
                widgets::TrackEdit(ctx, inst, "Secondary colour");
            PropRow("Presets");
            for (int i = 0; i < 8; ++i) {
                ImGui::PushID(i);
                if (ImGui::ColorButton("##preset", widgets::ColorFromIndex(i), ImGuiColorEditFlags_NoTooltip,
                                       ImVec2(20.0f, 20.0f))) {
                    WidgetState before = s;
                    s.color = widgets::ColorFromIndex(i);
                    CommitDiscrete(ctx, inst, "Preset colour", before);
                }
                ImGui::PopID();
                if (i != 7)
                    ImGui::SameLine();
            }
            ImGui::TextDisabled("(palette chips assign on click)");
            PropRow("Custom colour");
            {
                WidgetState before = s;
                if (ImGui::Checkbox("use component colour##custom", &s.useCustomColor))
                    CommitDiscrete(ctx, inst, "Use custom colour", before);
            }
            ImGui::EndTable();
        }
    }

    // ---- transform ------------------------------------------------------
    if (t.transform) {
        if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
            PropTableBegin("transform");
            PropRow("Position");
            if (ImGui::DragFloat2("##pos", &s.position.x, 0.5f))
                widgets::TrackEdit(ctx, inst, "Position");
            PropRow("Size");
            if (ImGui::DragFloat2("##size", &s.size.x, 0.5f, 0.0f, 8000.0f))
                widgets::TrackEdit(ctx, inst, "Size");
            PropRow("Scale");
            if (ImGui::DragFloat2("##scale", &s.scale.x, 0.01f, 0.01f, 10.0f))
                widgets::TrackEdit(ctx, inst, "Scale");
            PropRow("Rotation");
            if (ImGui::SliderFloat("##rot", &s.rotation, -180.0f, 180.0f, "%.1f deg"))
                widgets::TrackEdit(ctx, inst, "Rotation");
            PropRow("Opacity");
            if (ImGui::SliderFloat("##alpha", &s.opacity, 0.0f, 1.0f, "%.2f"))
                widgets::TrackEdit(ctx, inst, "Opacity");
            PropRow("Z order");
            if (ImGui::DragInt("##zorder", &s.zOrder))
                widgets::TrackEdit(ctx, inst, "Z order");
            ImGui::EndTable();
        }
    }

    // ---- anchors --------------------------------------------------------
    if (t.anchors) {
        if (ImGui::CollapsingHeader("Anchors & Pivot", ImGuiTreeNodeFlags_DefaultOpen)) {
            PropTableBegin("anchors");
            const auto anchorCombo = [&](const char* label, int& value, const char* undoLabel) {
                PropRow(label);
                int preset = value;
                const char* names[static_cast<int>(AnchorPreset::Count) + 1] = {};
                for (int i = 0; i < static_cast<int>(AnchorPreset::Count); ++i)
                    names[i] = AnchorName(static_cast<AnchorPreset>(i));
                if (ImGui::Combo("##ac", &preset, names, static_cast<int>(AnchorPreset::Count))) {
                    WidgetState before = s;
                    value = preset;
                    CommitDiscrete(ctx, inst, undoLabel, before);
                }
            };
            anchorCombo("Anchor min", s.anchorMin, "Anchor min");
            anchorCombo("Anchor max", s.anchorMax, "Anchor max");
            anchorCombo("Pivot", s.pivot, "Pivot");
            PropRow("Alignment");
            int align = s.alignment;
            if (ImGui::Combo("##align", &align, "Left\0Center\0Right\0")) {
                WidgetState before = s;
                s.alignment = align;
                CommitDiscrete(ctx, inst, "Alignment", before);
            }
            ImGui::EndTable();
            DrawAnchorPreview(s);
        }
    }

    // ---- spacing / flex --------------------------------------------------
    if (t.spacing) {
        if (ImGui::CollapsingHeader("Margins & Spacing", ImGuiTreeNodeFlags_DefaultOpen)) {
            PropTableBegin("spacing");
            PropRow("Spacing");
            if (ImGui::DragFloat2("##spacing", &s.spacingX, 0.2f, 0.0f, 200.0f))
                widgets::TrackEdit(ctx, inst, "Spacing");
            PropRow("Padding");
            if (ImGui::DragFloat2("##padding", &s.paddingX, 0.2f, 0.0f, 200.0f))
                widgets::TrackEdit(ctx, inst, "Padding");
            PropRow("Margin");
            if (ImGui::DragFloat2("##margin", &s.marginX, 0.2f, 0.0f, 200.0f))
                widgets::TrackEdit(ctx, inst, "Margin");
            PropRow("Flex direction");
            int dir = s.flexDirection;
            if (ImGui::Combo("##flexdir", &dir, "Row\0Column\0")) {
                WidgetState before = s;
                s.flexDirection = dir;
                CommitDiscrete(ctx, inst, "Flex direction", before);
            }
            PropRow("Flex grow");
            if (ImGui::SliderFloat("##flexgrow", &s.flexGrow, 0.0f, 4.0f, "%.2f"))
                widgets::TrackEdit(ctx, inst, "Flex grow");
            PropRow("Align items");
            int alignItems = s.alignItems;
            if (ImGui::Combo("##alignitems", &alignItems, "Start\0Center\0End\0Stretch\0")) {
                WidgetState before = s;
                s.alignItems = alignItems;
                CommitDiscrete(ctx, inst, "Align items", before);
            }
            ImGui::EndTable();
        }
    }

    // ---- validation -----------------------------------------------------
    if (t.validation) {
        if (ImGui::CollapsingHeader("Validation", ImGuiTreeNodeFlags_DefaultOpen))
            DrawValidationSection(ctx, inst);
    }

    // ---- actions ---------------------------------------------------------
    ImGui::Separator();
    if (ImGui::Button("Duplicate", ImVec2(96, 0)))
        ctx.DuplicateSelectedWidget();
    ImGui::SameLine();
    if (ImGui::Button("Reset", ImVec2(80, 0)))
        ctx.ResetWidgetState(inst);
    ImGui::SameLine();
    if (ImGui::Button("Copy", ImVec2(72, 0)))
        ctx.CopySelectedProperties();
    ImGui::SameLine();
    if (ImGui::Button("Paste", ImVec2(72, 0)))
        ctx.PastePropertiesIntoSelected();

    if (ImGui::Button("Reveal On Demo Page", ImVec2(200, 0)))
        RevealComponent(ctx, inst.id);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Button));
    if (ImGui::Button("Delete...", ImVec2(90, 0)))
        RequestDeleteComponent(ctx, inst);
    ImGui::PopStyleColor();

    ImGui::Separator();
    ImGui::TextDisabled("History: %d undo / %d redo", static_cast<int>(ctx.undo.UndoCount()),
                        static_cast<int>(ctx.undo.RedoCount()));
    if (ctx.undo.CanUndo()) {
        ImGui::SameLine();
        ImGui::TextDisabled("| next: %s", ctx.undo.UndoLabel().c_str());
    }
}

void DrawNoSelection(AppContext& ctx) {
    ImGui::TextWrapped("No component selected. Pick one in the Component Tree on the left, or use one of the "
                       "shortcuts below.");
    ImGui::Separator();
    static const char* quick[] = {
        "basic.button.primary", "basic.slider.horizontal", "basic.input.name", "basic.combo.quality",
        "layout.splitter.main", "slate.window.spawn", "umg.transform.widget", "ut.flex.container",
        "im.stats.graphs", "ugui.recttransform.target"
    };
    for (const char* id : quick) {
        const WidgetInstance* w = ctx.registry.Find(id);
        if (!w)
            continue;
        if (widgets::SelectableRow(w->displayName.c_str(), false, IconForKind(w->kind),
                                   AccentForCategory(w->category), id)) {
            ctx.SelectWidget(w->id);
        }
    }
    ImGui::Separator();
    widgets::TextMutedWrapped("Registry: %d components, %d live-bound", static_cast<int>(ctx.registry.Count()),
                              ctx.registry.CountLive());
    widgets::TextMutedWrapped("Tip: right-click a tree row for rename / duplicate / delete.");
}

} // namespace

void DrawInspector(AppContext& ctx) {
    if (!ImGui::Begin(kInspectorTitle)) {
        ImGui::End();
        return;
    }

    if (ctx.host.inspector) {
        ctx.host.inspector(ctx);
        ImGui::End();
        return;
    }

    WidgetInstance* inst = ctx.SelectedWidget();
    if (!inst) {
        DrawNoSelection(ctx);
        ImGui::End();
        return;
    }

    // Header: kind + id + live badge -------------------------------------
    const ImVec4 accent = AccentForCategory(inst->category);
    ImGui::PushStyleColor(ImGuiCol_Text, accent);
    icons::IconText(IconForKind(inst->kind), KindName(inst->kind), ImGui::GetColorU32(ImGuiCol_Text));
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (!inst->state.enabled) {
        widgets::Badge("disabled", ImVec4(0.55f, 0.55f, 0.60f, 1.0f));
        ImGui::SameLine();
    }
    if (!inst->state.visible) {
        widgets::Badge("hidden", ImVec4(0.55f, 0.55f, 0.60f, 1.0f));
        ImGui::SameLine();
    }
    widgets::Badge(inst->liveBound ? "live" : "unbound", inst->liveBound ? accent : ImVec4(0.45f, 0.47f, 0.52f, 1.0f));

    ImGui::TextDisabled("%s / %s", CategoryName(inst->category), inst->group.c_str());
    ImGui::Separator();

    DrawSelectedBody(ctx, *inst);

    ImGui::End();
}

} // namespace panels
} // namespace uilab
