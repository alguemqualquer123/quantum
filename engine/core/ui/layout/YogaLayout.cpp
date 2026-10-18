// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#include "ui/layout/YogaLayout.h"

#include <yoga/Yoga.h>

#include "ui/core/VisualElement.h"

using namespace doriax;
using namespace doriax::ui;

namespace {

    YGConfigRef getConfig() {
        static YGConfigRef config = nullptr;
        if (config == nullptr) {
            config = YGConfigNew();
            //web vocabulary: row direction, stretch alignment, shrinkable items
            YGConfigSetUseWebDefaults(config, true);
            //canvas space is already in pixels: disable pixel grid rounding
            YGConfigSetPointScaleFactor(config, 0.0f);
        }
        return config;
    }

    YGNodeRef nodeOf(const VisualElement& element) {
        return element.getLayoutNode();
    }

    YGNodeRef ensureNode(VisualElement& element) {
        YGNodeRef node = nodeOf(element);
        if (node == nullptr) {
            node = YGNodeNewWithConfig(getConfig());
            YGNodeSetContext(node, &element);
            element.setLayoutNode(node);
        }
        return node;
    }

    //enum mapping: explicit switches so a Yoga update cannot silently remap us
    YGDisplay toYoga(Display value) {
        switch (value) {
            case Display::None: return YGDisplayNone;
            default: return YGDisplayFlex;
        }
    }

    YGFlexDirection toYoga(FlexDirection value) {
        switch (value) {
            case FlexDirection::Row: return YGFlexDirectionRow;
            case FlexDirection::RowReverse: return YGFlexDirectionRowReverse;
            case FlexDirection::Column: return YGFlexDirectionColumn;
            default: return YGFlexDirectionColumnReverse;
        }
    }

    YGWrap toYoga(FlexWrap value) {
        switch (value) {
            case FlexWrap::Wrap: return YGWrapWrap;
            case FlexWrap::WrapReverse: return YGWrapWrapReverse;
            default: return YGWrapNoWrap;
        }
    }

    YGJustify toYoga(JustifyContent value) {
        switch (value) {
            case JustifyContent::Center: return YGJustifyCenter;
            case JustifyContent::FlexEnd: return YGJustifyFlexEnd;
            case JustifyContent::SpaceBetween: return YGJustifySpaceBetween;
            case JustifyContent::SpaceAround: return YGJustifySpaceAround;
            case JustifyContent::SpaceEvenly: return YGJustifySpaceEvenly;
            default: return YGJustifyFlexStart;
        }
    }

    YGAlign toYoga(AlignItems value) {
        switch (value) {
            case AlignItems::FlexStart: return YGAlignFlexStart;
            case AlignItems::Center: return YGAlignCenter;
            case AlignItems::FlexEnd: return YGAlignFlexEnd;
            case AlignItems::Baseline: return YGAlignBaseline;
            default: return YGAlignStretch;
        }
    }

    YGAlign toYoga(AlignSelf value) {
        switch (value) {
            case AlignSelf::FlexStart: return YGAlignFlexStart;
            case AlignSelf::Center: return YGAlignCenter;
            case AlignSelf::FlexEnd: return YGAlignFlexEnd;
            case AlignSelf::Stretch: return YGAlignStretch;
            case AlignSelf::Baseline: return YGAlignBaseline;
            default: return YGAlignAuto;
        }
    }

    YGAlign toYoga(AlignContent value) {
        switch (value) {
            case AlignContent::FlexStart: return YGAlignFlexStart;
            case AlignContent::Center: return YGAlignCenter;
            case AlignContent::FlexEnd: return YGAlignFlexEnd;
            case AlignContent::Stretch: return YGAlignStretch;
            case AlignContent::SpaceBetween: return YGAlignSpaceBetween;
            case AlignContent::SpaceAround: return YGAlignSpaceAround;
            default: return YGAlignAuto;
        }
    }

    const YGEdge kEdges[4] = { YGEdgeLeft, YGEdgeTop, YGEdgeRight, YGEdgeBottom };

    void applyWidth(YGNodeRef node, const StyleLength& length) {
        switch (length.unit) {
            case StyleLength::Unit::Percent: YGNodeStyleSetWidthPercent(node, length.value); break;
            case StyleLength::Unit::Auto: YGNodeStyleSetWidthAuto(node); break;
            default: YGNodeStyleSetWidth(node, length.value); break;
        }
    }

    void applyHeight(YGNodeRef node, const StyleLength& length) {
        switch (length.unit) {
            case StyleLength::Unit::Percent: YGNodeStyleSetHeightPercent(node, length.value); break;
            case StyleLength::Unit::Auto: YGNodeStyleSetHeightAuto(node); break;
            default: YGNodeStyleSetHeight(node, length.value); break;
        }
    }

