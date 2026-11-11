// Types.h - shared enums, the widget property model and small helpers.
//
// The "widget instance" model is the backbone of the lab: every component shown
// in the tree on the left owns a WidgetState, the Inspector edits that state and
// the demo pages render *from* it, which is what makes the inspector changes
// visible immediately in the preview.
#pragma once

#include <string>
#include <vector>

#include <imgui.h>

namespace uilab {

// ---------------------------------------------------------------------------
// Categories (one per UI paradigm demonstrated by the lab)
// ---------------------------------------------------------------------------
enum class Category : int {
    Basic = 0,   // common widgets shared by every toolkit
    Layout,      // boxes, grids, splitters, docking
    Slate,       // Unreal Slate / UMG editor style panels
    UMG,         // Unreal UMG widget blueprints
    UIToolkit,   // Unity UI Toolkit (UXML/USS like)
    Immediate,   // Unity IMGUI (immediate mode)
    UGui,        // Unity uGUI (Canvas / RectTransform)
    Count
};

const char* CategoryName(Category c);
const char* CategoryShortName(Category c);
const char* CategoryBlurb(Category c);

// ---------------------------------------------------------------------------
// Widget kinds - used for the trait table that drives the Inspector.
// ---------------------------------------------------------------------------
enum class WidgetKind : int {
    // basic
    Button = 0, ToggleButton, Checkbox, RadioButton, SliderHorizontal, SliderVertical, ProgressBar,
    TextInput, MultilineInput, Combo, ListBox, TreeView, Tabs, TooltipButton, ContextMenuArea, MenuBarSample,
    ToolbarSample, SeparatorLine, ScrollView, ImageView, ColorPicker, NumericInput, SearchField,
    ModalDialogTrigger, ToastTrigger,
    // layout
    VerticalBox, HorizontalBox, GridLayout, StackLayout, ScrollPanel, CollapsiblePanel, Splitter,
    DockableWindowTrigger, NestedPanels, ResponsivePanel, AnchorsPanel, SpacingControls,
    // slate
    SlateWindowTrigger, SlateDetails, SlateAssetBrowser, SlateHierarchy, SlateSearchTree, SlateContextMenu, SlateToolbar,
    // umg
    UmgCanvas, UmgOverlay, UmgWidgetContainer, UmgAnchors, UmgZOrder, UmgTransform, UmgProgress, UmgHud, UmgPreview,
    // ui toolkit
    UtHierarchy, UtStyleSheet, UtClasses, UtSelectors, UtDeclarative, UtFlex, UtTemplates, UtPreview,
    // immediate
    ImWindowTrigger, ImConditional, ImDynamicControls, ImDebugPanel, ImStats, ImUpdate,
    // ugui
    UgCanvas, UgRectTransform, UgAnchors, UgPivot, UgLayoutGroup, UgContentFitter, UgScrollRect, UgHud,
    Count
};

const char* KindName(WidgetKind k);
WidgetKind KindFromName(const std::string& name);

// Which property groups the Inspector should present for a kind.
struct WidgetTraits {
    bool text = false;        // label / content
    bool value = false;       // float value + range
    bool integer = false;     // int value + range
    bool items = false;       // item list + selection index
    bool color = false;       // primary/secondary colour
    bool transform = false;   // position/size/scale/rotation/opacity/z-order
    bool anchors = false;     // anchor / pivot / alignment presets
    bool spacing = false;     // margin / padding / spacing / flex
    bool validation = false;  // read-only + validation rule
    bool toggled = false;     // checked / toggled state
};

WidgetTraits TraitsForKind(WidgetKind k);

// ---------------------------------------------------------------------------
// Anchors / alignment
// ---------------------------------------------------------------------------
enum class AnchorPreset : int {
    TopLeft = 0, TopCenter, TopRight,
    MiddleLeft, MiddleCenter, MiddleRight,
    BottomLeft, BottomCenter, BottomRight,
    Stretch,
    Count
};

const char* AnchorName(AnchorPreset a);
// Normalised anchor position (0..1). Stretch maps to the centre (0.5, 0.5).
ImVec2 AnchorVec(AnchorPreset a);
bool IsStretch(AnchorPreset a);

const char* AlignName(int index);          // 0 = left, 1 = center, 2 = right
const char* FlexDirectionName(int index);  // 0 = row, 1 = column
const char* AlignItemsName(int index);     // 0 = start, 1 = center, 2 = end, 3 = stretch

// ---------------------------------------------------------------------------
// Editable property bag of a component
// ---------------------------------------------------------------------------
struct WidgetState {
    // content / identity
    std::string text = "Component";
    std::string tooltip = "Tooltip text";
    std::string styleClass = "default";
    std::string styleId;

