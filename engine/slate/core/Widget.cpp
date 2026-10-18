// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#include "slate/core/Widget.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "Log.h"

namespace doriax::slate {

    namespace {
        //Clamps a measured size against a min/max pair, style first then slot.
        //A max of 0 means unlimited, the same convention Slot::maxSize uses.
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
    }

    Widget::Widget() : Widget(std::string()) {
    }

    Widget::Widget(std::string widgetId) : id(std::move(widgetId)),
        parent(nullptr),
        visible(true),
        enabled(true),
        opacity(1.0f),
        zOrder(0),
        layoutNode(nullptr),
        resolvedState(WidgetVisualState::Normal),
        dirty(WidgetDirty::All) {
    }

    //layoutNode is deliberately left alone: the Yoga nodes are created and
    //released by FlexEngine::dispose before the tree is destroyed, and sibling
    //widgets may still reference them while the tree is being torn down.
    Widget::~Widget() {
        layoutNode = nullptr;
    }

    const std::string& Widget::getId() const {
        return id;
    }

    void Widget::setId(const std::string& newId) {
        id = newId;
    }

    void Widget::addStyleClass(const std::string& className) {
        if (className.empty() || hasStyleClass(className)) {
            return;
        }
        styleClasses.push_back(className);
        //USS selectors can key on the class, so the style pass must re-run
        markDirty(WidgetDirty::Style | WidgetDirty::Paint);
    }

    bool Widget::hasStyleClass(const std::string& className) const {
        for (size_t i = 0; i < styleClasses.size(); i++) {
            if (styleClasses[i] == className) {
                return true;
            }
        }
        return false;
    }

    const std::vector<std::string>& Widget::getStyleClasses() const {
        return styleClasses;
    }

    const std::string& Widget::getStyleName() const {
        return styleName;
    }

    void Widget::setStyleName(const std::string& name) {
        if (styleName == name) {
            return;
        }
        styleName = name;
        //a different named style can carry different padding, so the whole
        //pipeline has to see this widget again
        markSubtreeDirty(WidgetDirty::Style | WidgetDirty::Layout | WidgetDirty::Paint);
    }

    bool Widget::isVisible() const {
        return visible;
    }

    void Widget::setVisible(bool newVisible) {
        if (visible == newVisible) {
            return;
        }
        visible = newVisible;
        //hiding a child changes how its parent arranges the remaining ones
        markSubtreeDirty(WidgetDirty::Layout | WidgetDirty::Paint);
    }

    bool Widget::isEnabled() const {
        return enabled;
    }

    void Widget::setEnabled(bool newEnabled) {
        if (enabled == newEnabled) {
            return;
        }
        enabled = newEnabled;
        markDirty(WidgetDirty::Style | WidgetDirty::Paint);
    }

    void Widget::setOpacity(float newOpacity) {
        newOpacity = std::max(0.0f, std::min(1.0f, newOpacity));
        if (opacity == newOpacity) {
            return;
        }
        opacity = newOpacity;
        markDirty(WidgetDirty::Paint);
    }

    float Widget::getOpacity() const {
        return opacity;
    }

    const std::string& Widget::getTooltip() const {
        return tooltip;
    }

    void Widget::setTooltip(const std::string& newTooltip) {
        tooltip = newTooltip;
    }

    WidgetVisualState Widget::getVisualState() const {
        return enabled ? WidgetVisualState::Normal : WidgetVisualState::Disabled;
    }

    Widget* Widget::getParent() const {
        return parent;
    }

    const std::vector<std::unique_ptr<Widget>>& Widget::getChildren() const {
        return children;
    }

    Widget* Widget::addChild(std::unique_ptr<Widget> child) {
        if (child == nullptr) {
            Log::error("slate: Widget::addChild received a null child");
            return nullptr;
        }

        Widget* raw = child.get();
        if (raw->parent != nullptr) {
            Log::error("slate: Widget::addChild re-parents an owned child; detaching it first");
            raw->parent->removeChild(raw);
        }
        raw->parent = this;
        children.push_back(std::move(child));

        //paint order is (zOrder, insertion) and is resolved by whoever walks the
        //tree, so the base container just keeps the insertion order here
        markSubtreeDirty(WidgetDirty::Style | WidgetDirty::Layout | WidgetDirty::Paint);
        return raw;
    }

