// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "slate/layout/FlexEngine.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

#include <yoga/Yoga.h>

#include "slate/core/Widget.h"
#include "slate/layout/Containers.h"

namespace quantum::slate {

    namespace {

        //Widget.h forward-declares ::YGNode and Yoga.h defines YGNodeRef as
        //struct YGNode*, so the widget handle and the node handle are the same
        //pointer type here.

        YGConfigRef getConfig() {
            static YGConfigRef config = nullptr;
            if (config == nullptr) {
                config = YGConfigNew();
                //CSS semantics: percent bases resolve against the containing box
                //and the Slot defaults (shrink 1, stretch cross axis) line up
                YGConfigSetUseWebDefaults(config, true);
                //canvas space is already device pixels: no grid snapping
                YGConfigSetPointScaleFactor(config, 0.0f);
            }
            return config;
        }

        float definedValue(float value) {
            return YGFloatIsUndefined(value) ? 0.0f : value;
        }

        //A negative "available" is the Widget::measure spelling of unbounded;
        //Yoga wants YGUndefined passed to the solve.
        float islandAvailable(float value) {
            return value < 0.0f ? YGUndefined : value;
        }

        YGNodeRef ensureNode(Widget& widget) {
            YGNodeRef node = widget.getLayoutNode();
            if (node == nullptr) {
                node = YGNodeNewWithConfig(getConfig());
                YGNodeSetContext(node, &widget);
                widget.setLayoutNode(node);
                //a brand-new node carries no style at all, and syncChildren()
                //creates the child nodes before the child itself is walked, so
                //the widget has to look dirty again for that walk to fill it
                widget.markDirty(WidgetDirty::Layout);
            }
            return node;
        }

        //Nothing here may throw: the engine builds with _HAS_EXCEPTIONS=0 and
        //this runs inside YGNodeCalculateLayout's own call stack.
        YGSize measureCallback(YGNodeRef node, float width, YGMeasureMode widthMode,
                               float height, YGMeasureMode heightMode) {
            YGSize out = { 0.0f, 0.0f };

            Widget* widget = static_cast<Widget*>(YGNodeGetContext(node));
            if (widget == nullptr) {
                return out;
            }

            //a negative available is the Widget::measure spelling of
            //"unbounded", which Yoga reports as YGMeasureModeUndefined
            float availableX = (widthMode == YGMeasureModeUndefined || YGFloatIsUndefined(width))
                ? -1.0f : width;
            float availableY = (heightMode == YGMeasureModeUndefined || YGFloatIsUndefined(height))
                ? -1.0f : height;

            Vector2 measured = widget->measure(Vector2(availableX, availableY));

            if (std::isfinite(measured.x) && measured.x > 0.0f) {
                out.width = measured.x;
            }
            if (std::isfinite(measured.y) && measured.y > 0.0f) {
                out.height = measured.y;
            }
            return out;
        }

        //Explicit switches so a Yoga header change cannot silently remap us.
        YGJustify toYoga(Justify justify) {
            switch (justify) {
                case Justify::Center: return YGJustifyCenter;
                case Justify::End: return YGJustifyFlexEnd;
                case Justify::SpaceBetween: return YGJustifySpaceBetween;
                case Justify::SpaceAround: return YGJustifySpaceAround;
                case Justify::SpaceEvenly: return YGJustifySpaceEvenly;
                default: return YGJustifyFlexStart;
            }
        }

        YGAlign toYoga(Align align) {
            switch (align) {
                case Align::Start: return YGAlignFlexStart;
                case Align::Center: return YGAlignCenter;
                case Align::End: return YGAlignFlexEnd;
                default: return YGAlignStretch;
            }
        }

        Align crossAlignOf(const Slot& slot, bool parentHorizontal) {
            if (parentHorizontal) {
                switch (slot.vAlign) {
                    case VAlign::Top: return Align::Start;
                    case VAlign::Center: return Align::Center;
                    case VAlign::Bottom: return Align::End;
                    default: return Align::Stretch;
                }
            }
            switch (slot.hAlign) {
                case HAlign::Left: return Align::Start;
                case HAlign::Center: return Align::Center;
                case HAlign::Right: return Align::End;
                default: return Align::Stretch;
            }
        }

