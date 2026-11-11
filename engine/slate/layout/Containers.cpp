// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "slate/layout/Containers.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace quantum::slate {

    namespace {

        //Same clamp ladder Widget.cpp uses: style first, then slot.
        void clampSize(float& width, float& height, const Vector2& minSize, const Vector2& maxSize) {
            if (maxSize.x > 0.0f) {
                width = std::min(width, maxSize.x);
            }
            if (maxSize.y > 0.0f) {
                height = std::min(height, maxSize.y);
            }
            width = std::max(width, minSize.x);
            height = std::max(height, minSize.y);
        }

        Vector2 childAvailable(Vector2 available, const Insets& padding) {
            return Vector2(available.x >= 0.0f ? available.x - padding.left - padding.right : -1.0f,
                available.y >= 0.0f ? available.y - padding.top - padding.bottom : -1.0f);
        }

        //Largest visible child: the content extent the size-to-content
        //containers (Overlay, unoverridden SizeBox) fall back to.
        Vector2 largestChildSize(const std::vector<std::unique_ptr<Widget>>& kids, Vector2 inner) {
            float width = 0.0f;
            float height = 0.0f;
            for (size_t i = 0; i < kids.size(); i++) {
                if (!kids[i]->isVisible()) {
                    continue;
                }
                const Vector2 size = kids[i]->measure(inner);
                width = std::max(width, size.x);
                height = std::max(height, size.y);
            }
            return Vector2(width, height);
        }

        //Local recursion so ScrollBox::paint can hand the whole content tree
        //to the clip without depending on the (not yet written) frame walker.
        void paintSubtree(Widget& widget, PaintContext& context) {
            if (!widget.isVisible()) {
                return;
            }
            widget.paint(context);
            const std::vector<std::unique_ptr<Widget>>& children = widget.getChildren();
            for (size_t i = 0; i < children.size(); i++) {
                paintSubtree(*children[i], context);
            }
        }

        //Row-major fallback for children added without an explicit Cell.
        GridPanel::Cell defaultGridCell(unsigned int index, unsigned int columns) {
            GridPanel::Cell cell;
            if (columns > 0) {
                cell.row = index / columns;
                cell.column = index % columns;
            } else {
                cell.row = 0;
                cell.column = index;
            }
            cell.rowSpan = 1u;
            cell.columnSpan = 1u;
            return cell;
        }

        float weightSpan(const std::vector<float>& weights, unsigned int first, unsigned int span) {
            unsigned int last = first + (span > 0 ? span : 1u);
            float sum = 0.0f;
            for (unsigned int i = first; i < last && i < weights.size(); i++) {
                sum += weights[i];
            }
            return sum;
        }

        //The frozen Containers.h gives GridPanel no protected restyle hook (the
        //sibling module has updateLayoutChildren) and keeps cells/weights/gaps
        //private, so the resolved flow is published into each child's public
        //state instead: anchors = (row, column, rowSpan, columnSpan),
        //pivot = (rowWeight, columnWeight), offsets = (gapX, gapY) and
        //slot grow/basis. FlexEngine reads exactly that mirror to build the
        //anonymous row nodes; a plain addChild therefore behaves like a
        //row-major addAt. Mixing addAt and addChild on the same grid shifts the
        //explicit cells of the later children. The grid's private members are
        //fed in as arguments by the calling member functions, which is what
        //lets this live outside the class.
        void mirrorGridFlow(GridPanel& grid, unsigned int columns, float gapX, float gapY,
            const std::vector<GridPanel::Cell>& cells,
            const std::vector<float>& columnWeights,
            const std::vector<float>& rowWeights) {
            const std::vector<std::unique_ptr<Widget>>& children = grid.getChildren();
            for (size_t i = 0; i < children.size(); i++) {
                Widget* child = children[i].get();

                GridPanel::Cell cell = i < cells.size()
                    ? cells[i]
                    : defaultGridCell(static_cast<unsigned int>(i), columns);
                if (cell.rowSpan == 0u) {
                    cell.rowSpan = 1u;
                }
                if (cell.columnSpan == 0u) {
                    cell.columnSpan = 1u;
                }
                //rowSpan consumes no extra column; a cell may not reach past the
                //declared columns either
                unsigned int columnCount = columns > 0 ? columns : cell.column + cell.columnSpan;
                if (cell.column >= columnCount) {
                    cell.column = columnCount > 0 ? columnCount - 1u : 0u;
                }
                if (cell.column + cell.columnSpan > columnCount) {
                    cell.columnSpan = columnCount - cell.column;
                }

                const float columnWeight = weightSpan(columnWeights, cell.column, cell.columnSpan);
                const float rowWeight = weightSpan(rowWeights, cell.row, cell.rowSpan);

                AnchorData& anchor = child->getAnchorData();
                anchor.anchors = Vector4(static_cast<float>(cell.row), static_cast<float>(cell.column),
                    static_cast<float>(cell.rowSpan), static_cast<float>(cell.columnSpan));
                anchor.pivot = Vector2(rowWeight, columnWeight);
                anchor.offsets = Vector4(gapX, gapY, 0.0f, 0.0f);

                Slot& targetSlot = child->getSlot();
                targetSlot.grow = columnWeight;
                targetSlot.basis = columnWeight > 0.0f ? 0.0f : -1.0f;

                child->markDirty(WidgetDirty::Layout);
            }

            //row weights live on the anonymous row nodes FlexEngine builds from
            //this mirror, so the grid node itself must be rebuilt on every change
            grid.markDirty(WidgetDirty::Layout);
        }
    }

    // ---------------------------------------------------------------------------
    // Box
    // ---------------------------------------------------------------------------

    Box::Box() : Box(Orientation::Horizontal, std::string()) {
    }

    const char* Box::getTypeName() const { return "Box"; }

    Box::Box(Orientation boxOrientation, std::string boxId) : Widget(std::move(boxId)),
        orientation(boxOrientation),
        justify(Justify::Start),
        alignItems(Align::Stretch),
        gap(0.0f),
        wrap(false) {
    }

    Box::Box(std::string boxId) : Box(Orientation::Horizontal, std::move(boxId)) {
    }

    bool Box::isContainer() const {
        return true;
    }

    Box& Box::setOrientation(Orientation newOrientation) {
        if (orientation != newOrientation) {
            orientation = newOrientation;
            markSubtreeDirty(WidgetDirty::Layout);
        }
        return *this;
    }

    Orientation Box::getOrientation() const {
        return orientation;
    }

    Box& Box::setJustify(Justify newJustify) {
        if (justify != newJustify) {
            justify = newJustify;
            markSubtreeDirty(WidgetDirty::Layout);
        }
        return *this;
    }

    Justify Box::getJustify() const {
        return justify;
    }

    Box& Box::setAlignItems(Align align) {
        if (alignItems != align) {
            alignItems = align;
            markSubtreeDirty(WidgetDirty::Layout);
        }
        return *this;
    }

    Align Box::getAlignItems() const {
        return alignItems;
    }

    Box& Box::setGap(float newGap) {
        newGap = std::max(0.0f, newGap);
        if (gap != newGap) {
            gap = newGap;
            markSubtreeDirty(WidgetDirty::Layout);
        }
        return *this;
    }

    float Box::getGap() const {
        return gap;
    }

    Box& Box::setWrap(bool newWrap) {
        if (wrap != newWrap) {
            wrap = newWrap;
            markSubtreeDirty(WidgetDirty::Layout);
        }
        return *this;
    }

    bool Box::getWrap() const {
        return wrap;
    }

    Vector2 Box::measure(Vector2 available) {
        const WidgetStyle& style = getStyle();
        const float padX = style.padding.left + style.padding.right;
        const float padY = style.padding.top + style.padding.bottom;
        const Vector2 inner = childAvailable(available, style.padding);

        const bool horizontal = orientation == Orientation::Horizontal;
        float mainTotal = 0.0f;
        float crossMax = 0.0f;
        unsigned int counted = 0;

        for (size_t i = 0; i < children.size(); i++) {
            Widget* child = children[i].get();
            if (!child->isVisible()) {
                continue;
            }
            const Vector2 size = child->measure(inner);
            const float main = horizontal ? size.x : size.y;
            const float cross = horizontal ? size.y : size.x;
            mainTotal += main + (counted > 0 ? gap : 0.0f);
            crossMax = std::max(crossMax, cross);
            counted++;
        }

        float width = (horizontal ? mainTotal : crossMax) + padX;
        float height = (horizontal ? crossMax : mainTotal) + padY;

        clampSize(width, height, style.minSize, style.maxSize);
        clampSize(width, height, slot.minSize, slot.maxSize);
        if (available.x >= 0.0f) {
            width = std::min(width, available.x);
        }
        if (available.y >= 0.0f) {
            height = std::min(height, available.y);
        }

        desiredSize = Vector2(width, height);
        return desiredSize;
    }

    //Manual single-line placement. The documented path is FlexEngine: Yoga
    //solves every widget that lives inside a laid-out tree, and this arrange
    //only runs when a container is placed directly (the fallback below a
    //CanvasPanel's manual arrange, or a standalone call outside FlexEngine).
    void Box::arrange(const Rect& rect) {
        setLayoutRect(rect);
        clearDirtyFlags(WidgetDirty::Layout);

        const Rect content = contentRect(rect);
        const bool horizontal = orientation == Orientation::Horizontal;

        float totalMain = 0.0f;
        float totalGrow = 0.0f;
        unsigned int counted = 0;

        for (size_t i = 0; i < children.size(); i++) {
            Widget* child = children[i].get();
            if (!child->isVisible()) {
                continue;
            }
            const Vector2 size = child->measure(Vector2(content.getWidth(), content.getHeight()));
            const float main = horizontal ? size.x : size.y;
            totalMain += main + (counted > 0 ? gap : 0.0f);
            totalGrow += child->getSlot().grow;
            counted++;
        }

        const float contentMain = horizontal ? content.getWidth() : content.getHeight();
        float extraGap = 0.0f;
        float cursor = 0.0f;

        if (counted > 0) {
            if (justify == Justify::Center) {
                cursor = std::max(0.0f, (contentMain - totalMain) * 0.5f);
            } else if (justify == Justify::End) {
                cursor = std::max(0.0f, contentMain - totalMain);
            } else if (justify == Justify::SpaceBetween || justify == Justify::SpaceAround ||
                justify == Justify::SpaceEvenly) {
                float dividers = static_cast<float>(counted) - 1.0f;
                if (justify == Justify::SpaceAround) {
                    dividers = static_cast<float>(counted);
                } else if (justify == Justify::SpaceEvenly) {
                    dividers = static_cast<float>(counted) + 1.0f;
                }
                if (dividers > 0.0f) {
                    extraGap = std::max(0.0f, contentMain - totalMain) / dividers;
                }
                if (justify != Justify::SpaceBetween) {
                    cursor = extraGap;
                }
            }
        }

        for (size_t i = 0; i < children.size(); i++) {
            Widget* child = children[i].get();
            if (!child->isVisible()) {
                continue;
            }

            const Vector2 size = child->measure(Vector2(content.getWidth(), content.getHeight()));
            float main = horizontal ? size.x : size.y;
            if (totalGrow > 0.0f && contentMain > totalMain) {
                main += (contentMain - totalMain) * child->getSlot().grow / totalGrow;
            }
            const float cross = horizontal ? size.y : size.x;

            //slot Fill defers to the container's align-items; an explicit slot
            //alignment wins over it, matching the Yoga-side mapping
            Align align = alignItems;
            if (horizontal) {
                if (child->getSlot().vAlign != VAlign::Fill) {
                    align = child->getSlot().vAlign == VAlign::Top ? Align::Start
                        : child->getSlot().vAlign == VAlign::Center ? Align::Center : Align::End;
                }
            } else if (child->getSlot().hAlign != HAlign::Fill) {
                align = child->getSlot().hAlign == HAlign::Left ? Align::Start
                    : child->getSlot().hAlign == HAlign::Center ? Align::Center : Align::End;
            }

            const float contentCross = horizontal ? content.getHeight() : content.getWidth();
            float crossSize = cross;
            float crossPos = 0.0f;
            if (align == Align::Stretch) {
                crossSize = contentCross;
            } else if (align == Align::Center) {
                crossPos = (contentCross - cross) * 0.5f;
            } else if (align == Align::End) {
                crossPos = contentCross - cross;
            }

            Rect childRect = rect;
            if (horizontal) {
                childRect = Rect(content.getX() + cursor, content.getY() + crossPos,
                    std::max(0.0f, main), std::max(0.0f, crossSize));
            } else {
                childRect = Rect(content.getX() + crossPos, content.getY() + cursor,
                    std::max(0.0f, crossSize), std::max(0.0f, main));
            }
            child->arrange(childRect);

            cursor += main + gap + extraGap;
        }
    }

    // ---------------------------------------------------------------------------
    // Overlay
    // ---------------------------------------------------------------------------

    Overlay::Overlay() : Overlay(std::string()) {
    }

    Overlay::Overlay(std::string overlayId) : Widget(std::move(overlayId)) {
    }

    bool Overlay::isContainer() const {
        return true;
    }

    Vector2 Overlay::measure(Vector2 available) {
        const WidgetStyle& style = getStyle();
        const Vector2 inner = childAvailable(available, style.padding);

        const Vector2 content = largestChildSize(children, inner);
        float width = content.x;
        float height = content.y;

        width += style.padding.left + style.padding.right;
        height += style.padding.top + style.padding.bottom;
        clampSize(width, height, style.minSize, style.maxSize);
        clampSize(width, height, slot.minSize, slot.maxSize);
        if (available.x >= 0.0f) {
            width = std::min(width, available.x);
        }
        if (available.y >= 0.0f) {
            height = std::min(height, available.y);
        }

        desiredSize = Vector2(width, height);
        return desiredSize;
    }

    //Manual fallback path; under FlexEngine the children are solved as islands
    //sharing this rect (see writeIslands in FlexEngine.cpp).
    void Overlay::arrange(const Rect& rect) {
        setLayoutRect(rect);
        clearDirtyFlags(WidgetDirty::Layout);

        const Rect content = contentRect(rect);
        for (size_t i = 0; i < children.size(); i++) {
            if (children[i]->isVisible()) {
                children[i]->arrange(content);
            }
        }
    }

    // ---------------------------------------------------------------------------
    // GridPanel
    // ---------------------------------------------------------------------------

    GridPanel::GridPanel() : GridPanel(std::string()) {
    }

    GridPanel::GridPanel(std::string gridId) : Widget(std::move(gridId)),
        columns(0),
        rows(0),
        gapX(0.0f),
        gapY(0.0f) {
    }

    bool GridPanel::isContainer() const {
        return true;
    }

    GridPanel& GridPanel::setColumns(unsigned int newColumns) {
        columns = newColumns;
        mirrorGridFlow(*this, columns, gapX, gapY, cells, columnWeights, rowWeights);
        markSubtreeDirty(WidgetDirty::Layout);
        return *this;
    }

    GridPanel& GridPanel::setRows(unsigned int newRows) {
        rows = newRows;
        mirrorGridFlow(*this, columns, gapX, gapY, cells, columnWeights, rowWeights);
        markSubtreeDirty(WidgetDirty::Layout);
        return *this;
    }

    unsigned int GridPanel::getColumns() const {
        return columns;
    }

    unsigned int GridPanel::getRows() const {
        return rows;
    }

    GridPanel& GridPanel::setGap(float horizontal, float vertical) {
        gapX = std::max(0.0f, horizontal);
        gapY = std::max(0.0f, vertical);
        mirrorGridFlow(*this, columns, gapX, gapY, cells, columnWeights, rowWeights);
        markSubtreeDirty(WidgetDirty::Layout);
        return *this;
    }

    GridPanel& GridPanel::setColumnWeight(unsigned int column, float weight) {
        if (column >= columnWeights.size()) {
            columnWeights.resize(column + 1, 0.0f);
        }
        columnWeights[column] = std::max(0.0f, weight);
        mirrorGridFlow(*this, columns, gapX, gapY, cells, columnWeights, rowWeights);
        markSubtreeDirty(WidgetDirty::Layout);
        return *this;
    }

    GridPanel& GridPanel::setRowWeight(unsigned int row, float weight) {
        if (row >= rowWeights.size()) {
            rowWeights.resize(row + 1, 0.0f);
        }
        rowWeights[row] = std::max(0.0f, weight);
        mirrorGridFlow(*this, columns, gapX, gapY, cells, columnWeights, rowWeights);
        markSubtreeDirty(WidgetDirty::Layout);
        return *this;
    }

    Vector2 GridPanel::measure(Vector2 available) {
        mirrorGridFlow(*this, columns, gapX, gapY, cells, columnWeights, rowWeights);

        const WidgetStyle& style = getStyle();
        const Vector2 inner = childAvailable(available, style.padding);

        unsigned int columnCount = columns;
        unsigned int rowCount = rows;
        for (size_t i = 0; i < children.size(); i++) {
            if (i >= cells.size()) {
                continue;
            }
            const Cell& cell = cells[i];
            columnCount = std::max(columnCount, cell.column + cell.columnSpan);
            rowCount = std::max(rowCount, cell.row + cell.rowSpan);
        }

        //weighted columns/rows start at zero and share the leftover, matching
        //the grow/basis pair mirrorGridFlow writes into the child slots
        std::vector<float> colSize(columnCount, 0.0f);
        std::vector<float> rowSize(rowCount, 0.0f);
        float colWeightTotal = 0.0f;
        float rowWeightTotal = 0.0f;

        for (size_t i = 0; i < children.size(); i++) {
            Widget* child = children[i].get();
            if (!child->isVisible() || i >= cells.size()) {
                continue;
            }
            const Cell& cell = cells[i];
            const Vector2 size = child->measure(inner);
            if (weightSpan(columnWeights, cell.column, cell.columnSpan) <= 0.0f && cell.column < columnCount) {
                colSize[cell.column] = std::max(colSize[cell.column], size.x);
            }
            if (weightSpan(rowWeights, cell.row, cell.rowSpan) <= 0.0f && cell.row < rowCount) {
                rowSize[cell.row] = std::max(rowSize[cell.row], size.y);
            }
        }
        for (unsigned int c = 0; c < columnCount; c++) {
            if (weightSpan(columnWeights, c, 1u) > 0.0f) {
                colWeightTotal += columnWeights[c];
            }
        }
        for (unsigned int r = 0; r < rowCount; r++) {
            if (weightSpan(rowWeights, r, 1u) > 0.0f) {
                rowWeightTotal += rowWeights[r];
            }
        }

        float width = columnCount > 0 ? gapX * static_cast<float>(columnCount - 1) : 0.0f;
        for (unsigned int c = 0; c < columnCount; c++) {
            width += colSize[c];
        }
        float height = rowCount > 0 ? gapY * static_cast<float>(rowCount - 1) : 0.0f;
        for (unsigned int r = 0; r < rowCount; r++) {
            height += rowSize[r];
        }

        if (inner.x >= 0.0f && colWeightTotal > 0.0f) {
            const float extra = std::max(0.0f, inner.x - width);
            for (unsigned int c = 0; c < columnCount; c++) {
                if (columnWeights.size() > c && columnWeights[c] > 0.0f) {
                    colSize[c] = extra * columnWeights[c] / colWeightTotal;
                    width += colSize[c];
                }
            }
        }
        if (inner.y >= 0.0f && rowWeightTotal > 0.0f) {
            const float extra = std::max(0.0f, inner.y - height);
            for (unsigned int r = 0; r < rowCount; r++) {
                if (rowWeights.size() > r && rowWeights[r] > 0.0f) {
                    rowSize[r] = extra * rowWeights[r] / rowWeightTotal;
                    height += rowSize[r];
                }
            }
        }

        width += style.padding.left + style.padding.right;
        height += style.padding.top + style.padding.bottom;
        clampSize(width, height, style.minSize, style.maxSize);
        clampSize(width, height, slot.minSize, slot.maxSize);

        desiredSize = Vector2(width, height);
        return desiredSize;
    }

    //Manual grid placement (the non-Yoga fallback); the sizes computed here
    //are also what measure() reports, arranged into per-child rects.
    void GridPanel::arrange(const Rect& rect) {
        mirrorGridFlow(*this, columns, gapX, gapY, cells, columnWeights, rowWeights);
        setLayoutRect(rect);
        clearDirtyFlags(WidgetDirty::Layout);

        const Rect content = contentRect(rect);

        unsigned int columnCount = columns;
        unsigned int rowCount = rows;
        for (size_t i = 0; i < children.size(); i++) {
            if (i >= cells.size()) {
                continue;
            }
            columnCount = std::max(columnCount, cells[i].column + cells[i].columnSpan);
            rowCount = std::max(rowCount, cells[i].row + cells[i].rowSpan);
        }

        std::vector<float> colSize(columnCount, 0.0f);
        std::vector<float> rowSize(rowCount, 0.0f);
        float colWeightTotal = 0.0f;
        float rowWeightTotal = 0.0f;
        for (size_t i = 0; i < children.size(); i++) {
            Widget* child = children[i].get();
            if (!child->isVisible() || i >= cells.size()) {
                continue;
            }
            const Cell& cell = cells[i];
            const Vector2 size = child->measure(Vector2(content.getWidth(), content.getHeight()));
            if (weightSpan(columnWeights, cell.column, cell.columnSpan) <= 0.0f && cell.column < columnCount) {
                colSize[cell.column] = std::max(colSize[cell.column], size.x);
            }
            if (weightSpan(rowWeights, cell.row, cell.rowSpan) <= 0.0f && cell.row < rowCount) {
                rowSize[cell.row] = std::max(rowSize[cell.row], size.y);
            }
        }
        for (unsigned int c = 0; c < columnCount; c++) {
            if (c < columnWeights.size() && columnWeights[c] > 0.0f) {
                colWeightTotal += columnWeights[c];
            }
        }
        for (unsigned int r = 0; r < rowCount; r++) {
            if (r < rowWeights.size() && rowWeights[r] > 0.0f) {
                rowWeightTotal += rowWeights[r];
            }
        }

        float usedW = columnCount > 1 ? gapX * static_cast<float>(columnCount - 1) : 0.0f;
        for (unsigned int c = 0; c < columnCount; c++) {
            usedW += colSize[c];
        }
        if (colWeightTotal > 0.0f && content.getWidth() > usedW) {
            const float extra = content.getWidth() - usedW;
            float usedExtra = 0.0f;
            for (unsigned int c = 0; c < columnCount; c++) {
                if (c < columnWeights.size() && columnWeights[c] > 0.0f) {
                    colSize[c] = extra * columnWeights[c] / colWeightTotal;
                    usedExtra += colSize[c];
                }
            }
            usedW += usedExtra;
        }

        float usedH = rowCount > 1 ? gapY * static_cast<float>(rowCount - 1) : 0.0f;
        for (unsigned int r = 0; r < rowCount; r++) {
            usedH += rowSize[r];
        }
        if (rowWeightTotal > 0.0f && content.getHeight() > usedH) {
            const float extra = content.getHeight() - usedH;
            for (unsigned int r = 0; r < rowCount; r++) {
                if (r < rowWeights.size() && rowWeights[r] > 0.0f) {
                    rowSize[r] = extra * rowWeights[r] / rowWeightTotal;
                }
            }
        }

        //prefix offsets make a spanning cell cover its inner gaps as well
        std::vector<float> colX(columnCount + 1, 0.0f);
        for (unsigned int c = 0; c < columnCount; c++) {
            colX[c + 1] = colX[c] + colSize[c] + gapX;
        }
        std::vector<float> rowY(rowCount + 1, 0.0f);
        for (unsigned int r = 0; r < rowCount; r++) {
            rowY[r + 1] = rowY[r] + rowSize[r] + gapY;
        }

        for (size_t i = 0; i < children.size(); i++) {
            Widget* child = children[i].get();
            if (!child->isVisible()) {
                continue;
            }
            Cell cell = i < cells.size() ? cells[i] : defaultGridCell(static_cast<unsigned int>(i), columns);
            if (cell.columnSpan == 0u) {
                cell.columnSpan = 1u;
            }
            if (cell.rowSpan == 0u) {
                cell.rowSpan = 1u;
            }
            if (cell.column >= columnCount || cell.row >= rowCount) {
                continue;
            }
            const unsigned int colEnd = std::min(cell.column + cell.columnSpan, columnCount);
            const unsigned int rowEnd = std::min(cell.row + cell.rowSpan, rowCount);
            const float x = content.getX() + colX[cell.column];
            const float y = content.getY() + rowY[cell.row];
            const float w = std::max(0.0f, colX[colEnd] - colX[cell.column] - gapX);
            const float h = std::max(0.0f, rowY[rowEnd] - rowY[cell.row] - gapY);
            child->arrange(Rect(x, y, w, h));
        }
    }

    // ---------------------------------------------------------------------------
    // CanvasPanel
    // ---------------------------------------------------------------------------

    CanvasPanel::CanvasPanel() : CanvasPanel(std::string()) {
    }

    CanvasPanel::CanvasPanel(std::string canvasId) : Widget(std::move(canvasId)) {
    }

    bool CanvasPanel::isContainer() const {
        return true;
    }

    //The canvas takes what it is given; children never push its size.
    Vector2 CanvasPanel::measure(Vector2 available) {
        const Vector2 base = Widget::measure(available);
        float width = available.x >= 0.0f ? available.x : base.x;
        float height = available.y >= 0.0f ? available.y : base.y;
        desiredSize = Vector2(width, height);
        return desiredSize;
    }

    //Anchor resolution happens after the flow pass, against this rect; every
    //child is then placed by its own arrangement path.
    void CanvasPanel::arrange(const Rect& rect) {
        setLayoutRect(rect);
        clearDirtyFlags(WidgetDirty::Layout);

        for (size_t i = 0; i < children.size(); i++) {
            Widget* child = children[i].get();
            if (!child->isVisible()) {
                continue;
            }
            Rect childRect = resolveAnchorRect(child->getAnchorData(), rect);
            child->arrange(childRect);
        }
    }

    // ---------------------------------------------------------------------------
    // SizeBox
    // ---------------------------------------------------------------------------

    SizeBox::SizeBox() : SizeBox(std::string()) {
    }

    SizeBox::SizeBox(std::string boxId) : Widget(std::move(boxId)),
        forcedWidth(0.0f),
        forcedHeight(0.0f),
        minForcedWidth(0.0f),
        minForcedHeight(0.0f) {
    }

    namespace {
        //The forced dimensions travel through the public slot so FlexEngine's
        //generic slot application enforces them on the Yoga node without a
        //second reader for the private fields below.
        void writeForcedToSlot(Slot& target, float forcedW, float forcedH,
            float minW, float minH) {
            target.minSize.x = std::max(forcedW, minW);
            target.minSize.y = std::max(forcedH, minH);
            target.maxSize.x = forcedW > 0.0f ? std::max(forcedW, minW) : 0.0f;
            target.maxSize.y = forcedH > 0.0f ? std::max(forcedH, minH) : 0.0f;
        }
    }

    SizeBox& SizeBox::setWidth(float width) {
        forcedWidth = std::max(0.0f, width);
        writeForcedToSlot(slot, forcedWidth, forcedHeight, minForcedWidth, minForcedHeight);
        markSubtreeDirty(WidgetDirty::Layout);
        return *this;
    }

    SizeBox& SizeBox::setHeight(float height) {
        forcedHeight = std::max(0.0f, height);
        writeForcedToSlot(slot, forcedWidth, forcedHeight, minForcedWidth, minForcedHeight);
        markSubtreeDirty(WidgetDirty::Layout);
        return *this;
    }

    SizeBox& SizeBox::setMinWidth(float width) {
        minForcedWidth = std::max(0.0f, width);
        writeForcedToSlot(slot, forcedWidth, forcedHeight, minForcedWidth, minForcedHeight);
        markSubtreeDirty(WidgetDirty::Layout);
        return *this;
    }

    SizeBox& SizeBox::setMinHeight(float height) {
        minForcedHeight = std::max(0.0f, height);
        writeForcedToSlot(slot, forcedWidth, forcedHeight, minForcedWidth, minForcedHeight);
        markSubtreeDirty(WidgetDirty::Layout);
        return *this;
    }

    bool SizeBox::isContainer() const {
        return true;
    }

    Vector2 SizeBox::measure(Vector2 available) {
        const WidgetStyle& style = getStyle();
        const Vector2 inner = childAvailable(available, style.padding);

        //an unset override means "size to the content", exactly like SSizeBox;
        //the slot min/max pair written above already carries the forced size,
        //so the generic clamp below reproduces what the Yoga node will resolve
        const Vector2 content = largestChildSize(children, inner);
        float width = forcedWidth > 0.0f ? forcedWidth : content.x;
        float height = forcedHeight > 0.0f ? forcedHeight : content.y;
        width = std::max(width, minForcedWidth);
        height = std::max(height, minForcedHeight);

        width += style.padding.left + style.padding.right;
        height += style.padding.top + style.padding.bottom;
        clampSize(width, height, style.minSize, style.maxSize);
        clampSize(width, height, slot.minSize, slot.maxSize);
        if (available.x >= 0.0f) {
            width = std::min(width, available.x);
        }
        if (available.y >= 0.0f) {
            height = std::min(height, available.y);
        }

        desiredSize = Vector2(width, height);
        return desiredSize;
    }

    void SizeBox::arrange(const Rect& rect) {
        float width = rect.getWidth();
        float height = rect.getHeight();
        const float minW = std::max(forcedWidth, minForcedWidth);
        const float minH = std::max(forcedHeight, minForcedHeight);
        if (forcedWidth > 0.0f) {
            width = minW;
        } else {
            width = std::max(width, minForcedWidth);
        }
        if (forcedHeight > 0.0f) {
            height = minH;
        } else {
            height = std::max(height, minForcedHeight);
        }

        const Rect forced(rect.getX(), rect.getY(), width, height);
        setLayoutRect(forced);
        clearDirtyFlags(WidgetDirty::Layout);

        const Rect content = contentRect(forced);
        for (size_t i = 0; i < children.size(); i++) {
            if (children[i]->isVisible()) {
                children[i]->arrange(content);
            }
        }
    }

    // ---------------------------------------------------------------------------
    // ScaleBox
    // ---------------------------------------------------------------------------

    ScaleBox::ScaleBox() : ScaleBox(std::string()) {
    }

    ScaleBox::ScaleBox(std::string boxId) : Widget(std::move(boxId)),
        designSize(0.0f, 0.0f),
        appliedScale(1.0f, 1.0f) {
    }

    ScaleBox& ScaleBox::setDesignSize(Vector2 size) {
        designSize = Vector2(std::max(0.0f, size.x), std::max(0.0f, size.y));
        markSubtreeDirty(WidgetDirty::Layout);
        return *this;
    }

    Vector2 ScaleBox::getDesignSize() const {
        return designSize;
    }

    Vector2 ScaleBox::getScale() const {
        return appliedScale;
    }

    bool ScaleBox::isContainer() const {
        return true;
    }

    Vector2 ScaleBox::measure(Vector2 available) {
        float width = available.x >= 0.0f ? available.x : designSize.x;
        float height = available.y >= 0.0f ? available.y : designSize.y;

        appliedScale = Vector2(1.0f, 1.0f);
        if (designSize.x > 0.0f && width > 0.0f) {
            appliedScale.x = width / designSize.x;
        }
        if (designSize.y > 0.0f && height > 0.0f) {
            appliedScale.y = height / designSize.y;
        }

        width = std::max(width, getStyle().padding.left + getStyle().padding.right);
        height = std::max(height, getStyle().padding.top + getStyle().padding.bottom);
        clampSize(width, height, slot.minSize, slot.maxSize);

        desiredSize = Vector2(width, height);
        return desiredSize;
    }

    //The child keeps its design-size coordinates; the paint walk multiplies
    //them by getScale() through PaintContext (see Widget::applyPaintTransform).
    void ScaleBox::arrange(const Rect& rect) {
        setLayoutRect(rect);
        clearDirtyFlags(WidgetDirty::Layout);

        if (designSize.x > 0.0f) {
            appliedScale.x = rect.getWidth() / designSize.x;
        }
        if (designSize.y > 0.0f) {
            appliedScale.y = rect.getHeight() / designSize.y;
        }

        const Rect design(rect.getX(), rect.getY(), designSize.x, designSize.y);
        for (size_t i = 0; i < children.size(); i++) {
            if (children[i]->isVisible()) {
                children[i]->arrange(design);
            }
        }
    }

    // ---------------------------------------------------------------------------
    // Border
    // ---------------------------------------------------------------------------

    Border::Border() : Border(std::string()) {
    }

    Border::Border(std::string borderId) : Widget(std::move(borderId)) {
    }

    bool Border::isContainer() const {
        return true;
    }

    //Manual fill of the content box. Under FlexEngine the Border keeps its
    //child in the parent flow (the frozen header gives Border no measure
    //override, so the node must size to that child instead of through an
    //island), which stretches the cross axis and leaves the main axis at the
    //child's own size: a stretched Border fills one axis, not both.
    void Border::arrange(const Rect& rect) {
        setLayoutRect(rect);
        clearDirtyFlags(WidgetDirty::Layout);

        const Rect content = contentRect(rect);
        for (size_t i = 0; i < children.size(); i++) {
            if (children[i]->isVisible()) {
                children[i]->arrange(content);
            }
        }
    }

    // ---------------------------------------------------------------------------
    // ScrollBox
    // ---------------------------------------------------------------------------

    //Containers.h promises "the internal Box children are added to" but does not
    //declare the addChild override that would make it happen (Widget::addChild
    //is virtual for exactly this, and an override cannot be introduced from the
    //cpp). The header is frozen, so the content is the widget's own direct
    //children and `content` stays null; contentBox() reports the first child,
    //which is the content holder for the common "one Box inside the scroll"
    //authoring shape. The gap has no field in the header either, so it lives in
    //the widget's own anchor pivot, which resolveAnchorRect documents as unused
    //by placement; FlexEngine reads it back there to build the anonymous content
    //group (see solveScrollGroup in FlexEngine.cpp). Reporting both limits is
    //the price of the frozen header, not a design choice.
    ScrollBox::ScrollBox() : ScrollBox(std::string()) {
    }

    ScrollBox::ScrollBox(std::string scrollId) : Widget(std::move(scrollId)),
        content(nullptr),
        orientation(Orientation::Vertical),
        offset(0.0f, 0.0f) {
    }

    bool ScrollBox::isContainer() const {
        return true;
    }

    Widget* ScrollBox::contentBox() {
        return children.empty() ? nullptr : children.front().get();
    }

    ScrollBox& ScrollBox::setOrientation(Orientation newOrientation) {
        if (orientation != newOrientation) {
            orientation = newOrientation;
            markSubtreeDirty(WidgetDirty::Layout);
        }
        return *this;
    }

    Orientation ScrollBox::getOrientation() const {
        return orientation;
    }

    ScrollBox& ScrollBox::setGap(float newGap) {
        const float gap = std::max(0.0f, newGap);
        if (anchorData.pivot.x != gap) {
            anchorData.pivot.x = gap;
            markSubtreeDirty(WidgetDirty::Layout);
        }
        return *this;
    }

    void ScrollBox::setContentOffset(Vector2 newOffset) {
        offset = Vector2(std::max(0.0f, newOffset.x), std::max(0.0f, newOffset.y));
        //the offset only moves the content, but every child rect changes, so the
        //whole island has to be rewritten
        markSubtreeDirty(WidgetDirty::Layout);
    }

    Vector2 ScrollBox::getContentOffset() const {
        return offset;
    }

    //The content extent, from the sizes the last pass gave the children.
    Vector2 ScrollBox::getContentSize() const {
        const float gap = std::max(0.0f, anchorData.pivot.x);
        const bool horizontal = orientation == Orientation::Horizontal;

        float main = 0.0f;
        float cross = 0.0f;
        unsigned int counted = 0u;
        for (size_t i = 0; i < children.size(); i++) {
            const Widget* child = children[i].get();
            if (!child->isVisible()) {
                continue;
            }
            const Vector2 size = child->getDesiredSize();
            main += horizontal ? size.x : size.y;
            cross = std::max(cross, horizontal ? size.y : size.x);
            counted++;
        }
        if (counted > 1u) {
            main += gap * static_cast<float>(counted - 1u);
        }
        return horizontal ? Vector2(main, cross) : Vector2(cross, main);
    }

    void ScrollBox::scrollToEnd() {
        const Vector2 size = getContentSize();
        setContentOffset(Vector2(
            std::max(0.0f, size.x - layoutRect.getWidth()),
            std::max(0.0f, size.y - layoutRect.getHeight())));
    }

    namespace {
        //Content size of a scroll viewport measured the way the engine's group
        //node solves it: natural size along the scroll axis, bounded cross axis.
        Vector2 scrollContentMeasure(ScrollBox& scroll, Vector2 inner) {
            const float gap = std::max(0.0f, scroll.getAnchorData().pivot.x);
            const bool horizontal = scroll.getOrientation() == Orientation::Horizontal;
            const std::vector<std::unique_ptr<Widget>>& children = scroll.getChildren();

            float main = 0.0f;
            float cross = 0.0f;
            unsigned int counted = 0u;
            for (size_t i = 0; i < children.size(); i++) {
                Widget* child = children[i].get();
                if (!child->isVisible()) {
                    continue;
                }
                const Vector2 childAvailable = horizontal
                    ? Vector2(-1.0f, inner.y)
                    : Vector2(inner.x, -1.0f);
                const Vector2 size = child->measure(childAvailable);
                main += horizontal ? size.x : size.y;
                cross = std::max(cross, horizontal ? size.y : size.x);
                counted++;
            }
            if (counted > 1u) {
                main += gap * static_cast<float>(counted - 1u);
            }
            return horizontal ? Vector2(main, cross) : Vector2(cross, main);
        }
    }

    //A scroll viewport is a window on the content, not the content: on its own
    //scroll axis it claims no space (grow, a basis or an explicit slot height
    //give it one), while an Exactly measure is still honoured because Yoga
    //overrides the measured value with the bound it demanded. Claiming the
    //content here would let a long list squeeze its siblings out of the box.
    Vector2 ScrollBox::measure(Vector2 available) {
        const WidgetStyle& style = getStyle();
        const Vector2 inner = childAvailable(available, style.padding);
        const bool horizontal = orientation == Orientation::Horizontal;

        const Vector2 content = scrollContentMeasure(*this, inner);

        float width;
        float height;
        if (horizontal) {
            width = 0.0f;
            height = available.y >= 0.0f ? std::min(content.y, available.y) : content.y;
        } else {
            width = available.x >= 0.0f ? std::min(content.x, available.x) : content.x;
            height = 0.0f;
        }

        width += style.padding.left + style.padding.right;
        height += style.padding.top + style.padding.bottom;
        clampSize(width, height, style.minSize, style.maxSize);
        clampSize(width, height, slot.minSize, slot.maxSize);

        desiredSize = Vector2(width, height);
        return desiredSize;
    }

    //Manual fallback path; under FlexEngine the content is solved as one
    //anonymous island group (solveScrollGroup), which stacks the same way.
    void ScrollBox::arrange(const Rect& rect) {
        setLayoutRect(rect);
        clearDirtyFlags(WidgetDirty::Layout);

        const Rect inner = contentRect(rect);
        const float gap = std::max(0.0f, anchorData.pivot.x);
        const bool horizontal = orientation == Orientation::Horizontal;
        const Vector2 avail(horizontal ? -1.0f : inner.getWidth(),
            horizontal ? inner.getHeight() : -1.0f);

        float cursor = 0.0f;
        unsigned int counted = 0u;
        for (size_t i = 0; i < children.size(); i++) {
            Widget* child = children[i].get();
            if (!child->isVisible()) {
                continue;
            }
            if (counted > 0u) {
                cursor += gap;
            }
            const Vector2 size = child->measure(avail);
            //the cross axis stretches to the viewport, the same default Yoga
            //gets from the group node's align-items
            if (horizontal) {
                child->arrange(Rect(inner.getX() + cursor - offset.x,
                    inner.getY() - offset.y, std::max(0.0f, size.x), inner.getHeight()));
                cursor += size.x;
            } else {
                child->arrange(Rect(inner.getX() - offset.x,
                    inner.getY() + cursor - offset.y, inner.getWidth(),
                    std::max(0.0f, size.y)));
                cursor += size.y;
            }
            counted++;
        }
    }

    //ScrollBox paints its whole subtree so the clip in PaintContext covers the
    //children (overflow hidden). NOTE for the frame walker (Slate.cpp): a
    //ScrollBox's children must NOT be walked again after calling paint() on
    //it, or everything inside the scroll region is emitted twice.
    void ScrollBox::paint(PaintContext& context) {
        Widget::paint(context);
        if (context.list == nullptr || !isVisible()) {
            return;
        }

        PaintContext clipped = context;
        const Rect viewport = applyPaintTransform(layoutRect, context);
        clipped.clip = context.hasClip ? intersectRect(context.clip, viewport) : viewport;
        clipped.hasClip = true;
        clipped.opacity = context.opacity * opacity;

        for (size_t i = 0; i < children.size(); i++) {
            paintSubtree(*children[i], clipped);
        }
    }
}
