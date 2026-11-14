// (c) SR VINIX
// SPDX-License-Identifier: MIT

#ifndef SlateWidget_h
#define SlateWidget_h

#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "Export.h"
#include "math/Rect.h"
#include "math/Vector2.h"
#include "math/Vector4.h"
#include "slate/core/PaintList.h"
#include "slate/core/SlateTypes.h"
#include "slate/core/StyleSet.h"

//Yoga node handle (engine/libs/yoga): created, linked and released by the layout
//pass, kept opaque here so this header stays free of the layout dependency
struct YGNode;

namespace quantum::slate {

    // Accumulated paint state while the tree is walked top-down: nested clips
    // intersect, nested opacities multiply, and a ScaleBox ancestor contributes
    // a uniform scale around `origin` that every emitted rect must go through
    // (see Widget::applyPaintTransform).
    struct PaintContext {
        PaintList* list = nullptr;
        Rect clip;
        bool hasClip = false;
        float opacity = 1.0f;
        Vector2 scale = Vector2(1.0f, 1.0f);
        Vector2 origin = Vector2(0.0f, 0.0f);
    };

    // How the parent arranges this widget. Slate keeps arrangement in the slot
    // (not in the style), so a themed button still sizes where its menu puts it.
    struct Slot {
        HAlign hAlign = HAlign::Fill;
        VAlign vAlign = VAlign::Fill;
        Insets margin;
        //Flex participation. grow/shrink are weights, basis is the starting
        //size along the flow axis (-1 = auto, i.e. measure the content).
        float grow = 0.0f;
        float shrink = 1.0f;
        float basis = -1.0f;
        Vector2 minSize = Vector2(0.0f, 0.0f);
        Vector2 maxSize = Vector2(0.0f, 0.0f);   //0 = unlimited
        bool absolute = false;   //take this widget out of the flow (CSS position:absolute)
    };

    // Base node of the retained widget tree. Coordinates are canvas pixels with
    // a top-left origin; the same convention the legacy UI and STBText use.
    class Widget {
    public:
        Widget();
        explicit Widget(std::string id);
        virtual ~Widget();

        Widget(const Widget&) = delete;
        Widget& operator=(const Widget&) = delete;

        //identity
        const std::string& getId() const;
        void setId(const std::string& id);
        void addStyleClass(const std::string& className);
        bool hasStyleClass(const std::string& className) const;
        const std::vector<std::string>& getStyleClasses() const;

        //Named style in the active StyleSet ("Hud.Button"). Empty = defaults.
        const std::string& getStyleName() const;
        void setStyleName(const std::string& name);

        //state
        bool isVisible() const;
        void setVisible(bool visible);
        bool isEnabled() const;
        void setEnabled(bool enabled);
        bool isFocusable() const;
        Widget& setFocusable(bool focusable);
        bool isFocused() const;
        void setFocused(bool focused);
        bool isHovered() const;
        void setHovered(bool hovered);
        void setOpacity(float opacity);
        float getOpacity() const;
        const std::string& getTooltip() const;
        void setTooltip(const std::string& tooltip);

        //Which look the style resolves for. Containers that restyle their own
        //children (tab strips, list rows) override this.
        virtual WidgetVisualState getVisualState() const;
        virtual const char* getTypeName() const;

        //hierarchy
        Widget* getParent() const;
        const std::vector<std::unique_ptr<Widget>>& getChildren() const;
        //virtual so a widget that owns an inner content box (ScrollBox) can
        //redirect its children without hiding the base overload
        virtual Widget* addChild(std::unique_ptr<Widget> child);
        template <typename T, typename... Args>
        T* addChild(Args&&... args) {
            static_assert(std::is_base_of<Widget, T>::value, "T must derive from Widget");
            std::unique_ptr<T> child = std::make_unique<T>(std::forward<Args>(args)...);
            T* raw = child.get();
            addChild(std::unique_ptr<Widget>(std::move(child)));
            return raw;
        }
        bool removeChild(Widget* child);
        void clearChildren();
        unsigned int getChildCount() const;
        unsigned int getSubtreeCount() const;   //includes self
        Widget* findById(const std::string& id);

        //slot: setters return *this so a call site reads as one declaration
        Slot& getSlot();
        const Slot& getSlot() const;
        Widget& setHAlign(HAlign align);
        Widget& setVAlign(VAlign align);
        Widget& setMargin(const Insets& margin);
        Widget& setMargin(float all);
        Widget& setGrow(float grow);
        Widget& setBasis(float basis);
        Widget& setMinSize(Vector2 size);
        Widget& setMaxSize(Vector2 size);
        Widget& setAbsolute(bool absolute);