        Align mainAlignOf(const Slot& slot, bool parentHorizontal) {
            if (parentHorizontal) {
                switch (slot.hAlign) {
                    case HAlign::Center: return Align::Center;
                    case HAlign::Right: return Align::End;
                    default: return Align::Start;
                }
            }
            switch (slot.vAlign) {
                case VAlign::Center: return Align::Center;
                case VAlign::Bottom: return Align::End;
                default: return Align::Start;
            }
        }

        //Island parents keep their children out of the parent Yoga tree; each
        //child is solved as an independent root against the island's rect:
        //  CanvasPanel anchors, Overlay/SizeBox share the rect, ScrollBox lays
        //  the content at its natural size with an offset, ScaleBox at design
        //  size. SizeBox is an island because a flex child cannot be told to
        //  fill both axes by the container: its slot would need grow on the
        //  main axis, which belongs to the authoring, not the container.
        bool isIslandParent(const Widget& widget) {
            return dynamic_cast<const CanvasPanel*>(&widget) != nullptr ||
                dynamic_cast<const Overlay*>(&widget) != nullptr ||
                dynamic_cast<const SizeBox*>(&widget) != nullptr ||
                dynamic_cast<const ScrollBox*>(&widget) != nullptr ||
                dynamic_cast<const ScaleBox*>(&widget) != nullptr;
        }

        bool isGridParent(const Widget& widget) {
            return dynamic_cast<const GridPanel*>(&widget) != nullptr;
        }

        Orientation parentOrientation(const Widget& widget) {
            const Widget* parent = widget.getParent();
            if (parent == nullptr) {
                return Orientation::Horizontal;
            }
            const Box* box = dynamic_cast<const Box*>(parent);
            if (box != nullptr) {
                return box->getOrientation();
            }
            const ScrollBox* scroll = dynamic_cast<const ScrollBox*>(parent);
            if (scroll != nullptr) {
                return scroll->getOrientation();
            }
            //grid rows are laid out left to right by the row nodes below
            return Orientation::Horizontal;
        }

        void applyContainerFlow(Widget& widget, YGNodeRef node) {
            const Box* box = dynamic_cast<Box*>(&widget);
            if (box != nullptr) {
                YGNodeStyleSetFlexDirection(node,
                    box->getOrientation() == Orientation::Horizontal
                        ? YGFlexDirectionRow : YGFlexDirectionColumn);
                YGNodeStyleSetJustifyContent(node, toYoga(box->getJustify()));
                YGNodeStyleSetAlignItems(node, toYoga(box->getAlignItems()));
                YGNodeStyleSetGap(node, YGGutterAll, box->getGap());
                YGNodeStyleSetFlexWrap(node, box->getWrap() ? YGWrapWrap : YGWrapNoWrap);
                return;
            }

            if (isGridParent(widget)) {
                //the grid solves as a column of anonymous row nodes; the column
                //gap lives on the rows, the row gap on this node
                YGNodeStyleSetFlexDirection(node, YGFlexDirectionColumn);
                YGNodeStyleSetJustifyContent(node, YGJustifyFlexStart);
                YGNodeStyleSetAlignItems(node, YGAlignStretch);
                YGNodeStyleSetFlexWrap(node, YGWrapNoWrap);
                return;
            }

            if (isIslandParent(widget)) {
                //children are islands; flow props of this node are irrelevant
                YGNodeStyleSetOverflow(node, YGOverflowHidden);
            }
        }

