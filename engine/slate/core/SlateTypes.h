// (c) SR VINIX
// SPDX-License-Identifier: MIT

#ifndef SlateTypes_h
#define SlateTypes_h

#include <string>

#include "math/Rect.h"
#include "math/Vector2.h"
#include "math/Vector4.h"
#include "texture/Texture.h"

namespace quantum::slate {

    class Widget;

    struct SlateEvent {
        Widget* currentTarget = nullptr;
        bool handled = false;
        bool propagationStopped = false;

        void stopPropagation() {
            propagationStopped = true;
        }
    };

    struct SlatePointerEvent : SlateEvent {
        Vector2 position;
        int button = -1;
    };

    struct SlateKeyEvent : SlateEvent {
        int key = -1;
        bool repeat = false;
    };

    // Flow axis of a Box/WrapBox/ScrollBox.
    enum class Orientation {
        Horizontal,
        Vertical
    };

    // Slate slot alignment. Fill stretches the child along that axis.
    enum class HAlign {
        Left,
        Center,
        Right,
        Fill
    };

    enum class VAlign {
        Top,
        Center,
        Bottom,
        Fill
    };

    // How a brush maps its texture onto the target rect (FSlateBrush::DrawMode).
    enum class BrushDrawMode {
        Simple, //one quad, scaled to the rect
        Slice,  //nine-slice using the brush margins
        Tile    //repeat at the brush's native size
    };

    // Visual state a widget resolves its style through.
    enum class WidgetVisualState {
        Normal,
        Hovered,
        Pressed,
        Focused,
        Disabled,
        Count
    };

    // Free-space distribution along a box's main axis (justify-content).
    enum class Justify {
        Start,
        Center,
        End,
        SpaceBetween,
        SpaceAround,
        SpaceEvenly
    };

    // Cross-axis placement (align-items / align-self). Stretch fills the axis.
    enum class Align {
        Start,
        Center,
        End,
        Stretch
    };

    enum class TextAlign {
        Near,   //left / top
        Center,
        Far     //right / bottom
    };

    enum class TextOverflow {
        Clip,
        Ellipsis
    };

    // Edge insets (padding, margins, brush slices) in canvas pixels.
    struct Insets {
        float left;
        float top;
        float right;
        float bottom;

        Insets() : left(0.0f), top(0.0f), right(0.0f), bottom(0.0f) {}
        Insets(float all) : left(all), top(all), right(all), bottom(all) {}
        Insets(float l, float t, float r, float b) : left(l), top(t), right(r), bottom(b) {}

        bool isZero() const {
            return left == 0.0f && top == 0.0f && right == 0.0f && bottom == 0.0f;
        }
    };

    // A named piece of 2D artwork: texture + slicing margins + design size.
    // Brushes are the only way a widget references an image, so a theme can
    // swap art without touching widget code (Slate's FSlateBrush).
    struct SlateBrush {
        std::string name;
        Texture texture;
        Vector2 size;         //design size in canvas pixels
        Insets slices;        //nine-slice margins in texture pixels
        BrushDrawMode drawMode = BrushDrawMode::Simple;

        bool isEmpty() const {
            return texture.empty() && size.x <= 0.0f && size.y <= 0.0f;
        }
    };

    // UMG CanvasPanel slot: anchors in 0..1 relative to the canvas (or to the
    // parent panel rect), a pivot and pixel offsets from the anchored edges.
    // Mirrors the semantics of the legacy UILayout anchors (UILayout.h:10-50).
    struct AnchorData {
        Vector4 anchors;   //minX, minY, maxX, maxY
        Vector2 pivot;     //0..1, where the offsets are measured from
        Vector4 offsets;   //left, top, right, bottom in pixels

        AnchorData() : anchors(0.0f, 0.0f, 0.0f, 0.0f), pivot(0.0f, 0.0f),
                       offsets(0.0f, 0.0f, 0.0f, 0.0f) {}
    };

    // What a widget needs recomputed. Separate bits so a colour change does not
    // re-run layout and a size change does not re-run style resolution.
    enum class WidgetDirty : unsigned int {
        None = 0,
        Style = 1u << 0,
        Layout = 1u << 1,
        Paint = 1u << 2,
        All = 7u
    };

    inline WidgetDirty operator|(WidgetDirty a, WidgetDirty b) {
        return static_cast<WidgetDirty>(static_cast<unsigned int>(a) | static_cast<unsigned int>(b));
    }

    inline WidgetDirty operator&(WidgetDirty a, WidgetDirty b) {
        return static_cast<WidgetDirty>(static_cast<unsigned int>(a) & static_cast<unsigned int>(b));
    }

    inline WidgetDirty& operator|=(WidgetDirty& a, WidgetDirty b) {
        a = a | b;
        return a;
    }

    // Resolves an anchored slot inside `reference` (top-left origin pixels).
    // Shared by CanvasPanel and by any widget placed with explicit anchors.
    Rect resolveAnchorRect(const AnchorData& anchor, const Rect& reference);
}

#endif /* SlateTypes_h */