    bool Widget::removeChild(Widget* child) {
        for (size_t i = 0; i < children.size(); i++) {
            if (children[i].get() == child) {
                child->parent = nullptr;
                children.erase(children.begin() + i);
                markSubtreeDirty(WidgetDirty::Layout | WidgetDirty::Paint);
                return true;
            }
        }
        return false;
    }

    void Widget::clearChildren() {
        if (children.empty()) {
            return;
        }
        for (size_t i = 0; i < children.size(); i++) {
            children[i]->parent = nullptr;
        }
        children.clear();
        markSubtreeDirty(WidgetDirty::Layout | WidgetDirty::Paint);
    }

    unsigned int Widget::getChildCount() const {
        return static_cast<unsigned int>(children.size());
    }

    unsigned int Widget::getSubtreeCount() const {
        unsigned int count = 1;
        for (size_t i = 0; i < children.size(); i++) {
            count += children[i]->getSubtreeCount();
        }
        return count;
    }

    Widget* Widget::findById(const std::string& targetId) {
        if (!id.empty() && id == targetId) {
            return this;
        }
        for (size_t i = 0; i < children.size(); i++) {
            Widget* found = children[i]->findById(targetId);
            if (found != nullptr) {
                return found;
            }
        }
        return nullptr;
    }

    Slot& Widget::getSlot() {
        return slot;
    }

    const Slot& Widget::getSlot() const {
        return slot;
    }

    Widget& Widget::setHAlign(HAlign align) {
        if (slot.hAlign != align) {
            slot.hAlign = align;
            markSubtreeDirty(WidgetDirty::Layout);
        }
        return *this;
    }

    Widget& Widget::setVAlign(VAlign align) {
        if (slot.vAlign != align) {
            slot.vAlign = align;
            markSubtreeDirty(WidgetDirty::Layout);
        }
        return *this;
    }

    Widget& Widget::setMargin(const Insets& newMargin) {
        slot.margin = newMargin;
        markSubtreeDirty(WidgetDirty::Layout);
        return *this;
    }

    Widget& Widget::setMargin(float all) {
        return setMargin(Insets(all));
    }

    Widget& Widget::setGrow(float grow) {
        slot.grow = grow;
        markSubtreeDirty(WidgetDirty::Layout);
        return *this;
    }

    Widget& Widget::setBasis(float basis) {
        slot.basis = basis;
        markSubtreeDirty(WidgetDirty::Layout);
        return *this;
    }

    Widget& Widget::setMinSize(Vector2 size) {
        slot.minSize = size;
        markSubtreeDirty(WidgetDirty::Layout);
        return *this;
    }

    Widget& Widget::setMaxSize(Vector2 size) {
        slot.maxSize = size;
        markSubtreeDirty(WidgetDirty::Layout);
        return *this;
    }

    Widget& Widget::setAbsolute(bool absolute) {
        if (slot.absolute != absolute) {
            slot.absolute = absolute;
            markSubtreeDirty(WidgetDirty::Layout);
        }
        return *this;
    }

    AnchorData& Widget::getAnchorData() {
        return anchorData;
    }

    const AnchorData& Widget::getAnchorData() const {
        return anchorData;
    }

    Widget& Widget::setAnchors(float minX, float minY, float maxX, float maxY) {
        anchorData.anchors = Vector4(minX, minY, maxX, maxY);
        markSubtreeDirty(WidgetDirty::Layout);
        return *this;
    }

    Widget& Widget::setPivot(float x, float y) {
        anchorData.pivot = Vector2(x, y);
        markSubtreeDirty(WidgetDirty::Layout);
        return *this;
    }

    Widget& Widget::setOffsets(float left, float top, float right, float bottom) {
        anchorData.offsets = Vector4(left, top, right, bottom);
        markSubtreeDirty(WidgetDirty::Layout);
        return *this;
    }