        void applySlot(const Widget& widget, YGNodeRef node) {
            const Slot& slot = widget.getSlot();
            const WidgetStyle& style = widget.getStyle();

            const Orientation flow = parentOrientation(widget);
            const bool horizontal = flow == Orientation::Horizontal;
            const YGEdge mainStart = horizontal ? YGEdgeLeft : YGEdgeTop;
            const YGEdge mainEnd = horizontal ? YGEdgeRight : YGEdgeBottom;

            YGNodeStyleSetPositionType(node,
                slot.absolute ? YGPositionTypeAbsolute : YGPositionTypeRelative);

            YGNodeStyleSetFlexGrow(node, slot.grow);
            YGNodeStyleSetFlexShrink(node, slot.shrink);
            if (slot.basis >= 0.0f) {
                YGNodeStyleSetFlexBasis(node, slot.basis);
            } else {
                YGNodeStyleSetFlexBasisAuto(node);
            }

            YGNodeStyleSetMargin(node, YGEdgeLeft, slot.margin.left);
            YGNodeStyleSetMargin(node, YGEdgeTop, slot.margin.top);
            YGNodeStyleSetMargin(node, YGEdgeRight, slot.margin.right);
            YGNodeStyleSetMargin(node, YGEdgeBottom, slot.margin.bottom);

            float minWidth = std::max(slot.minSize.x, style.minSize.x);
            float minHeight = std::max(slot.minSize.y, style.minSize.y);
            YGNodeStyleSetMinWidth(node, minWidth > 0.0f ? minWidth : 0.0f);
            YGNodeStyleSetMinHeight(node, minHeight > 0.0f ? minHeight : 0.0f);

            //0 means unlimited in both Slot::maxSize and WidgetStyle::maxSize
            float maxWidth = YGUndefined;
            if (slot.maxSize.x > 0.0f && style.maxSize.x > 0.0f) {
                maxWidth = std::min(slot.maxSize.x, style.maxSize.x);
            } else if (slot.maxSize.x > 0.0f) {
                maxWidth = slot.maxSize.x;
            } else if (style.maxSize.x > 0.0f) {
                maxWidth = style.maxSize.x;
            }
            YGNodeStyleSetMaxWidth(node, maxWidth);

            float maxHeight = YGUndefined;
            if (slot.maxSize.y > 0.0f && style.maxSize.y > 0.0f) {
                maxHeight = std::min(slot.maxSize.y, style.maxSize.y);
            } else if (slot.maxSize.y > 0.0f) {
                maxHeight = slot.maxSize.y;
            } else if (style.maxSize.y > 0.0f) {
                maxHeight = style.maxSize.y;
            }
            YGNodeStyleSetMaxHeight(node, maxHeight);

            //Flexbox has no per-item main-axis alignment, so the slot's main
            //axis is expressed with auto margins (the CSS idiom); the auto
            //margin overrides the slot margin on that edge, as in CSS.
            const Align main = mainAlignOf(slot, horizontal);
            if (!slot.absolute && main == Align::Center) {
                YGNodeStyleSetMarginAuto(node, mainStart);
                YGNodeStyleSetMarginAuto(node, mainEnd);
            } else if (!slot.absolute && main == Align::End) {
                YGNodeStyleSetMarginAuto(node, mainStart);
            }

            //Cross axis: Fill means "defer to the container's align-items",
            //which is exactly align-self: auto in Yoga; anything else is an
            //explicit override.
            if (slot.absolute) {
                YGNodeStyleSetAlignSelf(node, YGAlignAuto);
            } else {
                const Align cross = crossAlignOf(slot, horizontal);
                const bool explicitCross = horizontal
                    ? slot.vAlign != VAlign::Fill
                    : slot.hAlign != HAlign::Fill;
                YGNodeStyleSetAlignSelf(node, explicitCross ? toYoga(cross) : YGAlignAuto);
            }
        }

        void applyStyleProps(const Widget& widget, YGNodeRef node, bool withPadding) {
            const WidgetStyle& style = widget.getStyle();

            //Measure nodes fold padding into their own measure(); only nodes
            //whose children are real Yoga children need Yoga-side padding.
            if (withPadding) {
                const Insets& padding = style.padding;
                YGNodeStyleSetPadding(node, YGEdgeLeft, padding.left);
                YGNodeStyleSetPadding(node, YGEdgeTop, padding.top);
                YGNodeStyleSetPadding(node, YGEdgeRight, padding.right);
                YGNodeStyleSetPadding(node, YGEdgeBottom, padding.bottom);
            }

            for (int i = 0; i < 4; i++) {
                const YGEdge edges[4] = { YGEdgeLeft, YGEdgeTop, YGEdgeRight, YGEdgeBottom };
                YGNodeStyleSetBorder(node, edges[i], style.borderWidth);
            }
        }

