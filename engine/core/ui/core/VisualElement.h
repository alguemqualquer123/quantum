// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#ifndef VisualElement_h
#define VisualElement_h

#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "Export.h"
#include "math/Rect.h"
#include "math/Vector2.h"
#include "math/Vector4.h"
#include "texture/Texture.h"
#include "ui/core/Style.h"

//Yoga node handle (engine/libs/yoga); created, linked and released by the
//layout pass, opaque here so the element header stays dependency free
struct YGNode;

namespace doriax::ui {

    class PaintList;

    // Clipping behaviour of children that overflow the element rect.
    enum class Overflow {
        Visible,
        Hidden
    };

    // What the element needs recomputed. The layout flag is consumed by the
    // flexbox phase; the paint pass today walks the tree every frame.
    enum class DirtyFlags : unsigned int {
        None = 0,
        Paint = 1u << 0,
        Layout = 1u << 1,
        Style = 1u << 2,
        All = 7u
    };

    inline DirtyFlags operator|(DirtyFlags a, DirtyFlags b) {
        return static_cast<DirtyFlags>(static_cast<unsigned int>(a) | static_cast<unsigned int>(b));
    }

    inline DirtyFlags operator&(DirtyFlags a, DirtyFlags b) {
        return static_cast<DirtyFlags>(static_cast<unsigned int>(a) & static_cast<unsigned int>(b));
    }

    // Accumulated paint state while the tree is walked top-down.
    struct PaintContext {
        PaintList* list = nullptr; //target command list
        Rect clip;                 //absolute rect the children are clipped to
        bool hasClip = false;      //false: no clipping
        float opacity = 1.0f;      //inherited * own
    };

    // Retained-mode tree node of the new UI module (doriax::ui). Coexists with
    // the legacy scene UI (doriax::Panel, doriax::Button, ...), which it does
    // not replace or touch.
    class DORIAX_API VisualElement {
    public:
        VisualElement();
        explicit VisualElement(std::string id);
        virtual ~VisualElement();

        VisualElement(const VisualElement&) = delete;
        VisualElement& operator=(const VisualElement&) = delete;

        //identity
        const std::string& getId() const;
        void setId(std::string id);
        void addToClassList(std::string className);
        bool hasClass(const std::string& className) const;
        const std::vector<std::string>& getClasses() const;

        //hierarchy
        VisualElement* getParent() const;
        const std::vector<std::unique_ptr<VisualElement>>& getChildren() const;
        VisualElement* addChild(std::unique_ptr<VisualElement> child);
        template <typename T, typename... Args>
        T* addChild(Args&&... args) {
            static_assert(std::is_base_of<VisualElement, T>::value, "T must derive from VisualElement");
            std::unique_ptr<T> child = std::make_unique<T>(std::forward<Args>(args)...);
            T* raw = child.get();
            addChild(std::unique_ptr<VisualElement>(std::move(child)));
            return raw;
        }
        std::unique_ptr<VisualElement> removeChild(VisualElement* child);
        void clearChildren();
        unsigned int getChildCount() const;
        unsigned int getSubtreeCount() const; //includes self

        //state
        bool isVisible() const;
        void setVisible(bool visible);
        bool isEnabled() const;
        void setEnabled(bool enabled);
        void setOpacity(float opacity);
        float getOpacity() const;
        void setOverflow(Overflow overflow);
        Overflow getOverflow() const;

        //geometry: setPosition/setSize are sugar over the flexbox style
        //(absolute left/top and fixed width/height); the resolved rect comes
        //from the layout pass
        const Vector2& getPosition() const;
        void setPosition(Vector2 position);
        const Vector2& getSize() const;
        void setSize(Vector2 size);
        //absolute rect on the canvas, filled by the layout pass
        const Rect& getLayoutRect() const;
        void setLayoutRect(const Rect& rect);

        //flexbox style (a future USS resolver writes into the same struct; the
        //setters only mark the element for the layout pass)
        LayoutStyle& getStyle();
        const LayoutStyle& getStyle() const;

