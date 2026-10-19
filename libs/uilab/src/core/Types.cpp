// Types.cpp - tables and helpers for the shared component model.
#include "core/Types.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>

namespace uilab {

// ---------------------------------------------------------------------------
// Categories
// ---------------------------------------------------------------------------
namespace {
const char* kCategoryNames[static_cast<int>(Category::Count)] = {
    "Basic Components", "Layout & Structure", "Slate (Inspiration)", "UMG (Inspiration)",
    "UI Toolkit (Inspiration)", "IMGUI (Inspiration)", "uGUI (Inspiration)"
};
const char* kCategoryShort[static_cast<int>(Category::Count)] = {
    "Basic", "Layout", "Slate", "UMG", "UI Toolkit", "IMGUI", "uGUI"
};
const char* kCategoryBlurb[static_cast<int>(Category::Count)] = {
    "Buttons, inputs, lists, trees, tabs, pickers, dialogs and notifications.",
    "Boxes, grids, stacks, splitters, collapsible and responsive containers.",
    "Editor style panels: hierarchy, details, asset browser, toolbars and context menus.",
    "Blueprint authoring concepts: canvas, overlay, anchors, z-order, transforms and HUD.",
    "Declarative elements, a USS/CSS inspired stylesheet, selectors and flex layout.",
    "Immediate mode windows, conditional widgets and per-frame generated controls.",
    "Canvas, RectTransform, pivot, layout groups, content fitter, scroll rect and HUD."
};
} // namespace

const char* CategoryName(Category c) {
    const int i = static_cast<int>(c);
    return (i >= 0 && i < static_cast<int>(Category::Count)) ? kCategoryNames[i] : "Unknown";
}
const char* CategoryShortName(Category c) {
    const int i = static_cast<int>(c);
    return (i >= 0 && i < static_cast<int>(Category::Count)) ? kCategoryShort[i] : "Unknown";
}
const char* CategoryBlurb(Category c) {
    const int i = static_cast<int>(c);
    return (i >= 0 && i < static_cast<int>(Category::Count)) ? kCategoryBlurb[i] : "";
}

// ---------------------------------------------------------------------------
// Kinds
// ---------------------------------------------------------------------------
namespace {
struct KindEntry { WidgetKind kind; const char* name; };

const KindEntry kKindTable[] = {
    { WidgetKind::Button, "Button" },
    { WidgetKind::ToggleButton, "Toggle Button" },
    { WidgetKind::Checkbox, "Checkbox" },
    { WidgetKind::RadioButton, "Radio Button" },
    { WidgetKind::SliderHorizontal, "Slider (Horizontal)" },
    { WidgetKind::SliderVertical, "Slider (Vertical)" },
    { WidgetKind::ProgressBar, "Progress Bar" },
    { WidgetKind::TextInput, "Text Input" },
    { WidgetKind::MultilineInput, "Multiline Input" },
    { WidgetKind::Combo, "Dropdown (Combo)" },
    { WidgetKind::ListBox, "List Box" },
    { WidgetKind::TreeView, "Tree View" },
    { WidgetKind::Tabs, "Tabs" },
    { WidgetKind::TooltipButton, "Tooltip" },
    { WidgetKind::ContextMenuArea, "Context Menu" },
    { WidgetKind::MenuBarSample, "Menu Bar" },
    { WidgetKind::ToolbarSample, "Toolbar" },
    { WidgetKind::SeparatorLine, "Separator" },
    { WidgetKind::ScrollView, "Scroll View" },
    { WidgetKind::ImageView, "Image View" },
    { WidgetKind::ColorPicker, "Color Picker" },
    { WidgetKind::NumericInput, "Numeric Input" },
    { WidgetKind::SearchField, "Search Field" },
    { WidgetKind::ModalDialogTrigger, "Modal Dialog" },
    { WidgetKind::ToastTrigger, "Notification / Toast" },

    { WidgetKind::VerticalBox, "Vertical Box" },
    { WidgetKind::HorizontalBox, "Horizontal Box" },
    { WidgetKind::GridLayout, "Grid Layout" },
    { WidgetKind::StackLayout, "Stack Layout" },
    { WidgetKind::ScrollPanel, "Scrollable Panel" },
    { WidgetKind::CollapsiblePanel, "Collapsible Panel" },
    { WidgetKind::Splitter, "Splitter" },
    { WidgetKind::DockableWindowTrigger, "Dockable Window" },
    { WidgetKind::NestedPanels, "Nested Panels" },
    { WidgetKind::ResponsivePanel, "Responsive Layout" },
    { WidgetKind::AnchorsPanel, "Anchors & Alignment" },
    { WidgetKind::SpacingControls, "Margins & Spacing" },

    { WidgetKind::SlateWindowTrigger, "Slate Window" },
    { WidgetKind::SlateDetails, "Property Details Panel" },
    { WidgetKind::SlateAssetBrowser, "Asset Browser (Slate)" },
    { WidgetKind::SlateHierarchy, "Hierarchy Panel" },
    { WidgetKind::SlateSearchTree, "Searchable Tree" },
    { WidgetKind::SlateContextMenu, "Slate Context Menu" },
    { WidgetKind::SlateToolbar, "Command Toolbar" },

    { WidgetKind::UmgCanvas, "Canvas Panel" },
    { WidgetKind::UmgOverlay, "Overlay Panel" },
    { WidgetKind::UmgWidgetContainer, "Widget Container" },
    { WidgetKind::UmgAnchors, "Anchor & Alignment" },
    { WidgetKind::UmgZOrder, "Z-Order" },
    { WidgetKind::UmgTransform, "Widget Transform" },
    { WidgetKind::UmgProgress, "Progress Bar (UMG)" },
    { WidgetKind::UmgHud, "HUD" },
    { WidgetKind::UmgPreview, "Interface Preview" },

    { WidgetKind::UtHierarchy, "Element Hierarchy" },
    { WidgetKind::UtStyleSheet, "Style Sheet" },
    { WidgetKind::UtClasses, "Style Classes & Themes" },
    { WidgetKind::UtSelectors, "Selectors" },
    { WidgetKind::UtDeclarative, "Declarative Elements" },
    { WidgetKind::UtFlex, "Flex Layout" },
    { WidgetKind::UtTemplates, "Component Templates" },
    { WidgetKind::UtPreview, "Interface Preview (Toolkit)" },

    { WidgetKind::ImWindowTrigger, "Immediate Mode Window" },
    { WidgetKind::ImConditional, "Conditional Widgets" },
    { WidgetKind::ImDynamicControls, "Dynamic Controls" },
    { WidgetKind::ImDebugPanel, "Debug Panel" },
    { WidgetKind::ImStats, "Real-time Statistics" },
    { WidgetKind::ImUpdate, "Per-frame Update" },

    { WidgetKind::UgCanvas, "Canvas (uGUI)" },
    { WidgetKind::UgRectTransform, "Rect Transform" },
    { WidgetKind::UgAnchors, "Anchors (uGUI)" },
    { WidgetKind::UgPivot, "Pivot" },
    { WidgetKind::UgLayoutGroup, "Layout Group" },
    { WidgetKind::UgContentFitter, "Content Size Fitter" },
    { WidgetKind::UgScrollRect, "Scroll Rect" },
    { WidgetKind::UgHud, "HUD (Health / Energy / Score)" },
};
} // namespace

const char* KindName(WidgetKind k) {
    const int i = static_cast<int>(k);
    if (i < 0 || i >= static_cast<int>(WidgetKind::Count))
        return "Unknown";
    return kKindTable[i].name;
}

WidgetKind KindFromName(const std::string& name) {
    for (const KindEntry& e : kKindTable)
        if (name == e.name)
            return e.kind;
    return WidgetKind::Count;
}

WidgetTraits TraitsForKind(WidgetKind k) {
    WidgetTraits t;
    switch (k) {
    case WidgetKind::Button:
        t.text = true; t.color = true; t.transform = true;
        break;
    case WidgetKind::ToggleButton:
        t.text = true; t.toggled = true; t.color = true;
        break;
    case WidgetKind::Checkbox:
        t.text = true; t.toggled = true;
        break;
    case WidgetKind::RadioButton:
        t.text = true; t.items = true;
        break;
    case WidgetKind::SliderHorizontal:
    case WidgetKind::SliderVertical:
        t.text = true; t.value = true; t.color = true;
        break;
    case WidgetKind::ProgressBar:
        t.text = true; t.value = true; t.color = true;
        break;
    case WidgetKind::TextInput:
        t.text = true; t.validation = true;
        break;
    case WidgetKind::MultilineInput:
        t.text = true; t.validation = true;
        break;
    case WidgetKind::Combo:
    case WidgetKind::ListBox:
        t.text = true; t.items = true;
        break;
    case WidgetKind::TreeView:
    case WidgetKind::Tabs:
        t.text = true; t.items = true;
        break;
    case WidgetKind::TooltipButton:
        t.text = true; t.validation = true; // reuse: tooltip + hint fields
        break;
    case WidgetKind::ContextMenuArea:
        t.text = true; t.items = true;
        break;
    case WidgetKind::MenuBarSample:
    case WidgetKind::ToolbarSample:
        t.text = true; t.items = true; t.spacing = true;
        break;
    case WidgetKind::SeparatorLine:
        t.color = true; t.spacing = true;
        break;
    case WidgetKind::ScrollView:
    case WidgetKind::ScrollPanel:
        t.text = true; t.integer = true;
        break;
    case WidgetKind::ImageView:
        t.text = true; t.color = true; t.transform = true;
        break;
    case WidgetKind::ColorPicker:
        t.text = true; t.color = true;
        break;
    case WidgetKind::NumericInput:
        t.text = true; t.value = true; t.integer = true;
        break;
    case WidgetKind::SearchField:
        t.text = true; t.items = true;
        break;
    case WidgetKind::ModalDialogTrigger:
    case WidgetKind::ToastTrigger:
        t.text = true; t.validation = true;
        break;

    case WidgetKind::VerticalBox:
    case WidgetKind::HorizontalBox:
        t.spacing = true; t.transform = true;
        break;
    case WidgetKind::GridLayout:
        t.integer = true; t.spacing = true;
        break;
    case WidgetKind::StackLayout:
        t.spacing = true; t.color = true;
        break;
    case WidgetKind::CollapsiblePanel:
    case WidgetKind::NestedPanels:
        t.text = true; t.spacing = true;
        break;
    case WidgetKind::Splitter:
        t.value = true; t.transform = true;
        break;
    case WidgetKind::DockableWindowTrigger:
        t.text = true; t.transform = true;
        break;
    case WidgetKind::ResponsivePanel:
        t.integer = true; t.spacing = true; t.text = true;
        break;
    case WidgetKind::AnchorsPanel:
        t.transform = true; t.anchors = true; t.color = true;
        break;
    case WidgetKind::SpacingControls:
        t.spacing = true;
        break;

    case WidgetKind::SlateWindowTrigger:
        t.text = true; t.transform = true; t.toggled = true;
        break;
    case WidgetKind::SlateDetails:
    case WidgetKind::SlateHierarchy:
    case WidgetKind::SlateSearchTree:
    case WidgetKind::SlateAssetBrowser:
        t.text = true; t.items = true;
        break;
    case WidgetKind::SlateContextMenu:
        t.items = true; t.text = true;
        break;
    case WidgetKind::SlateToolbar:
        t.text = true; t.items = true; t.spacing = true;
        break;

    case WidgetKind::UmgCanvas:
    case WidgetKind::UmgWidgetContainer:
        t.transform = true; t.color = true; t.anchors = true;
        break;
    case WidgetKind::UmgOverlay:
        t.spacing = true; t.transform = true; t.color = true;
        break;
    case WidgetKind::UmgAnchors:
        t.anchors = true; t.transform = true;
        break;
    case WidgetKind::UmgZOrder:
        t.transform = true; t.integer = true;
        break;
    case WidgetKind::UmgTransform:
        t.transform = true; t.color = true;
        break;
    case WidgetKind::UmgProgress:
        t.value = true; t.color = true; t.text = true;
        break;
    case WidgetKind::UmgHud:
    case WidgetKind::UmgPreview:
        t.color = true; t.text = true; t.value = true;
        break;

    case WidgetKind::UtHierarchy:
        t.text = true; t.items = true;
        break;
    case WidgetKind::UtStyleSheet:
        t.items = true; t.color = true;
        break;
    case WidgetKind::UtClasses:
        t.text = true; t.items = true; t.color = true;
        break;
    case WidgetKind::UtSelectors:
        t.text = true; t.color = true;
        break;
    case WidgetKind::UtDeclarative:
        t.text = true; t.items = true;
        break;
    case WidgetKind::UtFlex:
        t.spacing = true; t.transform = true;
        break;
    case WidgetKind::UtTemplates:
        t.text = true; t.items = true;
        break;
    case WidgetKind::UtPreview:
        t.color = true; t.text = true;
        break;

    case WidgetKind::ImWindowTrigger:
        t.text = true; t.toggled = true;
        break;
    case WidgetKind::ImConditional:
        t.items = true; t.toggled = true; t.text = true;
        break;
    case WidgetKind::ImDynamicControls:
        t.integer = true; t.items = true;
        break;
    case WidgetKind::ImDebugPanel:
        t.items = true; t.color = true;
        break;
    case WidgetKind::ImStats:
        t.value = true; t.color = true;
        break;
    case WidgetKind::ImUpdate:
        t.value = true; t.text = true;
        break;

    case WidgetKind::UgCanvas:
        t.color = true; t.transform = true; t.spacing = true;
        break;
    case WidgetKind::UgRectTransform:
    case WidgetKind::UgPivot:
    case WidgetKind::UgAnchors:
        t.transform = true; t.anchors = true;
        break;
    case WidgetKind::UgLayoutGroup:
        t.spacing = true; t.integer = true; t.toggled = true;
        break;
    case WidgetKind::UgContentFitter:
        t.toggled = true; t.spacing = true; t.text = true;
        break;
    case WidgetKind::UgScrollRect:
        t.value = true; t.transform = true; t.toggled = true;
        break;
    case WidgetKind::UgHud:
        t.value = true; t.color = true; t.text = true;
        break;
    default:
        break;
    }
    return t;
}

// ---------------------------------------------------------------------------
// Anchors / alignment
// ---------------------------------------------------------------------------
namespace {
const char* kAnchorNames[static_cast<int>(AnchorPreset::Count)] = {
    "Top Left", "Top Center", "Top Right",
    "Middle Left", "Middle Center", "Middle Right",
    "Bottom Left", "Bottom Center", "Bottom Right",
    "Stretch"
};
const char* kAlignNames[3] = { "Left", "Center", "Right" };
const char* kFlexNames[2] = { "Row", "Column" };
const char* kAlignItems[4] = { "Start", "Center", "End", "Stretch" };
} // namespace

const char* AnchorName(AnchorPreset a) {
    const int i = static_cast<int>(a);
    return (i >= 0 && i < static_cast<int>(AnchorPreset::Count)) ? kAnchorNames[i] : "?";
}

ImVec2 AnchorVec(AnchorPreset a) {
    switch (a) {
    case AnchorPreset::TopLeft:      return ImVec2(0.0f, 0.0f);
    case AnchorPreset::TopCenter:    return ImVec2(0.5f, 0.0f);
    case AnchorPreset::TopRight:     return ImVec2(1.0f, 0.0f);
    case AnchorPreset::MiddleLeft:   return ImVec2(0.0f, 0.5f);
    case AnchorPreset::MiddleCenter: return ImVec2(0.5f, 0.5f);
    case AnchorPreset::MiddleRight:  return ImVec2(1.0f, 0.5f);
    case AnchorPreset::BottomLeft:   return ImVec2(0.0f, 1.0f);
    case AnchorPreset::BottomCenter: return ImVec2(0.5f, 1.0f);
    case AnchorPreset::BottomRight:  return ImVec2(1.0f, 1.0f);
    case AnchorPreset::Stretch:
    default:                         return ImVec2(0.5f, 0.5f);
    }
}

bool IsStretch(AnchorPreset a) { return a == AnchorPreset::Stretch; }

const char* AlignName(int index) { return kAlignNames[(index < 0 || index > 2) ? 1 : index]; }
const char* FlexDirectionName(int index) { return kFlexNames[index == 0 ? 0 : 1]; }
const char* AlignItemsName(int index) { return kAlignItems[(index < 0 || index > 3) ? 0 : index]; }

const WidgetTraits& WidgetInstance::Traits() const {
    static WidgetTraits cache[static_cast<int>(WidgetKind::Count)];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < static_cast<int>(WidgetKind::Count); ++i)
            cache[i] = TraitsForKind(static_cast<WidgetKind>(i));
        init = true;
    }
    const int i = static_cast<int>(kind);
    if (i < 0 || i >= static_cast<int>(WidgetKind::Count)) {
        static WidgetTraits empty;
        return empty;
    }
    return cache[i];
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool ContainsNoCase(const std::string& haystack, const std::string& needle) {
    if (needle.empty())
        return true;
    return ToLower(haystack).find(ToLower(needle)) != std::string::npos;
}

std::string FormatFloat(float v, const char* fmt) {
    char buf[64] = {};
    std::snprintf(buf, sizeof(buf), (fmt && *fmt) ? fmt : "%.2f", static_cast<double>(v));
    return std::string(buf);
}

ImVec4 WithAlpha(const ImVec4& c, float a) { return ImVec4(c.x, c.y, c.z, a); }

ImVec4 LerpColor(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                  a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

const char* BoolLabel(bool v) { return v ? "true" : "false"; }

} // namespace uilab