    Widget& Widget::setZOrder(int newZOrder) {
        if (zOrder != newZOrder) {
            zOrder = newZOrder;
            //only the walk order changes, geometry and style are untouched
            markSubtreeDirty(WidgetDirty::Paint);
        }
        return *this;
    }

    int Widget::getZOrder() const {
        return zOrder;
    }

    const Rect& Widget::getLayoutRect() const {
        return layoutRect;
    }

    void Widget::setLayoutRect(const Rect& rect) {
        layoutRect = rect;
        desiredSize = Vector2(rect.getWidth(), rect.getHeight());
    }

    Vector2 Widget::getDesiredSize() const {
        return desiredSize;
    }

    void Widget::setLayoutNode(::YGNode* node) {
        layoutNode = node;
    }

    ::YGNode* Widget::getLayoutNode() const {
        return layoutNode;
    }

    void Widget::applyStyle() {
        WidgetVisualState state = getVisualState();
        StyleSet* set = StyleSet::get();

        WidgetStyle next;
        if (set != nullptr && !styleName.empty()) {
            next = set->resolve(styleName, state);
        }

        //A theme swap can restyle the same name, so the values are always
        //refreshed; layout only re-runs when the identity of the style changed
        bool restyled = resolvedFrom != styleName || resolvedState != state;
        resolvedStyle = next;
        resolvedFrom = styleName;
        resolvedState = state;

        markDirty(WidgetDirty::Paint);
        if (restyled) {
            markSubtreeDirty(WidgetDirty::Layout);
        }
    }

    const WidgetStyle& Widget::getStyle() const {
        return resolvedStyle;
    }

    WidgetDirty Widget::getDirtyFlags() const {
        return dirty;
    }

    void Widget::markDirty(WidgetDirty flags) {
        dirty = dirty | flags;
    }

    void Widget::clearDirtyFlags(WidgetDirty flags) {
        dirty = static_cast<WidgetDirty>(static_cast<unsigned int>(dirty) &
            ~static_cast<unsigned int>(flags));
    }

    void Widget::markSubtreeDirty(WidgetDirty flags) {
        markDirty(flags);
        //a child that changed size or moved invalidates every ancestor's
        //arrangement, so Layout climbs even when the caller only asked for Paint
        Widget* ancestor = parent;
        while (ancestor != nullptr) {
            ancestor->markDirty(WidgetDirty::Layout);
            ancestor = ancestor->parent;
        }
    }

    bool Widget::isContainer() const {
        return false;
    }

    Vector2 Widget::measure(Vector2 available) {
        //the base widget has no content of its own: leaves that paint a brush or
        //text override this and add their intrinsic size
        float width = resolvedStyle.padding.left + resolvedStyle.padding.right;
        float height = resolvedStyle.padding.top + resolvedStyle.padding.bottom;

        clampSize(width, height, resolvedStyle.minSize, resolvedStyle.maxSize);
        clampSize(width, height, slot.minSize, slot.maxSize);

        //the caller may still hand us less space than we ask for
        if (available.x >= 0.0f) {
            width = std::min(width, available.x);
        }
        if (available.y >= 0.0f) {
            height = std::min(height, available.y);
        }

        desiredSize = Vector2(width, height);
        return desiredSize;
    }

    void Widget::arrange(const Rect& rect) {
        layoutRect = rect;
        desiredSize = Vector2(rect.getWidth(), rect.getHeight());
        clearDirtyFlags(WidgetDirty::Layout);
    }

    void Widget::paint(PaintContext& context) {
        if (!visible || context.list == nullptr) {
            return;
        }

        Rect rect = applyPaintTransform(layoutRect, context);
        if (rect.getWidth() <= 0.0f || rect.getHeight() <= 0.0f) {
            return;
        }

        paintBackground(context, rect);
        paintBorder(context, rect);
    }

