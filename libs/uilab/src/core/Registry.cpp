#include "core/Registry.h"

#include "core/Validation.h"

#include <utility>

namespace uilab {

WidgetInstance& WidgetRegistry::Add(WidgetInstance instance) {
    if (instance.id.empty())
        instance.id = MakeUniqueId(instance.displayName);
    auto owned = std::make_unique<WidgetInstance>(std::move(instance));
    WidgetInstance* raw = owned.get();
    items_.push_back(std::move(owned));
    byId_[raw->id] = raw;
    return *raw;
}

WidgetInstance* WidgetRegistry::Find(std::string_view id) const {
    const auto it = byId_.find(std::string(id));
    return it == byId_.end() ? nullptr : it->second;
}

bool WidgetRegistry::Remove(std::string_view id) {
    const auto it = byId_.find(std::string(id));
    if (it == byId_.end())
        return false;
    WidgetInstance* target = it->second;
    byId_.erase(it);
    for (auto vit = items_.begin(); vit != items_.end(); ++vit) {
        if (vit->get() == target) {
            items_.erase(vit);
            return true;
        }
    }
    return false;
}

bool WidgetRegistry::Rename(std::string_view id, std::string newDisplayName, std::string& errorOut) {
    WidgetInstance* inst = Find(id);
    if (!inst) {
        errorOut = "Component not found.";
        return false;
    }
    if (!ValidateText(newDisplayName, "nonempty", errorOut))
        return false;
    inst->displayName = std::move(newDisplayName);
    errorOut.clear();
    return true;
}

int WidgetRegistry::CountCategory(Category c) const {
    int n = 0;
    for (const auto& item : items_)
        if (item->category == c)
            ++n;
    return n;
}

int WidgetRegistry::CountLive() const {
    int n = 0;
    for (const auto& item : items_)
        if (item->liveBound)
            ++n;
    return n;
}

std::string WidgetRegistry::MakeUniqueId(const std::string& base) const {
    std::string candidate = base.empty() ? "widget" : base;
    int suffix = 1;
    while (byId_.find(candidate) != byId_.end())
        candidate = base + "_" + std::to_string(++suffix);
    return candidate;
}

std::vector<std::string> WidgetRegistry::GroupsOf(Category c) const {
    std::vector<std::string> out;
    for (const auto& item : items_) {
        if (item->category != c)
            continue;
        bool found = false;
        for (const std::string& g : out)
            if (g == item->group) {
                found = true;
                break;
            }
        if (!found)
            out.push_back(item->group);
    }
    return out;
}

void WidgetRegistry::ClearState() {
    for (auto& item : items_)
        item->liveBound = true;
}

// ---------------------------------------------------------------------------
// Default component set
// ---------------------------------------------------------------------------
namespace {

WidgetInstance Make(const char* id, const char* name, WidgetKind kind, Category category, const char* group) {
    WidgetInstance w;
    w.id = id;
    w.displayName = name;
    w.kind = kind;
    w.category = category;
    w.group = group;
    w.liveBound = true;
    return w;
}

std::vector<std::string> Vec(std::initializer_list<const char*> items) {
    std::vector<std::string> out;
    out.reserve(items.size());
    for (const char* s : items)
        out.push_back(s);
    return out;
}

const ImVec4 kAccent = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
const ImVec4 kGreen = ImVec4(0.33f, 0.78f, 0.42f, 1.00f);
const ImVec4 kOrange = ImVec4(0.95f, 0.62f, 0.24f, 1.00f);
const ImVec4 kViolet = ImVec4(0.62f, 0.47f, 0.96f, 1.00f);
const ImVec4 kPanel = ImVec4(0.16f, 0.18f, 0.22f, 1.00f);

} // namespace

void BuildDefaultRegistry(WidgetRegistry& registry) {
    auto add = [&registry](const char* id, const char* name, WidgetKind kind, Category category,
                           const char* group) -> WidgetInstance& {
        return registry.Add(Make(id, name, kind, category, group));
    };

    // ------------------------------------------------------------------ Basic
    {
        WidgetInstance& w = add("basic.button.primary", "Primary Button", WidgetKind::Button,
                                Category::Basic, "Buttons");
        w.state.text = "Primary Button";
        w.state.tooltip = "Executes the primary action.";
        w.state.size = ImVec2(180.0f, 32.0f);
        w.state.color = kAccent;
    }
    {
        WidgetInstance& w = add("basic.button.secondary", "Secondary Button", WidgetKind::Button,
                                Category::Basic, "Buttons");
        w.state.text = "Secondary";
        w.state.tooltip = "Secondary action, neutral colour.";
        w.state.size = ImVec2(140.0f, 32.0f);
        w.state.color = ImVec4(0.34f, 0.37f, 0.43f, 1.00f);
    }
    {
        WidgetInstance& w = add("basic.button.icon", "Icon Button", WidgetKind::Button,
                                Category::Basic, "Buttons");
        w.state.text = "Save";
        w.state.tooltip = "Draws a vector icon next to the label.";
        w.state.size = ImVec2(120.0f, 32.0f);
        w.state.color = kGreen;
    }
    {
        WidgetInstance& w = add("basic.button.disabled", "Disabled Button", WidgetKind::Button,
                                Category::Basic, "Buttons");
        w.state.text = "Disabled";
        w.state.tooltip = "Disabled through the Inspector (Enabled = false).";
        w.state.size = ImVec2(140.0f, 32.0f);
        w.state.enabled = false;
    }
    {
        WidgetInstance& w = add("basic.toggle.play", "Toggle Button", WidgetKind::ToggleButton,
                                Category::Basic, "Toggles");
        w.state.text = "Auto Play";
        w.state.toggled = true;
        w.state.color = kOrange;
    }
    {
        WidgetInstance& w = add("basic.checkbox.grid", "Checkbox (grid)", WidgetKind::Checkbox,
                                Category::Basic, "Toggles");
        w.state.text = "Show Grid";
        w.state.checked = true;
    }
    {
        WidgetInstance& w = add("basic.checkbox.snap", "Checkbox (snap)", WidgetKind::Checkbox,
                                Category::Basic, "Toggles");
        w.state.text = "Snap To Grid";
        w.state.checked = false;
    }
    {
        WidgetInstance& w = add("basic.radio.quality", "Radio Button Group", WidgetKind::RadioButton,
                                Category::Basic, "Toggles");
        w.state.text = "Quality";
        w.state.items = Vec({ "Low", "Medium", "High", "Ultra" });
        w.state.selectedIndex = 2;
    }
    {
        WidgetInstance& w = add("basic.slider.horizontal", "Slider (Horizontal)", WidgetKind::SliderHorizontal,
                                Category::Basic, "Values");
        w.state.text = "Volume";
        w.state.value = 0.65f;
        w.state.minValue = 0.0f;
        w.state.maxValue = 1.0f;
        w.state.format = "%.2f";
        w.state.color = kAccent;
    }
    {
        WidgetInstance& w = add("basic.slider.vertical", "Slider (Vertical)", WidgetKind::SliderVertical,
                                Category::Basic, "Values");
        w.state.text = "Exposure";
        w.state.value = 1.40f;
        w.state.minValue = 0.0f;
        w.state.maxValue = 3.0f;
        w.state.color = kOrange;
    }
    {
        WidgetInstance& w = add("basic.progress.compile", "Progress Bar", WidgetKind::ProgressBar,
                                Category::Basic, "Values");
        w.state.text = "Compiling Shaders";
        w.state.value = 0.42f;
        w.state.color = kGreen;
    }
    {
        WidgetInstance& w = add("basic.input.name", "Text Input", WidgetKind::TextInput,
                                Category::Basic, "Inputs");
        w.state.text = "PlayerOne";
        w.state.validationRule = "nonempty";
        w.state.hint = "Player display name.";
    }
    {
        WidgetInstance& w = add("basic.input.multiline", "Multiline Input", WidgetKind::MultilineInput,
                                Category::Basic, "Inputs");
        w.state.text = "Notes";
        w.state.multilineText = "Paste design notes here.\n"
                                "The text is shared with the Inspector.";
        w.state.validationRule = "minlength";
    }
    {
        WidgetInstance& w = add("basic.combo.quality", "Dropdown (Combo)", WidgetKind::Combo,
                                Category::Basic, "Inputs");
        w.state.text = "Quality Preset";
        w.state.items = Vec({ "Low", "Medium", "High", "Epic", "Cinematic" });
        w.state.selectedIndex = 2;
    }
    {
        WidgetInstance& w = add("basic.list.levels", "List Box", WidgetKind::ListBox,
                                Category::Basic, "Collections");
        w.state.text = "Levels";
        w.state.items = Vec({ "Level_01_Outpost", "Level_02_Canyon", "Level_03_Facility",
                              "Level_04_Skybase", "Level_05_Underworld" });
        w.state.selectedIndex = 0;
    }
    {
        WidgetInstance& w = add("basic.tree.content", "Tree View", WidgetKind::TreeView,
                                Category::Basic, "Collections");
        w.state.text = "Content";
        w.state.items = Vec({ "Characters", "Environment", "Weapons", "User Interface" });
    }
    {
        WidgetInstance& w = add("basic.tabs.inspector", "Tabs", WidgetKind::Tabs,
                                Category::Basic, "Collections");
        w.state.text = "Tabs";
        w.state.items = Vec({ "Details", "Materials", "Physics" });
        w.state.selectedIndex = 0;
    }
    {
        WidgetInstance& w = add("basic.tooltip.save", "Tooltip", WidgetKind::TooltipButton,
                                Category::Basic, "Feedback");
        w.state.text = "Hover for Tooltip";
        w.state.tooltip = "Delayed tooltip with rich text and a shortcut hint.";
        w.state.hint = "Ctrl+S";
    }
    {
        WidgetInstance& w = add("basic.contextmenu.row", "Context Menu Area", WidgetKind::ContextMenuArea,
                                Category::Basic, "Feedback");
        w.state.text = "Right click this row";
        w.state.items = Vec({ "Rename", "Duplicate", "Delete", "Copy Reference" });
    }
    {
        WidgetInstance& w = add("basic.menubar.doc", "Menu Bar", WidgetKind::MenuBarSample,
                                Category::Basic, "Bars");
        w.state.text = "Embedded Menu Bar";
        w.state.items = Vec({ "File", "Edit", "View", "Components", "Theme", "Help" });
    }
    {
        WidgetInstance& w = add("basic.toolbar.commands", "Toolbar", WidgetKind::ToolbarSample,
                                Category::Basic, "Bars");
        w.state.text = "Embedded Toolbar";
        w.state.items = Vec({ "New", "Open", "Save", "Play", "Pause", "Stop" });
        w.state.spacingX = 6.0f;
    }
    {
        WidgetInstance& w = add("basic.separator.line", "Separator", WidgetKind::SeparatorLine,
                                Category::Basic, "Bars");
        w.state.text = "Section";
        w.state.color = ImVec4(0.40f, 0.44f, 0.52f, 1.00f);
    }
    {
        WidgetInstance& w = add("basic.scroll.panel", "Scroll View", WidgetKind::ScrollView,
                                Category::Basic, "Collections");
        w.state.text = "Log Entry";
        w.state.intValue = 40;
        w.state.intMin = 1;
        w.state.intMax = 200;
    }
    {
        WidgetInstance& w = add("basic.image.view", "Image View", WidgetKind::ImageView,
                                Category::Basic, "Media");
        w.state.text = "Procedural Texture";
        w.state.size = ImVec2(280.0f, 160.0f);
        w.state.color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    }
    {
        WidgetInstance& w = add("basic.color.picker", "Color Picker", WidgetKind::ColorPicker,
                                Category::Basic, "Media");
        w.state.text = "Accent Colour";
        w.state.color = ImVec4(0.95f, 0.55f, 0.20f, 1.00f);
        w.state.color2 = ImVec4(0.10f, 0.11f, 0.14f, 1.00f);
        w.state.useCustomColor = true;
    }
    {
        WidgetInstance& w = add("basic.numeric.amount", "Numeric Input", WidgetKind::NumericInput,
                                Category::Basic, "Inputs");
        w.state.text = "Count";
        w.state.intValue = 25;
        w.state.intMin = 0;
        w.state.intMax = 100;
        w.state.step = 1.0f;
    }
    {
        WidgetInstance& w = add("basic.search.filter", "Search Field", WidgetKind::SearchField,
                                Category::Basic, "Inputs");
        w.state.text = "";
        w.state.hint = "Filter assets...";
        w.state.items = Vec({ "Ballista", "Catapult", "Dragon", "Griffin", "Knight", "Wizard" });
    }
    {
        WidgetInstance& w = add("basic.dialog.trigger", "Modal Dialog", WidgetKind::ModalDialogTrigger,
                                Category::Basic, "Feedback");
        w.state.text = "Delete Asset";
        w.state.tooltip = "Opening a confirmation dialog.";
        w.state.hint = "Assets/MyAsset.uasset will be removed permanently.";
        w.state.validationRule = "identifier";
    }
    {
        WidgetInstance& w = add("basic.toast.trigger", "Notification / Toast", WidgetKind::ToastTrigger,
                                Category::Basic, "Feedback");
        w.state.text = "Build Succeeded";
        w.state.hint = "All shaders compiled in 3.4s.";
        w.state.selectedIndex = 1; // ToastKind index
    }

    // ----------------------------------------------------------------- Layout
    {
        WidgetInstance& w = add("layout.box.horizontal", "Horizontal Box", WidgetKind::HorizontalBox,
                                Category::Layout, "Boxes");
        w.state.spacingX = 10.0f;
        w.state.alignment = 1;
        w.state.color = kAccent;
    }
    {
        WidgetInstance& w = add("layout.box.vertical", "Vertical Box", WidgetKind::VerticalBox,
                                Category::Layout, "Boxes");
        w.state.spacingY = 8.0f;
        w.state.alignment = 0;
        w.state.color = kGreen;
    }
    {
        WidgetInstance& w = add("layout.grid.stats", "Grid Layout", WidgetKind::GridLayout,
                                Category::Layout, "Boxes");
        w.state.text = "Stat";
        w.state.intValue = 3;
        w.state.intMin = 1;
        w.state.intMax = 6;
        w.state.spacingX = 10.0f;
        w.state.spacingY = 10.0f;
    }
    {
        WidgetInstance& w = add("layout.stack.cards", "Stack Layout", WidgetKind::StackLayout,
                                Category::Layout, "Boxes");
        w.state.text = "Panel";
        w.state.intValue = 5;
        w.state.value = 22.0f;   // offset step in pixels
        w.state.maxValue = 64.0f;
        w.state.color = kViolet;
    }
    {
        WidgetInstance& w = add("layout.scroll.panel", "Scrollable Panel", WidgetKind::ScrollPanel,
                                Category::Layout, "Containers");
        w.state.text = "Row";
        w.state.intValue = 60;
        w.state.intMin = 5;
        w.state.intMax = 300;
    }
    {
        WidgetInstance& w = add("layout.collapsible.section", "Collapsible Panel", WidgetKind::CollapsiblePanel,
                                Category::Layout, "Containers");
        w.state.text = "Advanced Settings";
        w.state.toggled = true;
    }
    {
        WidgetInstance& w = add("layout.splitter.main", "Splitter", WidgetKind::Splitter,
                                Category::Layout, "Containers");
        w.state.value = 0.38f;
        w.state.minValue = 0.10f;
        w.state.maxValue = 0.90f;
        w.state.color = kAccent;
    }
    {
        WidgetInstance& w = add("layout.dockable.window", "Dockable Window", WidgetKind::DockableWindowTrigger,
                                Category::Layout, "Windows");
        w.state.text = "Floating Slate Window";
        w.state.toggled = false;
        w.state.position = ImVec2(320.0f, 220.0f);
        w.state.size = ImVec2(420.0f, 260.0f);
    }
    {
        WidgetInstance& w = add("layout.nested.panels", "Nested Panels", WidgetKind::NestedPanels,
                                Category::Layout, "Containers");
        w.state.text = "Panel";
        w.state.spacingX = 8.0f;
        w.state.spacingY = 8.0f;
    }
    {
        WidgetInstance& w = add("layout.responsive.grid", "Responsive Layout", WidgetKind::ResponsivePanel,
                                Category::Layout, "Containers");
        w.state.text = "Tile";
        w.state.intValue = 180;   // minimum tile width
        w.state.intMin = 90;
        w.state.intMax = 400;
        w.state.spacingX = 10.0f;
    }
    {
        WidgetInstance& w = add("layout.anchors.box", "Anchors & Alignment", WidgetKind::AnchorsPanel,
                                Category::Layout, "Alignment");
        w.state.text = "Anchored Box";
        w.state.size = ImVec2(180.0f, 90.0f);
        w.state.position = ImVec2(20.0f, 20.0f);
        w.state.anchorMin = static_cast<int>(AnchorPreset::TopLeft);
        w.state.anchorMax = static_cast<int>(AnchorPreset::TopLeft);
        w.state.pivot = static_cast<int>(AnchorPreset::TopLeft);
        w.state.color = kAccent;
    }
    {
        WidgetInstance& w = add("layout.spacing.controls", "Margins & Spacing", WidgetKind::SpacingControls,
                                Category::Layout, "Alignment");
        w.state.text = "Spacing Sample";
        w.state.spacingX = 10.0f;
        w.state.spacingY = 8.0f;
        w.state.paddingX = 12.0f;
        w.state.paddingY = 10.0f;
        w.state.marginX = 6.0f;
        w.state.marginY = 6.0f;
    }

    // ------------------------------------------------------------------ Slate
    {
        WidgetInstance& w = add("slate.window.spawn", "Slate Window", WidgetKind::SlateWindowTrigger,
                                Category::Slate, "Windows");
        w.state.text = "Details (Slate Style)";
        w.state.toggled = false;
        w.state.position = ImVec2(360.0f, 200.0f);
        w.state.size = ImVec2(460.0f, 300.0f);
        w.state.tooltip = "Dockable editor window with a custom title bar.";
    }
    {
        WidgetInstance& w = add("slate.details.properties", "Property Details", WidgetKind::SlateDetails,
                                Category::Slate, "Panels");
        w.state.text = "StaticMeshActor";
        w.state.items = Vec({ "Transform", "Static Mesh", "Materials", "Lighting", "Tags" });
        w.state.selectedIndex = 1;
    }
    {
        WidgetInstance& w = add("slate.assetbrowser.mini", "Asset Browser", WidgetKind::SlateAssetBrowser,
                                Category::Slate, "Panels");
        w.state.text = "/Game/Characters";
        w.state.items = Vec({ "SK_Knight", "M_Knight_Body", "T_Knight_Diffuse", "ABP_Knight" });
    }
    {
        WidgetInstance& w = add("slate.hierarchy.mini", "Hierarchy", WidgetKind::SlateHierarchy,
                                Category::Slate, "Panels");
        w.state.text = "SceneOutliner";
        w.state.items = Vec({ "DirectionalLight", "SkyAtmosphere", "Floor", "Knight", "Camera" });
    }
    {
        WidgetInstance& w = add("slate.searchtree.outliner", "Searchable Tree", WidgetKind::SlateSearchTree,
                                Category::Slate, "Panels");
        w.state.text = "";  // search query
        w.state.hint = "Search actors...";
        w.state.items = Vec({ "BP_Door", "BP_Lever", "BP_Trigger", "SM_Wall", "SM_Floor" });
    }
    {
        WidgetInstance& w = add("slate.contextmenu.actor", "Slate Context Menu", WidgetKind::SlateContextMenu,
                                Category::Slate, "Commands");
        w.state.items = Vec({ "Duplicate", "Copy Reference", "Focus", "Hide", "Delete" });
    }
    {
        WidgetInstance& w = add("slate.toolbar.commands", "Command Toolbar", WidgetKind::SlateToolbar,
                                Category::Slate, "Commands");
        w.state.items = Vec({ "New Blueprint", "Compile", "Play In Editor", "Simulate", "Save All" });
        w.state.spacingX = 6.0f;
    }

    // -------------------------------------------------------------------- UMG
    {
        WidgetInstance& w = add("umg.canvas.main", "Canvas Panel", WidgetKind::UmgCanvas,
                                Category::UMG, "Canvas");
        w.state.text = "Canvas Panel";
        w.state.size = ImVec2(720.0f, 420.0f);
        w.state.color = ImVec4(0.07f, 0.08f, 0.10f, 1.00f);
        w.state.anchorMin = static_cast<int>(AnchorPreset::TopLeft);
        w.state.anchorMax = static_cast<int>(AnchorPreset::TopLeft);
    }
    {
        WidgetInstance& w = add("umg.overlay.hud", "Overlay Panel", WidgetKind::UmgOverlay,
                                Category::UMG, "Panels");
        w.state.text = "HUD Overlay";
        w.state.spacingX = 12.0f;
        w.state.spacingY = 8.0f;
        w.state.alignment = 1;
        w.state.color = ImVec4(0.02f, 0.02f, 0.03f, 0.55f);
    }
    {
        WidgetInstance& w = add("umg.container.panel", "Widget Container", WidgetKind::UmgWidgetContainer,
                                Category::UMG, "Panels");
        w.state.text = "Inventory";
        w.state.flexDirection = 1;
        w.state.alignment = 0;
        w.state.color = ImVec4(0.12f, 0.13f, 0.17f, 0.92f);
    }
    {
        WidgetInstance& w = add("umg.anchors.controls", "Anchor & Alignment", WidgetKind::UmgAnchors,
                                Category::UMG, "Layout");
        w.state.anchorMin = static_cast<int>(AnchorPreset::TopRight);
        w.state.anchorMax = static_cast<int>(AnchorPreset::TopRight);
        w.state.pivot = static_cast<int>(AnchorPreset::TopRight);
        w.state.position = ImVec2(-24.0f, 24.0f);
        w.state.size = ImVec2(200.0f, 56.0f);
    }
    {
        WidgetInstance& w = add("umg.zorder.controls", "Z-Order", WidgetKind::UmgZOrder,
                                Category::UMG, "Layout");
        w.state.intValue = 3;
        w.state.intMin = 0;
        w.state.intMax = 20;
    }
    {
        WidgetInstance& w = add("umg.transform.widget", "Widget Transform", WidgetKind::UmgTransform,
                                Category::UMG, "Layout");
        w.state.text = "Reward Card";
        w.state.position = ImVec2(320.0f, 180.0f);
        w.state.size = ImVec2(220.0f, 120.0f);
        w.state.scale = ImVec2(1.0f, 1.0f);
        w.state.rotation = 0.0f;
        w.state.opacity = 1.0f;
        w.state.zOrder = 5;
        w.state.color = ImVec4(0.20f, 0.24f, 0.34f, 0.95f);
    }
    {
        WidgetInstance& w = add("umg.progress.bar", "Progress Bar (UMG)", WidgetKind::UmgProgress,
                                Category::UMG, "Widgets");
        w.state.text = "Experience";
        w.state.value = 0.68f;
        w.state.color = kOrange;
    }
    {
        WidgetInstance& w = add("umg.hud.status", "HUD", WidgetKind::UmgHud,
                                Category::UMG, "Widgets");
        w.state.text = "Player HUD";
        w.state.color = kGreen;
        w.state.value = 0.72f;
    }
    {
        WidgetInstance& w = add("umg.preview.screen", "Interface Preview", WidgetKind::UmgPreview,
                                Category::UMG, "Widgets");
        w.state.text = "Game Screen Preview";
        w.state.color = kAccent;
        w.state.value = 1.0f;
    }

    // -------------------------------------------------------------- UI Toolkit
    {
        WidgetInstance& w = add("ut.hierarchy.tree", "Element Hierarchy", WidgetKind::UtHierarchy,
                                Category::UIToolkit, "Document");
        w.state.text = "root.uxml";
        w.state.items = Vec({ "visual-element", "label#title", ".card", ".toolbar" });
    }
    {
        WidgetInstance& w = add("ut.stylesheet.rules", "Style Sheet", WidgetKind::UtStyleSheet,
                                Category::UIToolkit, "Styling");
        w.state.text = "main.uss";
        w.state.items = Vec({ ".card", ".card:hover", "#title", "button.primary" });
        w.state.color = kAccent;
    }
    {
        WidgetInstance& w = add("ut.classes.theme", "Style Classes", WidgetKind::UtClasses,
                                Category::UIToolkit, "Styling");
        w.state.text = "Theme: Dark";
        w.state.items = Vec({ "primary", "secondary", "danger", "ghost" });
        w.state.selectedIndex = 0;
        w.state.color = kViolet;
    }
    {
        WidgetInstance& w = add("ut.selectors.list", "Selectors", WidgetKind::UtSelectors,
                                Category::UIToolkit, "Styling");
        w.state.text = ".card > label:first-child";
        w.state.color = kGreen;
    }
    {
        WidgetInstance& w = add("ut.declarative.document", "Declarative Elements", WidgetKind::UtDeclarative,
                                Category::UIToolkit, "Document");
        w.state.text = "panel.uxml";
        w.state.items = Vec({ "VisualElement", "Button", "Label", "Scroller" });
    }
    {
        WidgetInstance& w = add("ut.flex.container", "Flex Layout", WidgetKind::UtFlex,
                                Category::UIToolkit, "Layout");
        w.state.text = "flex-container";
        w.state.flexDirection = 0;
        w.state.spacingX = 12.0f;
        w.state.spacingY = 12.0f;
        w.state.paddingX = 14.0f;
        w.state.paddingY = 12.0f;
        w.state.alignItems = 1;
    }
    {
        WidgetInstance& w = add("ut.templates.list", "Component Templates", WidgetKind::UtTemplates,
                                Category::UIToolkit, "Templates");
        w.state.text = "Templates";
        w.state.items = Vec({ "Card", "Toolbar", "Stat Row", "Inventory Slot" });
        w.state.selectedIndex = 0;
    }
    {
        WidgetInstance& w = add("ut.preview.panel", "Interface Preview (Toolkit)", WidgetKind::UtPreview,
                                Category::UIToolkit, "Templates");
        w.state.text = "USS Preview";
        w.state.color = ImVec4(0.15f, 0.16f, 0.20f, 1.00f);
    }

    // ----------------------------------------------------------------- IMGUI
    {
        WidgetInstance& w = add("im.window.floating", "Immediate Mode Window", WidgetKind::ImWindowTrigger,
                                Category::Immediate, "Window");
        w.state.text = "Immediate Mode Window";
        w.state.toggled = false;
        w.state.position = ImVec2(520.0f, 160.0f);
        w.state.size = ImVec2(380.0f, 300.0f);
    }
    {
        WidgetInstance& w = add("im.conditional.flags", "Conditional Widgets", WidgetKind::ImConditional,
                                Category::Immediate, "Widgets");
        w.state.text = "Show advanced";
        w.state.items = Vec({ "Wireframe", "Bounds", "FPS Graph", "Stats Panel", "Grid" });
        w.state.toggled = true;
    }
    {
        WidgetInstance& w = add("im.dynamic.controls", "Dynamic Controls", WidgetKind::ImDynamicControls,
                                Category::Immediate, "Widgets");
        w.state.text = "Property";
        w.state.intValue = 4;
        w.state.intMin = 1;
        w.state.intMax = 12;
        w.state.items = Vec({ "Float", "Integer", "Boolean", "Colour", "Text" });
    }
    {
        WidgetInstance& w = add("im.debug.overlay", "Debug Panel", WidgetKind::ImDebugPanel,
                                Category::Immediate, "Diagnostics");
        w.state.items = Vec({ "Draw Bounds", "Draw Anchors", "Draw Grid", "Log Interactions" });
        w.state.color = kOrange;
    }
    {
        WidgetInstance& w = add("im.stats.graphs", "Real-time Statistics", WidgetKind::ImStats,
                                Category::Immediate, "Diagnostics");
        w.state.text = "Frame time (ms)";
        w.state.value = 60.0f;   // smoothing window in frames
        w.state.minValue = 10.0f;
        w.state.maxValue = 240.0f;
        w.state.color = kAccent;
    }
    {
        WidgetInstance& w = add("im.update.animation", "Per-frame Update", WidgetKind::ImUpdate,
                                Category::Immediate, "Diagnostics");
        w.state.text = "Phase";
        w.state.value = 1.0f;      // animation speed
        w.state.minValue = 0.0f;
        w.state.maxValue = 4.0f;
    }

    // ------------------------------------------------------------------ uGUI
    {
        WidgetInstance& w = add("ugui.canvas.main", "Canvas (uGUI)", WidgetKind::UgCanvas,
                                Category::UGui, "Canvas");
        w.state.text = "Canvas";
        w.state.size = ImVec2(760.0f, 440.0f);
        w.state.color = ImVec4(0.09f, 0.10f, 0.13f, 1.00f);
        w.state.spacingX = 14.0f;
    }
    {
        WidgetInstance& w = add("ugui.recttransform.target", "Rect Transform", WidgetKind::UgRectTransform,
                                Category::UGui, "Transform");
        w.state.text = "Rect Target";
        w.state.anchorMin = static_cast<int>(AnchorPreset::MiddleCenter);
        w.state.anchorMax = static_cast<int>(AnchorPreset::MiddleCenter);
        w.state.pivot = static_cast<int>(AnchorPreset::MiddleCenter);
        w.state.position = ImVec2(0.0f, 0.0f);
        w.state.size = ImVec2(240.0f, 120.0f);
        w.state.color = ImVec4(0.24f, 0.30f, 0.42f, 0.95f);
    }
    {
        WidgetInstance& w = add("ugui.anchors.presets", "Anchors", WidgetKind::UgAnchors,
                                Category::UGui, "Transform");
        w.state.anchorMin = static_cast<int>(AnchorPreset::TopLeft);
        w.state.anchorMax = static_cast<int>(AnchorPreset::TopLeft);
        w.state.pivot = static_cast<int>(AnchorPreset::TopLeft);
    }
    {
        WidgetInstance& w = add("ugui.pivot.control", "Pivot", WidgetKind::UgPivot,
                                Category::UGui, "Transform");
        w.state.pivot = static_cast<int>(AnchorPreset::MiddleCenter);
        w.state.position = ImVec2(40.0f, 40.0f);
        w.state.size = ImVec2(160.0f, 90.0f);
        w.state.color = kViolet;
    }
    {
        WidgetInstance& w = add("ugui.layoutgroup.panel", "Layout Group", WidgetKind::UgLayoutGroup,
                                Category::UGui, "Layout");
        w.state.text = "Horizontal Layout Group";
        w.state.items = Vec({ "Horizontal", "Vertical", "Grid" });
        w.state.selectedIndex = 0;
        w.state.spacingX = 12.0f;
        w.state.paddingX = 12.0f;
        w.state.paddingY = 12.0f;
        w.state.toggled = true;      // child force expand
        w.state.flexGrow = 0.0f;
    }
    {
        WidgetInstance& w = add("ugui.contentfitter.panel", "Content Size Fitter", WidgetKind::UgContentFitter,
                                Category::UGui, "Layout");
        w.state.text = "Fitted Panel";
        w.state.toggled = true;
        w.state.spacingX = 10.0f;
        w.state.paddingX = 14.0f;
        w.state.paddingY = 12.0f;
    }
    {
        WidgetInstance& w = add("ugui.scrollrect.view", "Scroll Rect", WidgetKind::UgScrollRect,
                                Category::UGui, "Layout");
        w.state.text = "Scroll Rect";
        w.state.value = 0.0f;      // scroll offset (pixels)
        w.state.maxValue = 0.0f;   // content height - viewport height, filled at runtime
        w.state.toggled = true;    // inertia
        w.state.size = ImVec2(420.0f, 200.0f);
    }
    {
        WidgetInstance& w = add("ugui.hud.stats", "HUD (Health / Energy / Score)", WidgetKind::UgHud,
                                Category::UGui, "HUD");
        w.state.text = "HUD";
        w.state.value = 0.72f;
        w.state.color = kGreen;
    }
}

// ---------------------------------------------------------------------------
// Default state per kind (used by "Reset properties")
// ---------------------------------------------------------------------------
WidgetState DefaultStateOf(WidgetKind kind) {
    WidgetState s;
    switch (kind) {
    case WidgetKind::Button:
        s.text = "Button";
        s.size = ImVec2(160.0f, 32.0f);
        break;
    case WidgetKind::SliderHorizontal:
    case WidgetKind::SliderVertical:
        s.text = "Slider";
        s.value = 0.5f;
        break;
    case WidgetKind::ProgressBar:
        s.text = "Progress";
        s.value = 0.42f;
        break;
    default:
        break;
    }
    return s;
}

} // namespace uilab