    void applyFlexBasis(YGNodeRef node, const StyleLength& length) {
        switch (length.unit) {
            case StyleLength::Unit::Percent: YGNodeStyleSetFlexBasisPercent(node, length.value); break;
            case StyleLength::Unit::Auto: YGNodeStyleSetFlexBasisAuto(node); break;
            default: YGNodeStyleSetFlexBasis(node, length.value); break;
        }
    }

    //an "auto" limit means "no limit" and lifts a previous one
    void applyMinWidth(YGNodeRef node, const StyleLength& length) {
        switch (length.unit) {
            case StyleLength::Unit::Percent: YGNodeStyleSetMinWidthPercent(node, length.value); break;
            case StyleLength::Unit::Auto: YGNodeStyleSetMinWidth(node, YGUndefined); break;
            default: YGNodeStyleSetMinWidth(node, length.value); break;
        }
    }

    void applyMinHeight(YGNodeRef node, const StyleLength& length) {
        switch (length.unit) {
            case StyleLength::Unit::Percent: YGNodeStyleSetMinHeightPercent(node, length.value); break;
            case StyleLength::Unit::Auto: YGNodeStyleSetMinHeight(node, YGUndefined); break;
            default: YGNodeStyleSetMinHeight(node, length.value); break;
        }
    }

    void applyMaxWidth(YGNodeRef node, const StyleLength& length) {
        switch (length.unit) {
            case StyleLength::Unit::Percent: YGNodeStyleSetMaxWidthPercent(node, length.value); break;
            case StyleLength::Unit::Auto: YGNodeStyleSetMaxWidth(node, YGUndefined); break;
            default: YGNodeStyleSetMaxWidth(node, length.value); break;
        }
    }

    void applyMaxHeight(YGNodeRef node, const StyleLength& length) {
        switch (length.unit) {
            case StyleLength::Unit::Percent: YGNodeStyleSetMaxHeightPercent(node, length.value); break;
            case StyleLength::Unit::Auto: YGNodeStyleSetMaxHeight(node, YGUndefined); break;
            default: YGNodeStyleSetMaxHeight(node, length.value); break;
        }
    }

    void applyPosition(YGNodeRef node, const StyleLength& length, YGEdge edge) {
        switch (length.unit) {
            case StyleLength::Unit::Percent: YGNodeStyleSetPositionPercent(node, edge, length.value); break;
            case StyleLength::Unit::Auto: YGNodeStyleSetPosition(node, edge, YGUndefined); break;
            default: YGNodeStyleSetPosition(node, edge, length.value); break;
        }
    }

    void applyStyle(YGNodeRef node, const LayoutStyle& style) {
        YGNodeStyleSetDisplay(node, toYoga(style.display));
        YGNodeStyleSetPositionType(node, style.positionType == PositionType::Absolute ? YGPositionTypeAbsolute : YGPositionTypeRelative);
        YGNodeStyleSetFlexDirection(node, toYoga(style.flexDirection));
        YGNodeStyleSetFlexWrap(node, toYoga(style.flexWrap));
        YGNodeStyleSetJustifyContent(node, toYoga(style.justifyContent));
        YGNodeStyleSetAlignItems(node, toYoga(style.alignItems));
        YGNodeStyleSetAlignSelf(node, toYoga(style.alignSelf));

        //auto keeps Yoga's web default (stretch) untouched
        if (style.alignContent != AlignContent::Auto)
            YGNodeStyleSetAlignContent(node, toYoga(style.alignContent));

        YGNodeStyleSetFlexGrow(node, style.flexGrow);
        YGNodeStyleSetFlexShrink(node, style.flexShrink);
        applyFlexBasis(node, style.flexBasis);
        applyWidth(node, style.width);
        applyHeight(node, style.height);
        applyMinWidth(node, style.minWidth);
        applyMinHeight(node, style.minHeight);
        applyMaxWidth(node, style.maxWidth);
        applyMaxHeight(node, style.maxHeight);

        const StyleLength* margins[4] = { &style.marginLeft, &style.marginTop, &style.marginRight, &style.marginBottom };
        const StyleLength* paddings[4] = { &style.paddingLeft, &style.paddingTop, &style.paddingRight, &style.paddingBottom };

        for (int i = 0; i < 4; i++) {
            const StyleLength& margin = *margins[i];
            switch (margin.unit) {
                case StyleLength::Unit::Percent: YGNodeStyleSetMarginPercent(node, kEdges[i], margin.value); break;
                case StyleLength::Unit::Auto: YGNodeStyleSetMarginAuto(node, kEdges[i]); break;
                default: YGNodeStyleSetMargin(node, kEdges[i], margin.value); break;
            }

            //padding has no auto: it resolves to zero
            const StyleLength& padding = *paddings[i];
            switch (padding.unit) {
                case StyleLength::Unit::Percent: YGNodeStyleSetPaddingPercent(node, kEdges[i], padding.value); break;
                case StyleLength::Unit::Auto: YGNodeStyleSetPadding(node, kEdges[i], 0.0f); break;
                default: YGNodeStyleSetPadding(node, kEdges[i], padding.value); break;
            }
        }

        applyPosition(node, style.left, YGEdgeLeft);
        applyPosition(node, style.top, YGEdgeTop);
        applyPosition(node, style.right, YGEdgeRight);
        applyPosition(node, style.bottom, YGEdgeBottom);

        YGNodeStyleSetGap(node, YGGutterAll, style.gap);
    }

