// DemoUIToolkit.cpp - Unity UI Toolkit style page: a live UXML document plus an
// editable USS sheet resolved through the mini stylesheet engine each frame.
#include "demo/DemoHost.h"

#include <algorithm>
#include <cfloat>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "core/AppContext.h"
#include "demo/Stylesheet.h"
#include "panels/Panels.h"
#include "ui/Icons.h"
#include "ui/Widgets.h"

namespace uilab {
namespace demo {

namespace {

// The sheet is shared with the floating inspector window in this file.
demo::Sheet g_sheet;
std::string g_ussText;
std::string g_ussError;
bool g_ussOk = true;
bool g_sheetSeeded = false;
int g_selectedElement = 0;
std::string g_newElement = "label#subtitle";
bool g_hoverTitle = false;
bool g_hoverToolbar = false;
bool g_hoverCard = false;
bool g_hoverButton = false;

void EnsureSheetSeeded() {
    if (g_sheetSeeded)
        return;
    g_ussText = Sheet::DefaultUss();
    g_ussOk = g_sheet.Parse(g_ussText, g_ussError);
    g_sheetSeeded = true;
}

bool IsNameChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_';
}

bool IsSpaceChar(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

struct ElementDesc {
    std::string tag = "visual-element";
    std::string id;
    std::vector<std::string> classes;
};

// "label#title.card.primary" -> tag label, id title, classes {card, primary}.
ElementDesc ParseElement(const std::string& text) {
    ElementDesc e;
    size_t p = 0;
    const size_t tagStart = p;
    while (p < text.size() && IsNameChar(text[p]))
        ++p;
    if (p > tagStart)
        e.tag = text.substr(tagStart, p - tagStart);
    while (p < text.size()) {
        const char c = text[p];
        if (c == ':' || c == ' ' || IsSpaceChar(c))
            break;
        if (c == '#' || c == '.') {
            ++p;
            const size_t start = p;
            while (p < text.size() && text[p] != '.' && text[p] != '#' && text[p] != ':' &&
                   !IsSpaceChar(text[p]))
                ++p;
            const std::string token = text.substr(start, p - start);
            if (c == '#')
                e.id = token;
            else if (!token.empty())
                e.classes.push_back(token);
        } else {
            ++p;
        }
    }
    return e;
}

std::string SafeItem(const std::vector<std::string>& items, int index) {
    if (items.empty())
        return std::string();
    if (index < 0)
        index = 0;
    if (index >= static_cast<int>(items.size()))
        index = static_cast<int>(items.size()) - 1;
    return items[index];
}

std::string BuildUxmlDoc(const std::string& fileName, const std::vector<std::string>& items) {
    std::string doc = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    doc += "<!-- " + fileName + " - generated from the live element hierarchy -->\n";
    doc += "<ui:UXML xmlns:ui=\"UnityEngine.UIElements\">\n";
    doc += "  <ui:visual-element name=\"root\">\n";
    for (const std::string& raw : items) {
        const ElementDesc e = ParseElement(raw);
        doc += "    <ui:" + e.tag;
        if (!e.id.empty())
            doc += " name=\"" + e.id + "\"";
        if (!e.classes.empty()) {
            doc += " class=\"";
            for (size_t i = 0; i < e.classes.size(); ++i) {
                if (i != 0)
                    doc += " ";
                doc += e.classes[i];
            }
            doc += "\"";
        }
        doc += " />\n";
    }
    doc += "  </ui:visual-element>\n";
    doc += "</ui:UXML>\n";
    return doc;
}

std::string MatchedSelectorsText(const ElementDesc& e, bool hovered) {
    std::string out;
    for (const StyleRule& r : g_sheet.Rules()) {
        if (!r.Matches(e.tag, e.id, e.classes, hovered))
            continue;
        if (!out.empty())
            out += ", ";
        out += r.selector;
    }
    if (out.empty())
        out = "(none)";
    return out;
}

std::string HexOf(ImVec4 c) {
    auto byte = [](float v) {
        int b = static_cast<int>(v * 255.0f + 0.5f);
        if (b < 0)
            b = 0;
        if (b > 255)
            b = 255;
        return b;
    };
    char buf[12];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", byte(c.x), byte(c.y), byte(c.z));
    return std::string(buf);
}

// Key/value list of the properties a Resolve() call produced.
void DrawComputedRows(const ResolvedStyle& s) {
    char buf[96];
    if (!s.anySet) {
        widgets::TextMuted("No matching rules - the element uses inherited defaults.");
        return;
    }
    if (s.hasBackground)
        widgets::KeyValueRow("-unity-background-color", HexOf(s.background).c_str());
    if (s.hasColor)
        widgets::KeyValueRow("-unity-text-color", HexOf(s.color).c_str());
    if (s.hasBorder) {
        std::snprintf(buf, sizeof(buf), "%.1fpx", static_cast<double>(s.borderWidth));
        widgets::KeyValueRow("border-width", buf);
        widgets::KeyValueRow("border-color", HexOf(s.borderColor).c_str());
    }
    if (s.paddingTop != 0.0f || s.paddingRight != 0.0f || s.paddingBottom != 0.0f || s.paddingLeft != 0.0f) {
        std::snprintf(buf, sizeof(buf), "%g %g %g %g", static_cast<double>(s.paddingTop),
                      static_cast<double>(s.paddingRight), static_cast<double>(s.paddingBottom),
                      static_cast<double>(s.paddingLeft));
        widgets::KeyValueRow("padding", buf);
    }
    if (s.marginTop != 0.0f || s.marginRight != 0.0f || s.marginBottom != 0.0f || s.marginLeft != 0.0f) {
        std::snprintf(buf, sizeof(buf), "%g %g %g %g", static_cast<double>(s.marginTop),
                      static_cast<double>(s.marginRight), static_cast<double>(s.marginBottom),
                      static_cast<double>(s.marginLeft));
        widgets::KeyValueRow("margin", buf);
    }
    if (s.borderRadius != 0.0f) {
        std::snprintf(buf, sizeof(buf), "%gpx", static_cast<double>(s.borderRadius));
        widgets::KeyValueRow("border-radius", buf);
    }
    if (s.width != 0.0f) {
        std::snprintf(buf, sizeof(buf), "%gpx", static_cast<double>(s.width));
        widgets::KeyValueRow("width", buf);
    }
    if (s.height != 0.0f) {
        std::snprintf(buf, sizeof(buf), "%gpx", static_cast<double>(s.height));
        widgets::KeyValueRow("height", buf);
    }
    if (s.fontSize != 0.0f) {
        std::snprintf(buf, sizeof(buf), "%gpx", static_cast<double>(s.fontSize));
        widgets::KeyValueRow("font-size", buf);
    }
    if (s.flexGrow != 0.0f) {
        std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(s.flexGrow));
        widgets::KeyValueRow("flex-grow", buf);
    }
    if (s.flexDirection >= 0)
        widgets::KeyValueRow("flex-direction", FlexDirectionName(s.flexDirection));
    if (s.alignItems >= 0)
        widgets::KeyValueRow("align-items", AlignItemsName(s.alignItems));
    if (s.bold || s.italic)
        widgets::KeyValueRow("-unity-font-style", s.bold && s.italic ? "bold-and-italic"
                                                                      : (s.bold ? "bold" : "italic"));
}

void ReloadSheet(AppContext& ctx, WidgetInstance* sheetInst) {
    std::string err;
    g_ussOk = g_sheet.Parse(g_ussText, err);
    g_ussError = err;
    if (sheetInst) {
        // Mirror the parsed selectors into the live-bound state.items list.
        const WidgetState before = sheetInst->state;
        sheetInst->state.items.clear();
        for (const StyleRule& r : g_sheet.Rules())
            sheetInst->state.items.push_back(r.selector);
        ctx.RecordEdit(*sheetInst, "Rule list", before);
    }
    ctx.LogEvent("UI Toolkit", std::string("Style sheet reloaded: ") + std::to_string(g_sheet.RuleCount()) +
                                   " rules, " + std::to_string(g_sheet.PropertyCount()) + " properties, " +
                                   std::to_string(static_cast<int>(g_sheet.Errors().size())) + " errors");
}

// Live flex container: children placed from the state + resolved card style.
void DrawFlexPreview(WidgetState& s, const ResolvedStyle& card, const ThemePalette& pal) {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float width = avail.x;
    const float height = 200.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 boxMin = origin;
    const ImVec2 boxMax(origin.x + width, origin.y + height);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(boxMin, boxMax, ImGui::GetColorU32(ImGuiCol_FrameBg), 6.0f);
    dl->AddRect(boxMin, boxMax, ImGui::GetColorU32(ImGuiCol_Border), 6.0f);

    const ImVec2 innerMin(boxMin.x + std::max(0.0f, s.paddingX), boxMin.y + std::max(0.0f, s.paddingY));
    const ImVec2 innerMax(boxMax.x - std::max(0.0f, s.paddingX), boxMax.y - std::max(0.0f, s.paddingY));
    const bool column = s.flexDirection == 1;
    const float gap = column ? std::max(0.0f, s.spacingY) : std::max(0.0f, s.spacingX);
    const float baseSizes[4] = { 96.0f, 68.0f, 118.0f, 54.0f };

    float cursor = column ? innerMin.y : innerMin.x;
    for (int i = 0; i < 4; ++i) {
        const float mainSize = baseSizes[i] + std::max(0.0f, s.flexGrow) * 24.0f;
        const float crossRoomMin = column ? innerMin.x : innerMin.y;
        const float crossRoomMax = column ? innerMax.x : innerMax.y;
        const float crossRoom = std::max(0.0f, crossRoomMax - crossRoomMin);
        const float baseCross = 40.0f;
        float cross = baseCross;
        float crossPos = crossRoomMin;
        switch (std::clamp(s.alignItems, 0, 3)) {
        case 1: crossPos = crossRoomMin + (crossRoom - cross) * 0.5f; break;
        case 2: crossPos = crossRoomMax - cross; break;
        case 3: cross = crossRoom; break;
        default: break;
        }
        const ImVec2 cMin = column ? ImVec2(crossPos, cursor) : ImVec2(cursor, crossPos);
        const ImVec2 cMax = column ? ImVec2(crossPos + cross, cursor + mainSize)
                                   : ImVec2(cursor + mainSize, crossPos + cross);
        const ImVec4 fill = g_sheet.BackgroundOr(card, ImVec4(pal.accent.x, pal.accent.y, pal.accent.z, 0.55f));
        dl->AddRectFilled(cMin, cMax, ImGui::GetColorU32(fill), card.borderRadius);
        if (card.hasBorder) {
            const float thickness = card.borderWidth > 0.0f ? card.borderWidth : 1.0f;
            dl->AddRect(cMin, cMax, ImGui::GetColorU32(card.borderColor), card.borderRadius, 0, thickness);
        }
        char label[32];
        std::snprintf(label, sizeof(label), "item %d", i + 1);
        const ImVec2 ts = ImGui::CalcTextSize(label);
        const ImVec2 textPos((cMin.x + cMax.x) * 0.5f - ts.x * 0.5f, (cMin.y + cMax.y) * 0.5f - ts.y * 0.5f);
        dl->AddText(textPos, ImGui::GetColorU32(card.hasColor ? card.color : pal.text), label);
        cursor += mainSize + gap;
    }
    ImGui::Dummy(ImVec2(width, height));
}

// A USS-styled template instance used by the Templates section.
void DrawTemplateInstance(AppContext& ctx, int kind, int index, const ResolvedStyle& card) {
    const ThemePalette& pal = ctx.theme.Active();
    const ImVec4 fill = g_sheet.BackgroundOr(card, pal.childBg);
    char id[64];
    std::snprintf(id, sizeof(id), "tpl%d", index);
    switch (kind) {
    case 1: {    // Toolbar
        widgets::BeginCard(id, ImVec2(0.0f, 46.0f), fill, card.borderRadius, card.hasBorder);
        const char* buttons[3] = { "Save", "Open", "Build" };
        for (int b = 0; b < 3; ++b) {
            if (b != 0)
                ImGui::SameLine();
            if (ImGui::Button(buttons[b]))
                ctx.Interaction("Template Toolbar", buttons[b]);
        }
        widgets::EndCard();
        break;
    }
    case 2: {    // Stat Row
        widgets::BeginCard(id, ImVec2(0.0f, 44.0f), fill, card.borderRadius, card.hasBorder);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Stat");
        ImGui::SameLine(80.0f);
        char value[32];
        std::snprintf(value, sizeof(value), "%d", 40 + index * 17);
        widgets::ProgressBarEx("##bar", std::min(1.0f, (40 + index * 17) / 100.0f), ImVec2(0.0f, 16.0f), value,
                               pal.accent);
        widgets::EndCard();
        break;
    }
    case 3: {    // Inventory Slot
        widgets::BeginCard(id, ImVec2(0.0f, 58.0f), fill, card.borderRadius, card.hasBorder);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float slot = 34.0f;
        for (int k = 0; k < 4; ++k) {
            const ImVec2 sMin(pos.x + k * (slot + 6.0f), pos.y);
            const ImVec2 sMax(sMin.x + slot, sMin.y + slot);
            dl->AddRectFilled(sMin, sMax, ImGui::GetColorU32(pal.header), card.borderRadius);
            dl->AddRect(sMin, sMax, ImGui::GetColorU32(card.hasBorder ? card.color : pal.border),
                        card.borderRadius);
            char count[16];
            std::snprintf(count, sizeof(count), "%d", (index + k) % 4 == 0 ? 1 : 3);
            dl->AddText(ImVec2(sMax.x - 14.0f, sMax.y - 16.0f), ImGui::GetColorU32(pal.textDisabled), count);
        }
        ImGui::Dummy(ImVec2(4 * (slot + 6.0f), slot));
        widgets::EndCard();
        break;
    }
    default: {   // Card
        widgets::BeginCard(id, ImVec2(0.0f, 86.0f), fill, card.borderRadius, card.hasBorder);
        ImGui::PushStyleColor(ImGuiCol_Text, card.hasColor ? card.color : pal.text);
        ImGui::TextUnformatted(ctx.demo.blueprintName.c_str());
        ImGui::PopStyleColor();
        widgets::TextMuted("Template instance %d - styled by the live sheet.", index + 1);
        widgets::EndCard();
        break;
    }
    }
}

// One styled element rect (hover tracked through mouse position tests).
void DrawStyledRect(ImVec2 size, const ResolvedStyle& style, ImU32 fallbackFill, ImU32 fallbackText,
                    const char* text, bool* hoveredOut, bool* clickedOut) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 max(pos.x + size.x, pos.y + size.y);
    const bool hovered = ImGui::IsMouseHoveringRect(pos, max);
    const bool clicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    dl->AddRectFilled(pos, max, style.hasBackground ? ImGui::GetColorU32(style.background) : fallbackFill,
                      style.borderRadius);
    if (style.hasBorder) {
        const float thickness = style.borderWidth > 0.0f ? style.borderWidth : 1.0f;
        dl->AddRect(pos, max, ImGui::GetColorU32(style.borderColor), style.borderRadius, 0, thickness);
    }
    if (text && *text) {
        const ImVec2 ts = ImGui::CalcTextSize(text);
        const ImVec2 textPos(pos.x + std::max(4.0f, style.paddingLeft) + 4.0f,
                             pos.y + (size.y - ts.y) * 0.5f);
        dl->AddText(textPos, style.hasColor ? ImGui::GetColorU32(style.color) : fallbackText, text);
    }
    if (hoveredOut)
        *hoveredOut = hovered;
    if (clickedOut)
        *clickedOut = clicked;
    ImGui::Dummy(size);
}

} // namespace

