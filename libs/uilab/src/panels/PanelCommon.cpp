// PanelCommon.cpp - helpers shared by several panels (icons per kind, category
// colours, create/delete component flows).
#include "panels/Panels.h"

#include <algorithm>

#include <imgui.h>

#include "core/AppContext.h"
#include "core/Dialogs.h"
#include "ui/Widgets.h"

namespace uilab {
namespace panels {

icons::Icon IconForKind(WidgetKind kind) {
    using K = WidgetKind;
    switch (kind) {
    case K::Button:              return icons::Icon::Point;
    case K::ToggleButton:        return icons::Icon::Bolt;
    case K::Checkbox:            return icons::Icon::Check;
    case K::RadioButton:         return icons::Icon::List;
    case K::SliderHorizontal:
    case K::SliderVertical:      return icons::Icon::Sliders;
    case K::ProgressBar:         return icons::Icon::Chart;
    case K::TextInput:
    case K::MultilineInput:      return icons::Icon::File;
    case K::Combo:               return icons::Icon::ChevronDown;
    case K::ListBox:             return icons::Icon::List;
    case K::TreeView:            return icons::Icon::Tree;
    case K::Tabs:                return icons::Icon::PanelBottom;
    case K::TooltipButton:       return icons::Icon::Info;
    case K::ContextMenuArea:     return icons::Icon::Filter;
    case K::MenuBarSample:       return icons::Icon::List;
    case K::ToolbarSample:       return icons::Icon::Gear;
    case K::SeparatorLine:       return icons::Icon::Minus;
    case K::ScrollView:          return icons::Icon::PanelBottom;
    case K::ImageView:           return icons::Icon::Image;
    case K::ColorPicker:         return icons::Icon::Palette;
    case K::NumericInput:        return icons::Icon::Gear;
    case K::SearchField:         return icons::Icon::Search;
    case K::ModalDialogTrigger:  return icons::Icon::Warning;
    case K::ToastTrigger:        return icons::Icon::Info;

    case K::VerticalBox:         return icons::Icon::PanelBottom;
    case K::HorizontalBox:       return icons::Icon::PanelLeft;
    case K::GridLayout:          return icons::Icon::Grid;
    case K::StackLayout:         return icons::Icon::Layers;
    case K::ScrollPanel:         return icons::Icon::PanelBottom;
    case K::CollapsiblePanel:    return icons::Icon::ChevronDown;
    case K::Splitter:            return icons::Icon::Sliders;
    case K::DockableWindowTrigger: return icons::Icon::Dock;
    case K::NestedPanels:        return icons::Icon::Layers;
    case K::ResponsivePanel:     return icons::Icon::Grid;
    case K::AnchorsPanel:        return icons::Icon::Point;
    case K::SpacingControls:     return icons::Icon::Grid;

    case K::SlateWindowTrigger:  return icons::Icon::Dock;
    case K::SlateDetails:        return icons::Icon::Sliders;
    case K::SlateAssetBrowser:   return icons::Icon::Folder;
    case K::SlateHierarchy:      return icons::Icon::Tree;
    case K::SlateSearchTree:     return icons::Icon::Search;
    case K::SlateContextMenu:    return icons::Icon::List;
    case K::SlateToolbar:        return icons::Icon::Gear;

    case K::UmgCanvas:           return icons::Icon::Grid;
    case K::UmgOverlay:          return icons::Icon::Layers;
    case K::UmgWidgetContainer:  return icons::Icon::PanelBottom;
    case K::UmgAnchors:          return icons::Icon::Point;
    case K::UmgZOrder:           return icons::Icon::Layers;
    case K::UmgTransform:        return icons::Icon::Cube;
    case K::UmgProgress:         return icons::Icon::Chart;
    case K::UmgHud:              return icons::Icon::Heart;
    case K::UmgPreview:          return icons::Icon::Image;

    case K::UtHierarchy:         return icons::Icon::Tree;
    case K::UtStyleSheet:        return icons::Icon::Brush;
    case K::UtClasses:           return icons::Icon::Palette;
    case K::UtSelectors:         return icons::Icon::Filter;
    case K::UtDeclarative:       return icons::Icon::File;
    case K::UtFlex:              return icons::Icon::Grid;
    case K::UtTemplates:         return icons::Icon::Copy;
    case K::UtPreview:           return icons::Icon::Image;

    case K::ImWindowTrigger:     return icons::Icon::Dock;
    case K::ImConditional:       return icons::Icon::Check;
    case K::ImDynamicControls:   return icons::Icon::Refresh;
    case K::ImDebugPanel:        return icons::Icon::Bug;
    case K::ImStats:             return icons::Icon::Chart;
    case K::ImUpdate:            return icons::Icon::Play;

    case K::UgCanvas:            return icons::Icon::Grid;
    case K::UgRectTransform:     return icons::Icon::Cube;
    case K::UgAnchors:           return icons::Icon::Point;
    case K::UgPivot:             return icons::Icon::Point;
    case K::UgLayoutGroup:       return icons::Icon::Layers;
    case K::UgContentFitter:     return icons::Icon::Refresh;
    case K::UgScrollRect:        return icons::Icon::PanelBottom;
    case K::UgHud:               return icons::Icon::Heart;

    case K::Count:               break;
    }
    return icons::Icon::Widget;
}

ImVec4 AccentForCategory(Category c) {
    switch (c) {
    case Category::Basic:     return ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    case Category::Layout:    return ImVec4(0.33f, 0.78f, 0.42f, 1.00f);
    case Category::Slate:     return ImVec4(0.95f, 0.62f, 0.24f, 1.00f);
    case Category::UMG:       return ImVec4(0.62f, 0.47f, 0.96f, 1.00f);
    case Category::UIToolkit: return ImVec4(0.30f, 0.80f, 0.82f, 1.00f);
    case Category::Immediate: return ImVec4(0.92f, 0.36f, 0.32f, 1.00f);
    case Category::UGui:      return ImVec4(0.88f, 0.72f, 0.32f, 1.00f);
    case Category::Count:     break;
    }
    return ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
}

void OpenCreateComponentDialog(AppContext& ctx) {
    std::vector<std::string> kinds;
    for (int i = 0; i < static_cast<int>(WidgetKind::Count); ++i)
        kinds.push_back(KindName(static_cast<WidgetKind>(i)));

    ctx.dialogs.CreateWidgetDialog("New Component", kinds,
                                   [&ctx](bool ok, std::string name, std::string tag, int kindIndex) {
        if (!ok)
            return;
        if (name.empty())
            name = "Component";
        WidgetInstance inst;
        inst.displayName = name;
        inst.kind = static_cast<WidgetKind>(std::clamp(kindIndex, 0, static_cast<int>(WidgetKind::Count) - 1));
        inst.category = ctx.activeCategory;
        inst.group = tag.empty() ? std::string("Custom") : tag;
        inst.state.text = name;
        inst.state = DefaultStateOf(inst.kind);
        inst.state.text = name;
        inst.liveBound = false;
        inst.id = ctx.registry.MakeUniqueId("custom." + ToLower(inst.group) + "." + ToLower(name));
        WidgetInstance& created = ctx.registry.Add(std::move(inst));
        ctx.SelectWidget(created.id);
        ctx.LogConsole(LogLevel::Success, "Registry",
                       "Created " + created.displayName + " (" + std::string(KindName(created.kind)) + ")");
        ctx.Notify(ToastKind::Success, "Component created",
                   created.displayName + " in " + CategoryName(created.category));
    });
}

void RequestDeleteComponent(AppContext& ctx, WidgetInstance& inst) {
    const std::string id = inst.id;
    const std::string name = inst.displayName;
    ctx.dialogs.Confirm("Delete Component",
                        "Delete \"" + name + "\"? This removes it from the component tree and cannot be "
                        "undone through the history stack.",
                        true, [&ctx, id, name](bool confirmed) {
        if (!confirmed)
            return;
        if (ctx.registry.Remove(id)) {
            if (ctx.selection.widgetId == id)
                ctx.selection.widgetId.clear();
            ctx.LogConsole(LogLevel::Warning, "Registry", "Deleted component: " + name);
            ctx.Notify(ToastKind::Warning, "Component deleted", name);
        } else {
            ctx.LogConsole(LogLevel::Error, "Registry", "Component not found: " + id);
        }
    });
}

void RevealComponent(AppContext& ctx, const std::string& id) {
    if (const WidgetInstance* w = ctx.registry.Find(id)) {
        ctx.activeCategory = w->category;
        ctx.SelectWidget(id);
        ctx.LogDebug("Panels", "Revealed " + w->displayName + " on the " +
                                   std::string(CategoryName(w->category)) + " page");
    }
}

} // namespace panels
} // namespace uilab
