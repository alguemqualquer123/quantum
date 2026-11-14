// (c) SR VINIX
// SPDX-License-Identifier: MIT

#ifndef SlateContainers_h
#define SlateContainers_h

#include <vector>

#include "Export.h"
#include "math/Vector2.h"
#include "slate/core/Widget.h"

namespace quantum::slate {

    // Slate-style containers. Arrangement is driven by each child's Slot and the
    // container's own flow properties, resolved by FlexEngine (Yoga-backed).

    // Single-axis flow (SBox / UMG HorizontalBox + VerticalBox + quantum HBox).
    class Box : public Widget {
    public:
        Box();
        explicit Box(Orientation orientation, std::string id = std::string());
        explicit Box(std::string id);

        bool isContainer() const override;
        const char* getTypeName() const override;

        Box& setOrientation(Orientation orientation);
        Orientation getOrientation() const;
        //Free space in the main axis (justify-content).
        Box& setJustify(Justify justify);
        Justify getJustify() const;
        //Cross-axis placement of children that do not fill it (align-items).
        Box& setAlignItems(Align align);
        Align getAlignItems() const;
        Box& setGap(float gap);
        float getGap() const;
        Box& setWrap(bool wrap);
        bool getWrap() const;

        void arrange(const Rect& rect) override;
        Vector2 measure(Vector2 available) override;

    private:
        Orientation orientation;
        Justify justify;
        Align alignItems;
        float gap;
        bool wrap;
    };

    // Children share the same rect, painted in order (UMG Overlay / SOverlay).
    // The measured size is the largest child's.
    class Overlay : public Widget {
    public:
        Overlay();
        explicit Overlay(std::string id);

        bool isContainer() const override;
        void arrange(const Rect& rect) override;
        Vector2 measure(Vector2 available) override;
    };

    // Explicit rows and columns (SGridPanel / UMG GridPanel). Children are
    // placed with addChildAt(row, column); a cell may span several of them.
    class GridPanel : public Widget {
    public:
        GridPanel();
        explicit GridPanel(std::string id);

        bool isContainer() const override;

        GridPanel& setColumns(unsigned int columns);
        GridPanel& setRows(unsigned int rows);
        unsigned int getColumns() const;
        unsigned int getRows() const;
        GridPanel& setGap(float horizontal, float vertical);

        //Row/column weights: 0 sizes to content, > 0 shares leftover space.
        GridPanel& setColumnWeight(unsigned int column, float weight);
        GridPanel& setRowWeight(unsigned int row, float weight);

        struct Cell {
            unsigned int row;
            unsigned int column;
            unsigned int rowSpan;
            unsigned int columnSpan;
        };

        //Adds the child to this cell; ordering inside a cell follows insertion.
        template <typename T, typename... Args>
        T* addAt(const Cell& cell, Args&&... args) {
            T* child = Widget::addChild<T>(std::forward<Args>(args)...);
            cells.push_back({ cell.row, cell.column, cell.rowSpan, cell.columnSpan });
            return child;
        }
        template <typename T, typename... Args>
        T* addAt(unsigned int row, unsigned int column, Args&&... args) {
            return addAt<T>(Cell{ row, column, 1u, 1u }, std::forward<Args>(args)...);
        }

        void arrange(const Rect& rect) override;
        Vector2 measure(Vector2 available) override;

    private:
        unsigned int columns;
        unsigned int rows;
        float gapX;
        float gapY;
        std::vector<Cell> cells;
        std::vector<float> columnWeights;
        std::vector<float> rowWeights;
    };

    // uGUI/UMG CanvasPanel: every child is positioned by its AnchorData, so a
    // HUD element can stay pinned to a corner while the canvas resizes.
    class CanvasPanel : public Widget {
    public:
        CanvasPanel();
        explicit CanvasPanel(std::string id);

        bool isContainer() const override;
        //The canvas rect is the reference; children never influence it.
        void arrange(const Rect& rect) override;
        Vector2 measure(Vector2 available) override;
    };

    // Pins the size of its single child (SSizeBox / UMG SizeBox).
    class SizeBox : public Widget {
    public:
        SizeBox();
        explicit SizeBox(std::string id);

        SizeBox& setWidth(float width);
        SizeBox& setHeight(float height);
        SizeBox& setMinWidth(float width);
        SizeBox& setMinHeight(float height);
        bool isContainer() const override;
        void arrange(const Rect& rect) override;
        Vector2 measure(Vector2 available) override;

    private:
        float forcedWidth;
        float forcedHeight;
        float minForcedWidth;
        float minForcedHeight;
    };

    // Renders a child at a fixed design size, scaled to fit (UMG ScaleBox): the
    // DPI/reference-resolution trick, so a 1920x1080 layout survives any window.
    class ScaleBox : public Widget {
    public:
        ScaleBox();
        explicit ScaleBox(std::string id);

        ScaleBox& setDesignSize(Vector2 size);
        Vector2 getDesignSize() const;
        //Paint scale in X/Y returned to the renderer; the layout pass keeps the
        //widget at the design size and the paint walk applies the scale.
        Vector2 getScale() const;
        bool isContainer() const override;
        void arrange(const Rect& rect) override;
        Vector2 measure(Vector2 available) override;

    private:
        Vector2 designSize;
        Vector2 appliedScale;
    };

    // A background + border with one child (SBorder / UMG Border).
    class Border : public Widget {
    public:
        Border();
        explicit Border(std::string id);

        bool isContainer() const override;
        void arrange(const Rect& rect) override;
    };

    // Clipped, scrollable Box (UCommonScrollBox / UI Toolkit ScrollView). The
    // content offset is applied here; the wheel, drag and scrollbars arrive with
    // the input phase, so the offset is set explicitly for now.
    class ScrollBox : public Widget {
    public:
        ScrollBox();
        explicit ScrollBox(std::string id);

        bool isContainer() const override;

        ScrollBox& setOrientation(Orientation orientation);
        Orientation getOrientation() const;
        ScrollBox& setGap(float gap);
        //Content is laid out at its natural size and clipped to this widget.
        void setContentOffset(Vector2 offset);
        Vector2 getContentOffset() const;
        Vector2 getContentSize() const;
        //Clamps the offset against the content size (used by wheel/drag later).
        void scrollToEnd();

        void arrange(const Rect& rect) override;
        Vector2 measure(Vector2 available) override;
        void paint(PaintContext& context) override;

    private:
        Widget* contentBox();   //the internal Box children are added to

        Box* content;
        Orientation orientation;
        Vector2 offset;
    };
}

#endif /* SlateContainers_h */