// ---------------------------------------------------------------------------
// Page
// ---------------------------------------------------------------------------
void DrawUIToolkitPage(AppContext& ctx) {
    EnsureSheetSeeded();
    const ImVec4 accent = panels::AccentForCategory(Category::UIToolkit);

    widgets::TextMuted("UXML describes the document, USS describes the look - this page renders both live.");
    ImGui::Separator();

    WidgetInstance* tree = Bind(ctx, "ut.hierarchy.tree");
    WidgetInstance* sheetInst = Bind(ctx, "ut.stylesheet.rules");
    WidgetInstance* classes = Bind(ctx, "ut.classes.theme");
    WidgetInstance* selectors = Bind(ctx, "ut.selectors.list");
    WidgetInstance* flex = Bind(ctx, "ut.flex.container");
    WidgetInstance* templates = Bind(ctx, "ut.templates.list");
    WidgetInstance* preview = Bind(ctx, "ut.preview.panel");

    // ---- (a) Document (UXML) --------------------------------------------
    if (widgets::SectionHeader("Document (UXML)", icons::Icon::File)) {
        if (!tree) {
            widgets::TextMuted("Component ut.hierarchy.tree is missing.");
        } else {
            WidgetState& s = tree->state;
            g_selectedElement = std::clamp(g_selectedElement, 0,
                                           s.items.empty() ? 0 : static_cast<int>(s.items.size()) - 1);
            if (ImGui::BeginTable("##uxml_cols", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoPadInnerX)) {
                ImGui::TableNextColumn();
                ImGui::SeparatorText("Element hierarchy");
                if (ImGui::BeginChild("##utree", ImVec2(0.0f, 130.0f), ImGuiChildFlags_Borders)) {
                    for (int i = 0; i < static_cast<int>(s.items.size()); ++i) {
                        bool dbl = false;
                        if (widgets::ListRow(s.items[i].c_str(), i == g_selectedElement, 0.0f, &dbl)) {
                            g_selectedElement = i;
                            ctx.SelectWidget(tree->id);
                        }
                        if (dbl)
                            ctx.Interaction("Element Hierarchy", "double-click " + s.items[i]);
                    }
                    if (s.items.empty())
                        widgets::TextMuted("(empty document)");
                }
                ImGui::EndChild();

                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Element:");
                ImGui::SameLine();
                if (!s.items.empty()) {
                    ImGui::SetNextItemWidth(-90.0f);
                    if (ImGui::InputText("##elem_edit", &s.items[g_selectedElement]))
                        widgets::TrackEdit(ctx, *tree, "Element");
                    ImGui::SameLine();
                    if (icons::IconButton("##elem_del", icons::Icon::Trash, "Remove element", ImVec2(22.0f, 22.0f))) {
                        const WidgetState before = s;
                        s.items.erase(s.items.begin() + g_selectedElement);
                        ctx.RecordEdit(*tree, "Remove element", before);
                        ctx.Interaction("Element Hierarchy", "removed element");
                    }
                } else {
                    ImGui::TextDisabled("no elements");
                }
                const float addWidth = ImGui::CalcTextSize("Add").x + ImGui::GetStyle().FramePadding.x * 2.0f;
                ImGui::SetNextItemWidth(-(addWidth + ImGui::GetStyle().ItemSpacing.x + 6.0f));
                ImGui::InputText("##new_elem", &g_newElement);
                ImGui::SameLine();
                if (ImGui::Button("Add")) {
                    if (!g_newElement.empty()) {
                        const WidgetState before = s;
                        s.items.push_back(g_newElement);
                        g_selectedElement = static_cast<int>(s.items.size()) - 1;
                        ctx.RecordEdit(*tree, "Add element", before);
                        ctx.Interaction("Element Hierarchy", "added " + g_newElement);
                    }
                }

                ImGui::TableNextColumn();
                WidgetInstance* doc = Bind(ctx, "ut.declarative.document");
                const std::string fileName = doc ? doc->state.text : std::string("panel.uxml");
                ImGui::SeparatorText("Declarative document");
                static std::string docText;
                docText = BuildUxmlDoc(fileName, s.items);
                ImGui::InputTextMultiline("##uxml", &docText, ImVec2(-FLT_MIN, 210.0f),
                                          ImGuiInputTextFlags_ReadOnly);
                if (doc) {
                    widgets::TextMutedWrapped("%s - items live-bind to the Inspector (%d kinds)",
                                              KindName(doc->kind), static_cast<int>(doc->state.items.size()));
                }
                ImGui::EndTable();
            }
        }
    }

    // ---- (b) Style Sheet (USS) -------------------------------------------
    if (widgets::SectionHeader("Style Sheet (USS)", icons::Icon::Brush)) {
        const std::string fileName = sheetInst ? sheetInst->state.text : std::string("main.uss");
        widgets::TextMuted("Sheet: %s - the sheet below is parsed and applied live.", fileName.c_str());
        ImGui::InputTextMultiline("##uss", &g_ussText, ImVec2(-FLT_MIN, 180.0f));
        if (ImGui::IsItemDeactivatedAfterEdit())
            ReloadSheet(ctx, sheetInst);

        if (ImGui::Button("Reload"))
            ReloadSheet(ctx, sheetInst);
        ImGui::SameLine();
        if (ImGui::Button("Apply"))
            ReloadSheet(ctx, sheetInst);
        ImGui::SameLine();
        if (ImGui::Button("Reset To Default")) {
            g_ussText = Sheet::DefaultUss();
            ReloadSheet(ctx, sheetInst);
        }
        ImGui::SameLine();
        const int errorCount = static_cast<int>(g_sheet.Errors().size());
        widgets::Badge(g_ussOk && errorCount == 0 ? "parsed" : "check sheet",
                       g_ussOk && errorCount == 0 ? ctx.theme.Active().success : ctx.theme.Active().error);
        ImGui::SameLine();
        ImGui::TextDisabled("%d rules / %d properties", g_sheet.RuleCount(), g_sheet.PropertyCount());
        if (errorCount > 0) {
            const std::string first = !g_ussError.empty() ? g_ussError : g_sheet.Errors().front();
            ImGui::PushStyleColor(ImGuiCol_Text, ctx.theme.Active().error);
            ImGui::TextWrapped("%s", first.c_str());
            ImGui::PopStyleColor();
        }

        if (ImGui::BeginTable("##uss_rules", 3,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Selector");
            ImGui::TableSetupColumn("Line");
            ImGui::TableSetupColumn("Properties");
            ImGui::TableHeadersRow();
            for (const StyleRule& r : g_sheet.Rules()) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(r.selector.c_str());
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%d", r.sourceLine);
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%d", static_cast<int>(r.properties.size()));
            }
            ImGui::EndTable();
        } else if (g_sheet.Rules().empty()) {
            ImGui::TextDisabled("No rules parsed yet - press Reload.");
        }

        if (sheetInst) {
            widgets::SeparatorText("Selectors bound to the component");
            for (int i = 0; i < static_cast<int>(sheetInst->state.items.size()); ++i) {
                if (i != 0)
                    ImGui::SameLine();
                if (widgets::ChipSelectable(sheetInst->state.items[i].c_str(), false, accent))
                    ctx.Interaction("Style Sheet", "clicked selector " + sheetInst->state.items[i]);
            }
            if (sheetInst->state.items.empty())
                ImGui::TextDisabled("(press Reload to mirror parsed selectors here)");
        }
    }

    // ---- (c) Classes & Selectors ------------------------------------------
    if (widgets::SectionHeader("Classes & Selectors", icons::Icon::Palette)) {
        if (ImGui::BeginTable("##class_cols", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoPadInnerX)) {
            ImGui::TableNextColumn();
            if (classes) {
                widgets::TextMuted("Theme class applied to the preview card:");
                for (int i = 0; i < static_cast<int>(classes->state.items.size()); ++i) {
                    if (i != 0)
                        ImGui::SameLine();
                    const bool selected = classes->state.selectedIndex == i;
                    if (widgets::ChipSelectable(classes->state.items[i].c_str(), selected,
                                                selected ? classes->state.color : accent)) {
                        classes->state.selectedIndex = i;
                        ctx.Interaction("Style Classes", "class " + classes->state.items[i]);
                    }
                }
            } else {
                widgets::TextMuted("Component ut.classes.theme is missing.");
            }
            ImGui::TableNextColumn();
            if (selectors) {
                WidgetState& s = selectors->state;
                if (ImGui::InputText("##selector_text", &s.text))
                    widgets::TrackEdit(ctx, *selectors, "Selector text");
                std::string elementName;
                ElementDesc e;
                if (tree && !tree->state.items.empty() && g_selectedElement < static_cast<int>(tree->state.items.size())) {
                    elementName = tree->state.items[g_selectedElement];
                    e = ParseElement(elementName);
                } else {
                    elementName = "label#title";
                    e = ParseElement(elementName);
                }
                const std::string matched = MatchedSelectorsText(e, g_hoverCard);
                widgets::KeyValueRow("Element", elementName.c_str());
                widgets::KeyValueRow("Matching rules", matched.c_str());
                widgets::KeyValueRow("Current selector", s.text.c_str());
            } else {
                widgets::TextMuted("Component ut.selectors.list is missing.");
            }
            ImGui::EndTable();
        }
    }

    // ---- (d) Flex Layout ---------------------------------------------------
    if (widgets::SectionHeader("Flex Layout", icons::Icon::Layers)) {
        if (!flex) {
            widgets::TextMuted("Component ut.flex.container is missing.");
        } else {
            WidgetState& s = flex->state;
            if (ImGui::BeginTable("##flex_cols", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoPadInnerX)) {
                ImGui::TableNextColumn();
                widgets::KeyValueRow("Container", s.text.c_str());
                int dir = std::clamp(s.flexDirection, 0, 1);
                if (ImGui::Combo("##flexdir", &dir, "Row\0Column\0")) {
                    const WidgetState before = s;
                    s.flexDirection = dir;
                    ctx.RecordEdit(*flex, "Flex direction", before);
                    ctx.Interaction("Flex Layout", std::string("direction ") + FlexDirectionName(dir));
                }
                int align = std::clamp(s.alignItems, 0, 3);
                if (ImGui::Combo("##flexalign", &align, "Start\0Center\0End\0Stretch\0")) {
                    const WidgetState before = s;
                    s.alignItems = align;
                    ctx.RecordEdit(*flex, "Align items", before);
                    ctx.Interaction("Flex Layout", std::string("align ") + AlignItemsName(align));
                }
                if (ImGui::DragFloat2("##gap", &s.spacingX, 0.5f, 0.0f, 64.0f, "Gap x/y %.0f"))
                    widgets::TrackEdit(ctx, *flex, "Gap");
                if (ImGui::DragFloat2("##pad", &s.paddingX, 0.5f, 0.0f, 48.0f, "Padding x/y %.0f"))
                    widgets::TrackEdit(ctx, *flex, "Padding");
                if (ImGui::SliderFloat("##grow", &s.flexGrow, 0.0f, 4.0f, "Flex grow %.2f"))
                    widgets::TrackEdit(ctx, *flex, "Flex grow");
                ImGui::TableNextColumn();
                const ElementDesc cardEl = ParseElement(".card");
                const ResolvedStyle card = g_sheet.Resolve(cardEl.tag, cardEl.id, cardEl.classes, false);
                DrawFlexPreview(s, card, ctx.theme.Active());
                ImGui::EndTable();
            }
        }
    }

    // ---- (e) Templates ------------------------------------------------------
    if (widgets::SectionHeader("Templates", icons::Icon::Copy)) {
        if (!templates) {
            widgets::TextMuted("Component ut.templates.list is missing.");
        } else {
            WidgetState& s = templates->state;
            for (int i = 0; i < static_cast<int>(s.items.size()); ++i) {
                if (i != 0)
                    ImGui::SameLine();
                if (widgets::ChipSelectable(s.items[i].c_str(), s.selectedIndex == i,
                                            s.selectedIndex == i ? s.color : accent)) {
                    s.selectedIndex = i;
                    ctx.Interaction("Templates", "template " + s.items[i]);
                }
            }
            if (ImGui::SliderInt("##copies", &s.intValue, 1, 6, "Instances: %d")) {
                widgets::TrackEdit(ctx, *templates, "Instance count");
                ++ctx.demo.sliderChanges;
            }
            const int copies = std::clamp(s.intValue, 1, 6);
            const int kind = std::clamp(s.selectedIndex, 0, 3);
            const ElementDesc cardEl = ParseElement(".card");
            const ResolvedStyle card = g_sheet.Resolve(cardEl.tag, cardEl.id, cardEl.classes, false);
            if (ImGui::BeginChild("##templates", ImVec2(0.0f, 240.0f), ImGuiChildFlags_Borders)) {
                for (int i = 0; i < copies; ++i)
                    DrawTemplateInstance(ctx, kind, i, card);
            }
            ImGui::EndChild();
        }
    }

    // ---- (f) Preview + computed styles --------------------------------------
    if (widgets::SectionHeader("Preview (computed)", icons::Icon::Eye)) {
        if (!preview) {
            widgets::TextMuted("Component ut.preview.panel is missing.");
        } else if (!preview->state.visible) {
            widgets::TextMuted("Preview is hidden (uncheck Visible in the Inspector).");
        } else {
            WidgetInstance* classes2 = Bind(ctx, "ut.classes.theme");
            const std::string themeClass =
                classes2 ? SafeItem(classes2->state.items, classes2->state.selectedIndex) : std::string("primary");
            const ThemePalette& pal = ctx.theme.Active();
            const ResolvedStyle panel = g_sheet.Resolve("visual-element", "", {"preview-panel"}, false);
            widgets::BeginCard("utpreview", ImVec2(0.0f, 300.0f), g_sheet.BackgroundOr(panel, preview->state.color),
                               8.0f, true);

            const ImU32 defText = ImGui::GetColorU32(pal.text);
            const ImU32 defPanel = ImGui::GetColorU32(ImVec4(pal.header.x, pal.header.y, pal.header.z, 1.0f));

            // #title label (font-size from the sheet, hover flips the :hover rules)
            {
                const std::string title = preview->state.text.empty() ? std::string("Interface Preview")
                                                                      : preview->state.text;
                const ResolvedStyle style = g_sheet.Resolve("label", "title", {}, g_hoverTitle);
                const float fontSize = style.fontSize > 0.0f ? style.fontSize : 18.0f;
                const ImVec2 ts = ImGui::CalcTextSize(title.c_str());
                const ImVec2 size(ts.x + fontSize, ts.y + 16.0f);
                DrawStyledRect(size, style, 0, defText, title.c_str(), &g_hoverTitle, nullptr);
            }
            // .toolbar row
            {
                const float w = ImGui::GetContentRegionAvail().x;
                const ResolvedStyle style = g_sheet.Resolve("", "", {"toolbar"}, g_hoverToolbar);
                const float yBefore = ImGui::GetCursorPosY();
                DrawStyledRect(ImVec2(w, 34.0f), style, defPanel, defText, nullptr, &g_hoverToolbar, nullptr);
                ImGui::SetCursorPosY(yBefore + 6.0f);
                ImGui::SetCursorPosX(std::max(8.0f, style.paddingLeft) + 8.0f);
                const char* toolbarItems[3] = { "Save", "Open", "Build" };
                for (int i = 0; i < 3; ++i) {
                    if (i != 0)
                        ImGui::SameLine();
                    if (ImGui::SmallButton(toolbarItems[i]))
                        ctx.Interaction("Toolbar", toolbarItems[i]);
                }
                ImGui::Dummy(ImVec2(0.0f, 16.0f));
            }
            // .card + theme class
            {
                const float w = ImGui::GetContentRegionAvail().x;
                std::vector<std::string> cardClasses{ "card" };
                if (!themeClass.empty())
                    cardClasses.push_back(themeClass);
                const ResolvedStyle style = g_sheet.Resolve("", "", cardClasses, g_hoverCard);
                const std::string label = "card ." + themeClass;
                DrawStyledRect(ImVec2(w, 86.0f), style, defPanel, defText, label.c_str(), &g_hoverCard, nullptr);
            }
            // button.primary
            {
                const ResolvedStyle style = g_sheet.Resolve("button", "", {"primary"}, g_hoverButton);
                bool clicked = false;
                DrawStyledRect(ImVec2(170.0f, 30.0f), style, ImGui::GetColorU32(pal.button), defText,
                               "Primary Action", &g_hoverButton, &clicked);
                if (clicked) {
                    ++ctx.demo.buttonClicks;
                    ctx.Interaction("Preview Button", "pressed");
                    ctx.Notify(ToastKind::Info, "button.clicked", "Handled in the UI Toolkit preview.");
                }
            }
            widgets::EndCard();

            // computed styles of the selected document element
            std::string elementName = "label#title";
            if (tree && !tree->state.items.empty() &&
                g_selectedElement < static_cast<int>(tree->state.items.size()))
                elementName = tree->state.items[g_selectedElement];
            const ElementDesc e = ParseElement(elementName);
            const ResolvedStyle r = g_sheet.Resolve(e.tag, e.id, e.classes, g_hoverCard);
            if (ImGui::BeginTable("##computed_cols", 2,
                                  ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoPadInnerX)) {
                ImGui::TableNextColumn();
                ImGui::SeparatorText("Computed styles");
                widgets::KeyValueRow("Element", elementName.c_str());
                widgets::KeyValueRow("Matched selectors", MatchedSelectorsText(e, g_hoverCard).c_str());
                const std::string summary = r.Describe();
                widgets::KeyValueRow("Resolved", summary.c_str());
                DrawComputedRows(r);
                ImGui::TableNextColumn();
                ImGui::SeparatorText("Sheet status");
                widgets::KeyValueRow("Rules", std::to_string(g_sheet.RuleCount()).c_str());
                widgets::KeyValueRow("Properties", std::to_string(g_sheet.PropertyCount()).c_str());
                const std::string errText = g_ussOk ? "(ok)" : (g_ussError.empty() ? "unknown error" : g_ussError);
                widgets::KeyValueRow("Parse", errText.c_str());
                widgets::TextMuted("Theme class: %s - hover the preview card to evaluate .card:hover.",
                                   themeClass.c_str());
                ImGui::EndTable();
            }
        }
    }

    DrawUnboundList(ctx, Category::UIToolkit);
}