    Rect Widget::contentRect(const Rect& rect) const {
        float left = resolvedStyle.padding.left;
        float top = resolvedStyle.padding.top;
        float right = resolvedStyle.padding.right;
        float bottom = resolvedStyle.padding.bottom;

        float width = std::max(0.0f, rect.getWidth() - left - right);
        float height = std::max(0.0f, rect.getHeight() - top - bottom);
        return Rect(rect.getX() + left, rect.getY() + top, width, height);
    }

    Rect Widget::applyPaintTransform(const Rect& rect, const PaintContext& context) {
        //ScaleBox ancestors scale around context.origin; identity when scale is 1
        float x0 = context.origin.x + (rect.getX() - context.origin.x) * context.scale.x;
        float y0 = context.origin.y + (rect.getY() - context.origin.y) * context.scale.y;
        float x1 = context.origin.x + (rect.getX() + rect.getWidth() - context.origin.x) * context.scale.x;
        float y1 = context.origin.y + (rect.getY() + rect.getHeight() - context.origin.y) * context.scale.y;

        float left = std::min(x0, x1);
        float top = std::min(y0, y1);
        return Rect(left, top, std::fabs(x1 - x0), std::fabs(y1 - y0));
    }

    void Widget::paintBackground(PaintContext& context, const Rect& rect) {
        const Vector4& color = resolvedStyle.backgroundColor;

        if (!resolvedStyle.backgroundBrush.empty()) {
            StyleSet* set = StyleSet::get();
            const SlateBrush* brush = set != nullptr ? set->findBrush(resolvedStyle.backgroundBrush) : nullptr;
            if (brush != nullptr && !brush->isEmpty()) {
                //the brush carries the artwork, the style colour tints it; a fully
                //transparent colour would erase it, so fall back to white
                Vector4 tint = color.w > 0.0f ? color : Vector4(1.0f, 1.0f, 1.0f, 1.0f);
                context.list->addImage(rect, *brush, applyOpacity(tint, context),
                    context.clip, context.hasClip);
                return;
            }
        }

        if (color.w <= 0.0f) {
            return;
        }
        context.list->addRect(rect, applyOpacity(color, context), resolvedStyle.borderRadius,
            context.clip, context.hasClip);
    }

    void Widget::paintBorder(PaintContext& context, const Rect& rect) {
        if (!resolvedStyle.borderBrush.empty()) {
            StyleSet* set = StyleSet::get();
            const SlateBrush* brush = set != nullptr ? set->findBrush(resolvedStyle.borderBrush) : nullptr;
            if (brush != nullptr && !brush->isEmpty()) {
                const Vector4& color = resolvedStyle.borderColor;
                Vector4 tint = color.w > 0.0f ? color : Vector4(1.0f, 1.0f, 1.0f, 1.0f);
                context.list->addImage(rect, *brush, applyOpacity(tint, context),
                    context.clip, context.hasClip);
                return;
            }
        }

        if (resolvedStyle.borderWidth <= 0.0f || resolvedStyle.borderColor.w <= 0.0f) {
            return;
        }
        context.list->addBorder(rect, applyOpacity(resolvedStyle.borderColor, context),
            resolvedStyle.borderWidth, resolvedStyle.borderRadius,
            context.clip, context.hasClip);
    }

    Vector4 Widget::applyOpacity(const Vector4& color, const PaintContext& context) {
        //context.opacity already holds the inherited chain; the widget's own value
        //is folded in by the walker before it calls paint
        Vector4 out = color;
        out.w = color.w * context.opacity;
        return out;
    }

    Rect Widget::intersectRect(const Rect& a, const Rect& b) {
        float left = std::max(a.getX(), b.getX());
        float top = std::max(a.getY(), b.getY());
        float right = std::min(a.getX() + a.getWidth(), b.getX() + b.getWidth());
        float bottom = std::min(a.getY() + a.getHeight(), b.getY() + b.getHeight());

        if (right <= left || bottom <= top) {
            return Rect(0.0f, 0.0f, 0.0f, 0.0f);
        }
        return Rect(left, top, right - left, bottom - top);
    }
}