        //Per-grid anonymous row nodes; the engine owns them, not the widgets.
        //A row is (grid widget, row index); its children are the cell widget
        //nodes ordered by column.
        std::map<Widget*, std::vector<YGNodeRef>> g_gridRows;

        //Per-ScrollBox anonymous content group. The viewport is an island, so
        //its children never enter the parent tree; they stack inside this one
        //engine-owned node, solved at natural size along the scroll axis.
        std::map<Widget*, YGNodeRef> g_scrollGroups;

        void freeGridRows(Widget* grid) {
            auto it = g_gridRows.find(grid);
            if (it == g_gridRows.end()) {
                return;
            }
            for (size_t i = 0; i < it->second.size(); i++) {
                YGNodeRef row = it->second[i];
                //cell nodes are freed earlier (post-order) and detach
                //themselves; anything still listed is a stale reference
                YGNodeRemoveAllChildren(row);
                YGNodeFree(row);
            }
            g_gridRows.erase(it);
        }

        void freeScrollGroup(Widget* scroll) {
            auto it = g_scrollGroups.find(scroll);
            if (it == g_scrollGroups.end()) {
                return;
            }
            YGNodeRemoveAllChildren(it->second);
            YGNodeFree(it->second);
            g_scrollGroups.erase(it);
        }

        //Mirror of the flow state GridPanel publishes into its children (see
        //the contract note below).
        struct SlateGridCell {
            unsigned int row;
            unsigned int column;
            unsigned int rowSpan;
            unsigned int columnSpan;
            float rowWeight;
            float columnWeight;
            float gapX;
            float gapY;
        };

        //GridPanel keeps cells/weights/gaps private and Containers.h has no
        //updateLayoutChildren hook like the sibling module's VisualElement, so
        //GridPanel's mirrorGridFlow (Containers.cpp) publishes the resolved
        //flow into each child's public state: anchors = (row, column, rowSpan,
        //columnSpan), pivot = (rowWeight, columnWeight), offsets = (gapX,
        //gapY) and slot grow/basis. This is the whole cross-file contract; both
        //sides live in layout/.
        bool readGridCell(const Widget& child, SlateGridCell& out) {
            const AnchorData& anchor = child.getAnchorData();
            out.row = static_cast<unsigned int>(std::max(0.0f, anchor.anchors.x));
            out.column = static_cast<unsigned int>(std::max(0.0f, anchor.anchors.y));
            out.rowSpan = std::max(1u, static_cast<unsigned int>(anchor.anchors.z));
            out.columnSpan = std::max(1u, static_cast<unsigned int>(anchor.anchors.w));
            out.rowWeight = std::max(0.0f, anchor.pivot.x);
            out.columnWeight = std::max(0.0f, anchor.pivot.y);
            out.gapX = std::max(0.0f, anchor.offsets.x);
            out.gapY = std::max(0.0f, anchor.offsets.y);
            return true;
        }