// ---------------------------------------------------------------------------
// Floating "computed styles" inspector
// ---------------------------------------------------------------------------
void DrawUiToolkitInspectorWindow(AppContext& ctx) {
    if (!ctx.showUiToolkitInspectorWindow)
        return;
    EnsureSheetSeeded();

    ImGui::SetNextWindowSize(ImVec2(380.0f, 440.0f), ImGuiCond_FirstUseEver);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDocking;
    if (!ImGui::Begin("UI Toolkit Inspector", &ctx.showUiToolkitInspectorWindow, flags)) {
        ImGui::End();
        return;
    }

    const WidgetInstance* sel = ctx.SelectedWidget();
    if (!sel) {
        widgets::TextMuted("Select a component to see its computed USS style.");
        ImGui::End();
        return;
    }

    const ThemePalette& pal = ctx.theme.Active();
    const ImVec4 accent = panels::AccentForCategory(sel->category);
    ImGui::PushStyleColor(ImGuiCol_Text, accent);
    icons::IconText(panels::IconForKind(sel->kind), KindName(sel->kind), ImGui::GetColorU32(ImGuiCol_Text));
    ImGui::PopStyleColor();
    ImGui::SameLine();
    widgets::Badge(sel->liveBound ? "live" : "unbound", sel->liveBound ? accent : pal.textDisabled);
    ImGui::TextDisabled("%s", sel->displayName.c_str());
    ImGui::Separator();

    // Build the element descriptor from the state's style fields.
    std::string tag = "visual-element";
    switch (sel->kind) {
    case WidgetKind::Button:
    case WidgetKind::ToggleButton:
        tag = "button";
        break;
    case WidgetKind::TextInput:
    case WidgetKind::MultilineInput:
    case WidgetKind::SearchField:
        tag = "label";
        break;
    default:
        break;
    }
    std::vector<std::string> classes;
    if (!sel->state.styleClass.empty() && sel->state.styleClass != "default")
        classes.push_back(sel->state.styleClass);
    const std::string id = sel->state.styleId;

    const ResolvedStyle r = g_sheet.Resolve(tag, id, classes, false);
    widgets::KeyValueRow("Tag", tag.c_str());
    widgets::KeyValueRow("name (id)", id.empty() ? "(none)" : id.c_str());
    std::string classText;
    for (const std::string& c : classes) {
        if (!classText.empty())
            classText += ", ";
        classText += "." + c;
    }
    if (classText.empty())
        classText = "(no classes)";
    widgets::KeyValueRow("Classes", classText.c_str());
    ImGui::SeparatorText("Matched selectors");
    int matched = 0;
    for (const StyleRule& rule : g_sheet.Rules()) {
        if (!rule.Matches(tag, id, classes, false))
            continue;
        ++matched;
        ImGui::TextUnformatted(rule.selector.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("line %d, %d props", rule.sourceLine, static_cast<int>(rule.properties.size()));
    }
    if (matched == 0)
        widgets::TextMuted("No rule matches - edit Style class / Style id in the Inspector.");

    ImGui::SeparatorText("Computed values");
    const std::string summary = r.Describe();
    widgets::KeyValueRow("Resolved", summary.c_str());
    DrawComputedRows(r);

    ImGui::Separator();
    if (ImGui::Button("Select On Demo Page")) {
        ctx.SelectCategory(Category::UIToolkit);
        ctx.SelectWidget(sel->id);
    }
    ImGui::SameLine();
    if (ImGui::Button("Close"))
        ctx.showUiToolkitInspectorWindow = false;

    ImGui::End();
}

} // namespace demo
} // namespace uilab