    YGSize measureCallback(YGNodeRef node, float width, YGMeasureMode widthMode, float height, YGMeasureMode heightMode) {
        VisualElement* element = (VisualElement*)YGNodeGetContext(node);

        YGSize size = { 0.0f, 0.0f };
        if (element == nullptr)
            return size;

        const Vector2 measured = element->measureIntrinsicSize(width, widthMode == YGMeasureModeAtMost,
                                                                height, heightMode == YGMeasureModeAtMost);
        size.width = measured.x;
        size.height = measured.y;
        return size;
    }

    //Reconciles the Yoga children with the element children. Identity is
    //checked, not only the count: elements can be moved between parents.
    void syncChildren(YGNodeRef node, VisualElement& element) {
        const std::vector<std::unique_ptr<VisualElement>>& children = element.getChildren();

        bool matches = YGNodeGetChildCount(node) == children.size();
        if (matches) {
            for (size_t i = 0; i < children.size(); i++) {
                if (nodeOf(*children[i]) != YGNodeGetChild(node, i)) {
                    matches = false;
                    break;
                }
            }
        }

        if (matches)
            return;

        YGNodeRemoveAllChildren(node);
        for (const std::unique_ptr<VisualElement>& child : children)
            YGNodeInsertChild(node, ensureNode(*child), YGNodeGetChildCount(node));
    }

    bool syncDirty(VisualElement& element) {
        bool changed = false;

        if ((element.getDirtyFlags() & DirtyFlags::Layout) != DirtyFlags::None) {
            changed = true;

            YGNodeRef node = ensureNode(element);

            //containers derive child styles (grid cells, scroll offsets) before
            //their children are synced; a "true" return re-syncs the children
            const bool childrenStyled = element.updateLayoutChildren();
            if (childrenStyled) {
                for (const std::unique_ptr<VisualElement>& child : element.getChildren())
                    child->markDirty(DirtyFlags::Layout);
            }

            applyStyle(node, element.getStyle());

            if (element.usesMeasureFunc()) {
                YGNodeSetMeasureFunc(node, measureCallback);
                //the measured content can change without an style change
                //(text edits), so the node is marked explicitly; for regular
                //nodes the Yoga style setters already flag the dirty state
                YGNodeMarkDirty(node);
            } else {
                YGNodeSetMeasureFunc(node, nullptr);
            }

            element.clearDirtyFlags(DirtyFlags::Layout);
            syncChildren(node, element);
        }

        for (const std::unique_ptr<VisualElement>& child : element.getChildren())
            changed = syncDirty(*child) || changed;

        return changed;
    }

    void storeRects(VisualElement& element, float originX, float originY) {
        YGNodeRef node = nodeOf(element);
        if (node == nullptr)
            return;

        const float x = originX + YGNodeLayoutGetLeft(node);
        const float y = originY + YGNodeLayoutGetTop(node);
        const float width = YGNodeLayoutGetWidth(node);
        const float height = YGNodeLayoutGetHeight(node);

        element.setLayoutRect(Rect(x, y, width, height));

        for (const std::unique_ptr<VisualElement>& child : element.getChildren())
            storeRects(*child, x, y);
    }
}

bool YogaLayout::update(VisualElement& rootElement, float width, float height) {
    const bool changed = syncDirty(rootElement);
    if (!changed)
        return false;

    YGNodeRef root = ensureNode(rootElement);

    //an auto-sized root stretches to the canvas so percent children resolve
    const LayoutStyle& rootStyle = rootElement.getStyle();
    if (rootStyle.width.unit == StyleLength::Unit::Auto)
        YGNodeStyleSetWidth(root, width);
    if (rootStyle.height.unit == StyleLength::Unit::Auto)
        YGNodeStyleSetHeight(root, height);

    YGNodeCalculateLayout(root, width, height, YGDirectionLTR);
    storeRects(rootElement, 0.0f, 0.0f);
    return true;
}