        void syncGridRows(Widget& widget, YGNodeRef node) {
            freeGridRows(&widget);
            YGNodeRemoveAllChildren(node);

            const std::vector<std::unique_ptr<Widget>>& children = widget.getChildren();
            if (children.empty()) {
                return;
            }

            struct Placed {
                unsigned int row;
                unsigned int column;
                float rowWeight;
                Widget* widget;
            };

            unsigned int rowCount = 0;
            std::vector<Placed> placed;
            placed.reserve(children.size());
            for (size_t i = 0; i < children.size(); i++) {
                SlateGridCell cell;
                readGridCell(*children[i], cell);
                Placed p;
                p.row = cell.row;
                p.column = cell.column;
                p.rowWeight = cell.rowWeight;
                p.widget = children[i].get();
                placed.push_back(p);
                if (p.row + 1u > rowCount) {
                    rowCount = p.row + 1u;
                }
            }
            const GridPanel* grid = static_cast<GridPanel*>(&widget);
            if (grid->getRows() > rowCount) {
                rowCount = grid->getRows();
            }

            float gapY = 0.0f;
            float gapX = 0.0f;
            {
                SlateGridCell first;
                readGridCell(*children[0], first);
                gapX = first.gapX;
                gapY = first.gapY;
            }
            YGNodeStyleSetGap(node, YGGutterRow, gapY);

            //rows read top to bottom, cells left to right, insertion-stable
            std::stable_sort(placed.begin(), placed.end(),
                [](const Placed& a, const Placed& b) {
                    if (a.row != b.row) {
                        return a.row < b.row;
                    }
                    return a.column < b.column;
                });

            for (unsigned int r = 0; r < rowCount; r++) {
                YGNodeRef row = YGNodeNewWithConfig(getConfig());
                YGNodeSetContext(row, nullptr);   //anonymous: never measured
                YGNodeStyleSetFlexDirection(row, YGFlexDirectionRow);
                YGNodeStyleSetJustifyContent(row, YGJustifyFlexStart);
                YGNodeStyleSetAlignItems(row, YGAlignStretch);
                YGNodeStyleSetFlexWrap(row, YGWrapNoWrap);
                YGNodeStyleSetGap(row, YGGutterColumn, gapX);
                //a row must not shrink below the content it carries
                YGNodeStyleSetFlexShrink(row, 0.0f);
                g_gridRows[&widget].push_back(row);
                YGNodeInsertChild(node, row, YGNodeGetChildCount(node));

                float weight = 0.0f;
                for (size_t i = 0; i < placed.size(); i++) {
                    if (placed[i].row != r) {
                        continue;
                    }
                    YGNodeRef childNode = ensureNode(*placed[i].widget);
                    YGNodeRef owner = YGNodeGetOwner(childNode);
                    if (owner != nullptr && owner != row) {
                        YGNodeRemoveChild(owner, childNode);
                    }
                    if (YGNodeGetOwner(childNode) == nullptr) {
                        YGNodeInsertChild(row, childNode, YGNodeGetChildCount(row));
                    }
                    if (placed[i].rowWeight > weight) {
                        weight = placed[i].rowWeight;
                    }
                }

                //a weighted row shares the leftover height; the anonymous node
                //carries it because Yoga reads the row, not the cell slots
                if (weight > 0.0f) {
                    YGNodeStyleSetFlexGrow(row, weight);
                    YGNodeStyleSetFlexBasis(row, 0.0f);
                } else {
                    YGNodeStyleSetFlexGrow(row, 0.0f);
                    YGNodeStyleSetFlexBasisAuto(row);
                }
            }
        }

        void detachFromTree(Widget& child) {
            YGNodeRef node = child.getLayoutNode();
            if (node == nullptr) {
                return;
            }
            YGNodeRef owner = YGNodeGetOwner(node);
            if (owner != nullptr) {
                YGNodeRemoveChild(owner, node);
            }
        }

        void syncChildren(Widget& widget, YGNodeRef node) {
            if (YGNodeHasMeasureFunc(node)) {
                YGNodeSetMeasureFunc(node, nullptr);
            }

            if (isIslandParent(widget)) {
                //island children are independent roots: evict any node that a
                //previous parent placement attached here
                const std::vector<std::unique_ptr<Widget>>& children = widget.getChildren();
                for (size_t i = 0; i < children.size(); i++) {
                    detachFromTree(*children[i]);
                }
                YGNodeRemoveAllChildren(node);
                return;
            }

            if (isGridParent(widget)) {
                syncGridRows(widget, node);
                return;
            }

            const std::vector<std::unique_ptr<Widget>>& children = widget.getChildren();
            bool matches = YGNodeGetChildCount(node) == static_cast<uint32_t>(children.size());
            if (matches) {
                for (size_t i = 0; i < children.size(); i++) {
                    if (children[i]->getLayoutNode() != YGNodeGetChild(node, static_cast<uint32_t>(i))) {
                        matches = false;
                        break;
                    }
                }
            }
            if (matches) {
                return;
            }

            YGNodeRemoveAllChildren(node);
            for (size_t i = 0; i < children.size(); i++) {
                YGNodeRef childNode = ensureNode(*children[i]);
                YGNodeRef owner = YGNodeGetOwner(childNode);
                if (owner != nullptr) {
                    YGNodeRemoveChild(owner, childNode);
                }
                YGNodeInsertChild(node, childNode, YGNodeGetChildCount(node));
            }
        }