    // state flags
    bool visible = true;
    bool enabled = true;
    bool readOnly = false;
    bool checked = false;
    bool toggled = false;

    // numeric values
    float value = 0.5f;
    float minValue = 0.0f;
    float maxValue = 1.0f;
    float step = 0.01f;
    int intValue = 50;
    int intMin = 0;
    int intMax = 100;
    std::string format = "%.2f";
    int selectedIndex = 0;
    std::vector<std::string> items = { "Item A", "Item B", "Item C" };

    // colours
    ImVec4 color = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    ImVec4 color2 = ImVec4(0.10f, 0.12f, 0.15f, 1.00f);
    bool useCustomColor = false;

    // transform (canvas widgets / preview)
    ImVec2 position = ImVec2(24.0f, 24.0f);   // anchored position (pixels)
    ImVec2 size = ImVec2(200.0f, 40.0f);      // size delta (pixels)
    ImVec2 scale = ImVec2(1.0f, 1.0f);
    float rotation = 0.0f;                    // degrees
    float opacity = 1.0f;
    int zOrder = 0;
    int anchorMin = static_cast<int>(AnchorPreset::TopLeft);
    int anchorMax = static_cast<int>(AnchorPreset::TopLeft);
    int pivot = static_cast<int>(AnchorPreset::MiddleCenter);
    int alignment = 1;

    // spacing / flex
    float spacingX = 8.0f;
    float spacingY = 8.0f;
    float paddingX = 8.0f;
    float paddingY = 8.0f;
    float marginX = 0.0f;
    float marginY = 0.0f;
    int flexDirection = 1;   // 0 = row, 1 = column
    float flexGrow = 0.0f;
    int alignItems = 0;      // see AlignItemsName

    // validation
    std::string validationRule = "none";   // none|nonempty|integer|float|minlength|pattern|range
    std::string pattern = "[A-Za-z0-9_]+";
    std::string hint = "Hint text";
    std::string validationMessage;         // filled by the inspector validation feedback

    // free text areas used by the multiline demo
    std::string multilineText =
        "Multiline input sample.\n"
        "Edit the text directly or through the Inspector.\n"
        "Every change is reported in the Events panel.";
};

// ---------------------------------------------------------------------------
// A component instance living in the registry
// ---------------------------------------------------------------------------
struct WidgetInstance {
    std::string id;              // stable, unique, used as ImGui id too
    std::string displayName;     // editable by the user
    WidgetKind kind = WidgetKind::Button;
    Category category = Category::Basic;
    std::string group;           // sub-group inside the category tree
    WidgetState state;
    bool liveBound = false;      // true when a demo page renders from this state

    const char* KindLabel() const { return KindName(kind); }
    const WidgetTraits& Traits() const;
};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
std::string ToLower(std::string s);
bool ContainsNoCase(const std::string& haystack, const std::string& needle);
std::string FormatFloat(float v, const char* fmt);
ImVec4 WithAlpha(const ImVec4& c, float a);
ImVec4 LerpColor(const ImVec4& a, const ImVec4& b, float t);
const char* BoolLabel(bool v);

} // namespace uilab