        void setDisplay(Display display);
        Display getDisplay() const;
        void setPositionType(PositionType positionType);
        PositionType getPositionType() const;
        void setFlexDirection(FlexDirection flexDirection);
        FlexDirection getFlexDirection() const;
        void setFlexWrap(FlexWrap flexWrap);
        FlexWrap getFlexWrap() const;
        void setJustifyContent(JustifyContent justifyContent);
        JustifyContent getJustifyContent() const;
        void setAlignItems(AlignItems alignItems);
        AlignItems getAlignItems() const;
        void setAlignSelf(AlignSelf alignSelf);
        AlignSelf getAlignSelf() const;
        void setAlignContent(AlignContent alignContent);
        AlignContent getAlignContent() const;
        void setFlexGrow(float flexGrow);
        float getFlexGrow() const;
        void setFlexShrink(float flexShrink);
        float getFlexShrink() const;
        void setFlexBasis(StyleLength flexBasis);
        void setWidth(StyleLength width);
        const StyleLength& getWidth() const;
        void setHeight(StyleLength height);
        const StyleLength& getHeight() const;
        void setMinWidth(StyleLength minWidth);
        void setMinHeight(StyleLength minHeight);
        void setMaxWidth(StyleLength maxWidth);
        void setMaxHeight(StyleLength maxHeight);
        void setMargin(Edge edge, StyleLength margin);
        void setMargin(StyleLength margin);
        void setPadding(Edge edge, StyleLength padding);
        void setPadding(StyleLength padding);
        void setGap(float gap);
        float getGap() const;
        void setLeft(StyleLength left);
        void setTop(StyleLength top);
        void setRight(StyleLength right);
        void setBottom(StyleLength bottom);

        //layout pass hooks: the measure pair lets content-sized elements (text
        //labels and future widgets) feed their intrinsic size to the engine
        virtual bool usesMeasureFunc() const;
        virtual Vector2 measureIntrinsicSize(float availableWidth, bool widthAtMost,
                                             float availableHeight, bool heightAtMost);
        //container hook, called before the element style is applied; returns
        //true when the children styles were changed and must be re-synced
        virtual bool updateLayoutChildren();

        //Yoga node handle, owned and linked by the layout pass (see YogaLayout)
        void setLayoutNode(::YGNode* node);
        ::YGNode* getLayoutNode() const;

        //paint properties (the USS phase replaces this source of values)
        const Vector4& getBackgroundColor() const;
        void setBackgroundColor(const Vector4& color);
        const Vector4& getBorderColor() const;
        void setBorderColor(const Vector4& color);
        float getBorderWidth() const;
        void setBorderWidth(float width);
        float getBorderRadius() const;
        void setBorderRadius(float radius);
        const Texture& getTexture() const;
        void setTexture(const Texture& texture);
        const Vector4& getTextureColor() const;
        void setTextureColor(const Vector4& color);
        //nine-slice insets (left, top, right, bottom) in texture pixels;
        //all zero keeps a plain stretch
        const Vector4& getNineSlice() const;
        void setNineSlice(float left, float top, float right, float bottom);

        //dirty tracking
        DirtyFlags getDirtyFlags() const;
        void markDirty(DirtyFlags flags = DirtyFlags::All);
        void clearDirtyFlags(DirtyFlags flags = DirtyFlags::All);

        //painting: called by the paint pass with the absolute rect resolved.
        //Widgets override this and call the base implementation first.
        virtual void paint(PaintContext& context, const Rect& rect);

    protected:
        //color * context.opacity
        static Vector4 applyOpacity(const Vector4& color, const PaintContext& context);

    private:
        std::string id;
        std::vector<std::string> classes;

        VisualElement* parent;
        std::vector<std::unique_ptr<VisualElement>> children;

        bool visible;
        bool enabled;
        float opacity;
        Overflow overflow;

        Vector2 position;
        Vector2 size;
        Rect layoutRect;
        LayoutStyle style;

        //owned by the element: released (with its subtree) in the destructor
        ::YGNode* layoutNode;

        Vector4 backgroundColor;
        Vector4 borderColor;
        float borderWidth;
        float borderRadius;
        Texture texture;
        Vector4 textureColor;
        Vector4 nineSlice;

        DirtyFlags dirty;
    };
}

#endif /* VisualElement_h */