        bool syncWidget(Widget& widget) {
            bool changed = false;

            if ((widget.getDirtyFlags() & WidgetDirty::Layout) != WidgetDirty::None ||
                widget.getLayoutNode() == nullptr) {
                changed = true;

                YGNodeRef node = ensureNode(widget);
                YGNodeStyleSetDisplay(node, widget.isVisible() ? YGDisplayFlex : YGDisplayNone);

                //the other module runs updateLayoutChildren() here; with the
                //headers frozen the container's measure() is the only public
                //entry point that can restyle child slots, and dirty containers
                //get exactly one such call per pass
                if (widget.isContainer() && !isIslandParent(widget)) {
                    widget.measure(Vector2(-1.0f, -1.0f));
                }

                applyContainerFlow(widget, node);
                applySlot(widget, node);
                syncChildren(widget, node);

                //Yoga forbids a measure function on a node with children, and
                //the child count only settles after syncChildren()
                if (YGNodeGetChildCount(node) == 0) {
                    YGNodeSetContext(node, &widget);
                    YGNodeSetMeasureFunc(node, measureCallback);
                    YGNodeMarkDirty(node);
                    applyStyleProps(widget, node, false);
                } else {
                    applyStyleProps(widget, node, true);
                }

                widget.clearDirtyFlags(WidgetDirty::Layout);
            }

            //island subtrees are synced by their own solve; walking them twice
            //would clear the flags before the island pass could see them
            if (!isIslandParent(widget)) {
                const std::vector<std::unique_ptr<Widget>>& children = widget.getChildren();
                for (size_t i = 0; i < children.size(); i++) {
                    changed = syncWidget(*children[i]) || changed;
                }
            }

            return changed;
        }

        void writeWidget(Widget& widget, YGNodeRef node, float originX, float originY);
        void solveIsland(Widget& widget, float availableWidth, float availableHeight,
                         float originX, float originY);
        void solveScrollGroup(ScrollBox& scroll, const Rect& rect);

        void writeIslands(Widget& widget, const Rect& rect) {
            const std::vector<std::unique_ptr<Widget>>& children = widget.getChildren();

            if (dynamic_cast<CanvasPanel*>(&widget) != nullptr) {
                for (size_t i = 0; i < children.size(); i++) {
                    Widget* child = children[i].get();
                    Rect slotRect = resolveAnchorRect(child->getAnchorData(), rect);
                    solveIsland(*child, slotRect.getWidth(), slotRect.getHeight(),
                        slotRect.getX(), slotRect.getY());
                }
                return;
            }

            //Overlay and SizeBox pin every child to their own rect: a flex child
            //cannot be told to fill the main axis by its container, and these
            //two containers mean "fill".
            if (dynamic_cast<Overlay*>(&widget) != nullptr ||
                dynamic_cast<SizeBox*>(&widget) != nullptr) {
                for (size_t i = 0; i < children.size(); i++) {
                    solveIsland(*children[i], rect.getWidth(), rect.getHeight(),
                        rect.getX(), rect.getY());
                }
                return;
            }

            if (ScrollBox* scroll = dynamic_cast<ScrollBox*>(&widget)) {
                if (children.empty()) {
                    freeScrollGroup(&widget);
                } else {
                    //the content is the viewport's own children (the frozen
                    //Containers.h cannot declare the addChild redirect that would
                    //move them into an inner Box), so they stack in one
                    //engine-owned group node solved at natural size
                    solveScrollGroup(*scroll, rect);
                }
                return;
            }

            if (ScaleBox* scale = dynamic_cast<ScaleBox*>(&widget)) {
                if (!children.empty()) {
                    //an explicit-size ScaleBox is never measured by Yoga, so
                    //the paint scale is refreshed here from the solved rect
                    scale->measure(Vector2(rect.getWidth(), rect.getHeight()));
                    const Vector2 design = scale->getDesignSize();
                    solveIsland(*children[0], design.x, design.y, rect.getX(), rect.getY());
                }
                return;
            }
        }