        //anchored placement, read by CanvasPanel
        AnchorData& getAnchorData();
        const AnchorData& getAnchorData() const;
        Widget& setAnchors(float minX, float minY, float maxX, float maxY);
        Widget& setPivot(float x, float y);
        Widget& setOffsets(float left, float top, float right, float bottom);

        //z-order: children are painted in (zOrder, insertion) order, stable.
        Widget& setZOrder(int zOrder);
        int getZOrder() const;

        //geometry: layoutRect is written by the layout pass and is absolute on
        //the canvas; desiredSize is what measure() produced for this widget.
        const Rect& getLayoutRect() const;
        void setLayoutRect(const Rect& rect);
        Vector2 getDesiredSize() const;

        //Yoga node, owned by the widget and driven by FlexEngine
        void setLayoutNode(::YGNode* node);
        ::YGNode* getLayoutNode() const;

        //style resolution (called by the style pass; widgets never cache a
        //WidgetStyle across frames because the theme may be swapped)
        void applyStyle();
        const WidgetStyle& getStyle() const;

        //dirty tracking
        WidgetDirty getDirtyFlags() const;        void markDirty(WidgetDirty flags = WidgetDirty::All);
        void clearDirtyFlags(WidgetDirty flags = WidgetDirty::All);
        //Marks this widget and every ancestor layout-dirty, because a child
        //resize changes the parent's arrangement.
        void markSubtreeDirty(WidgetDirty flags);

        //flow direction of containers, ignored by leaves
        virtual bool isContainer() const;
        //content size for a given space; includes padding and slot min/max
        virtual Vector2 measure(Vector2 available);
        //place children inside `rect`; only containers do anything
        virtual void arrange(const Rect& rect);

        //paint: emits commands for this widget into context.list. The default
        //implementation draws the style background/border; widgets that add
        //content (text, icons) call it first.
        virtual void paint(PaintContext& context);

        // Event hooks are split into capture, target and bubble phases by
        // EventRouter. Returning true marks the event handled; stopPropagation
        // prevents later phases from observing it.
        virtual bool onPointerDown(SlatePointerEvent& event);
        virtual bool onPointerUp(SlatePointerEvent& event);
        virtual bool onPointerMove(SlatePointerEvent& event);
        virtual bool onKeyDown(SlateKeyEvent& event);
        virtual bool onPointerDownCapture(SlatePointerEvent& event);
        virtual bool onPointerUpCapture(SlatePointerEvent& event);
        virtual bool onPointerMoveCapture(SlatePointerEvent& event);
        virtual bool onKeyDownCapture(SlateKeyEvent& event);
        virtual bool onPointerDownBubble(SlatePointerEvent& event);
        virtual bool onPointerUpBubble(SlatePointerEvent& event);
        virtual bool onPointerMoveBubble(SlatePointerEvent& event);
        virtual bool onKeyDownBubble(SlateKeyEvent& event);

    protected:
        //Rect minus the style padding, used by content-bearing widgets.
        Rect contentRect(const Rect& rect) const;
        //Canvas rect after the ScaleBox ancestors: origin + (rect - origin) * scale.
        static Rect applyPaintTransform(const Rect& rect, const PaintContext& context);
        void paintBackground(PaintContext& context, const Rect& rect);
        void paintBorder(PaintContext& context, const Rect& rect);
        static Vector4 applyOpacity(const Vector4& color, const PaintContext& context);
        static Rect intersectRect(const Rect& a, const Rect& b);

        std::string id;
        std::string styleName;
        std::string tooltip;
        std::vector<std::string> styleClasses;

        Widget* parent;
        std::vector<std::unique_ptr<Widget>> children;

        bool visible;
        bool enabled;
        bool focusable;
        bool focused;
        bool hovered;
        float opacity;
        int zOrder;

        Slot slot;
        AnchorData anchorData;
        //created and released by FlexEngine; nullptr until the first layout
        ::YGNode* layoutNode;

        Rect layoutRect;
        Vector2 desiredSize;
        WidgetStyle resolvedStyle;
        //Style name the resolvedStyle was built from, to detect restyles.
        std::string resolvedFrom;
        WidgetVisualState resolvedState;

        WidgetDirty dirty;
    };
}

#endif /* SlateWidget_h */