        void writeWidget(Widget& widget, YGNodeRef node, float originX, float originY) {
            const float left = originX + definedValue(YGNodeLayoutGetLeft(node));
            const float top = originY + definedValue(YGNodeLayoutGetTop(node));
            const float width = definedValue(YGNodeLayoutGetWidth(node));
            const float height = definedValue(YGNodeLayoutGetHeight(node));

            const Rect rect(left, top, width, height);
            widget.setLayoutRect(rect);
            widget.clearDirtyFlags(WidgetDirty::Layout);

            if (isIslandParent(widget)) {
                writeIslands(widget, rect);
                return;
            }

            auto rowsIt = g_gridRows.find(&widget);
            if (rowsIt != g_gridRows.end()) {
                //the cell nodes hang under the anonymous row nodes, so their
                //layout offsets resolve against the row origin
                for (size_t r = 0; r < rowsIt->second.size(); r++) {
                    YGNodeRef row = rowsIt->second[r];
                    const float rowX = left + definedValue(YGNodeLayoutGetLeft(row));
                    const float rowY = top + definedValue(YGNodeLayoutGetTop(row));
                    const uint32_t childCount = YGNodeGetChildCount(row);
                    for (uint32_t c = 0; c < childCount; c++) {
                        YGNodeRef childNode = YGNodeGetChild(row, c);
                        Widget* child = static_cast<Widget*>(YGNodeGetContext(childNode));
                        if (child != nullptr) {
                            writeWidget(*child, childNode, rowX, rowY);
                        }
                    }
                }
                return;
            }

            const std::vector<std::unique_ptr<Widget>>& children = widget.getChildren();
            for (size_t i = 0; i < children.size(); i++) {
                YGNodeRef childNode = children[i]->getLayoutNode();
                if (childNode == nullptr) {
                    continue;
                }
                writeWidget(*children[i], childNode, left, top);
            }
        }

        void solveIsland(Widget& widget, float availableWidth, float availableHeight,
                         float originX, float originY) {
            syncWidget(widget);
            YGNodeRef node = widget.getLayoutNode();
            if (node == nullptr) {
                return;
            }

            if (availableWidth >= 0.0f) {
                YGNodeStyleSetWidth(node, availableWidth);
            } else {
                YGNodeStyleSetWidthAuto(node);
            }
            if (availableHeight >= 0.0f) {
                YGNodeStyleSetHeight(node, availableHeight);
            } else {
                YGNodeStyleSetHeightAuto(node);
            }

            //islands always re-solve: the scroll offset and the canvas anchors
            //rewrite child rects even when no child widget is dirty
            YGNodeCalculateLayout(node,
                islandAvailable(availableWidth), islandAvailable(availableHeight),
                YGDirectionLTR);
            writeWidget(widget, node, originX, originY);
        }

        void solveScrollGroup(ScrollBox& scroll, const Rect& rect) {
            const std::vector<std::unique_ptr<Widget>>& children = scroll.getChildren();

            YGNodeRef& slotRef = g_scrollGroups[&scroll];
            YGNodeRef group = slotRef;
            if (group == nullptr) {
                group = YGNodeNewWithConfig(getConfig());
                YGNodeSetContext(group, nullptr);   //anonymous: never measured
                slotRef = group;
            }

            const bool horizontal = scroll.getOrientation() == Orientation::Horizontal;
            //Containers.h has no gap field on ScrollBox, so the widget keeps the
            //content gap in its own anchor pivot (see Containers.cpp)
            const float gap = std::max(0.0f, scroll.getAnchorData().pivot.x);

            YGNodeStyleSetFlexDirection(group,
                horizontal ? YGFlexDirectionRow : YGFlexDirectionColumn);
            YGNodeStyleSetJustifyContent(group, YGJustifyFlexStart);
            YGNodeStyleSetAlignItems(group, YGAlignStretch);
            YGNodeStyleSetFlexWrap(group, YGWrapNoWrap);
            YGNodeStyleSetGap(group, YGGutterAll, gap);

            //the viewport's own padding is part of its rect, so the content
            //group is pinned to the inner box
            const WidgetStyle& style = scroll.getStyle();
            const float innerX = rect.getX() + style.padding.left;
            const float innerY = rect.getY() + style.padding.top;
            const float innerWidth = std::max(0.0f,
                rect.getWidth() - style.padding.left - style.padding.right);
            const float innerHeight = std::max(0.0f,
                rect.getHeight() - style.padding.top - style.padding.bottom);

            YGNodeRemoveAllChildren(group);
            for (size_t i = 0; i < children.size(); i++) {
                if (!children[i]->isVisible()) {
                    continue;
                }
                syncWidget(*children[i]);
                YGNodeRef childNode = ensureNode(*children[i]);
                detachFromTree(*children[i]);
                YGNodeInsertChild(group, childNode, YGNodeGetChildCount(group));
            }

            //the scroll axis stays auto so the content reaches its natural size
            const float availableWidth = horizontal ? YGUndefined : innerWidth;
            const float availableHeight = horizontal ? innerHeight : YGUndefined;
            if (horizontal) {
                YGNodeStyleSetWidthAuto(group);
                YGNodeStyleSetHeight(group, innerHeight);
            } else {
                YGNodeStyleSetWidth(group, innerWidth);
                YGNodeStyleSetHeightAuto(group);
            }

            const Vector2 offset = scroll.getContentOffset();
            YGNodeCalculateLayout(group, availableWidth, availableHeight, YGDirectionLTR);

            const float originX = innerX - offset.x;
            const float originY = innerY - offset.y;
            const uint32_t childCount = YGNodeGetChildCount(group);
            for (uint32_t c = 0; c < childCount; c++) {
                YGNodeRef childNode = YGNodeGetChild(group, c);
                Widget* child = static_cast<Widget*>(YGNodeGetContext(childNode));
                if (child != nullptr) {
                    writeWidget(*child, childNode, originX, originY);
                }
            }
        }

        bool solveTree(Widget& root, float canvasWidth, float canvasHeight) {
            const bool changed = syncWidget(root);
            YGNodeRef rootNode = root.getLayoutNode();
            if (rootNode == nullptr) {
                return false;
            }

            //the canvas is the containing block of the tree root, so percent
            //bases and stretched children resolve against it
            YGNodeStyleSetWidth(rootNode, canvasWidth);
            YGNodeStyleSetHeight(rootNode, canvasHeight);

            //re-solving a clean tree with an unchanged canvas is pure waste;
            //the first pass is caught by hasNewLayout still being false
            if (changed || YGNodeIsDirty(rootNode) || !YGNodeGetHasNewLayout(rootNode)) {
                YGNodeCalculateLayout(rootNode, canvasWidth, canvasHeight, YGDirectionLTR);
                writeWidget(root, rootNode, 0.0f, 0.0f);
            }
            return true;
        }

        void disposeWidget(Widget& widget) {
            const std::vector<std::unique_ptr<Widget>>& children = widget.getChildren();
            for (size_t i = 0; i < children.size(); i++) {
                disposeWidget(*children[i]);
            }

            //anonymous rows are referenced only by the grid node, so they go
            //first; widget nodes detach themselves from their owner on free
            freeGridRows(&widget);
            freeScrollGroup(&widget);

            YGNodeRef node = widget.getLayoutNode();
            if (node != nullptr) {
                YGNodeFree(node);
                widget.setLayoutNode(nullptr);
            }
        }
    }

    bool FlexEngine::layout(Widget& root, float canvasWidth, float canvasHeight) {
        if (!(canvasWidth > 0.0f) || !(canvasHeight > 0.0f)) {
            return false;
        }
        return solveTree(root, canvasWidth, canvasHeight);
    }

    void FlexEngine::dispose(Widget& root) {
        disposeWidget(root);
    }
}
