// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "ui/core/VisualElement.h"

#include <utility>

#include <yoga/Yoga.h>

#include "ui/render/PaintList.h"

using namespace quantum;
using namespace quantum::ui;

namespace {

    //Detaches the Yoga nodes of a subtree before the owning node is freed
    //recursively, so the element destructors never touch freed nodes.
    void forgetLayoutNodes(VisualElement& element) {
        element.setLayoutNode(nullptr);
        for (const std::unique_ptr<VisualElement>& child : element.getChildren())
            forgetLayoutNodes(*child);
    }
}

VisualElement::VisualElement() :
    parent(nullptr),
    visible(true),
    enabled(true),
    opacity(1.0f),
    overflow(Overflow::Visible),
    position(0.0f, 0.0f),
    size(0.0f, 0.0f),
    layoutRect(0.0f, 0.0f, 0.0f, 0.0f),
    layoutNode(nullptr),
    backgroundColor(0.0f, 0.0f, 0.0f, 0.0f),
    borderColor(0.0f, 0.0f, 0.0f, 0.0f),
    borderWidth(0.0f),
    borderRadius(0.0f),
    textureColor(1.0f, 1.0f, 1.0f, 1.0f),
    nineSlice(0.0f, 0.0f, 0.0f, 0.0f),
    dirty(DirtyFlags::All) {
}

VisualElement::VisualElement(std::string id) : VisualElement() {
    this->id = std::move(id);
}

VisualElement::~VisualElement() {
    ::YGNode* node = layoutNode;
    if (node == nullptr)
        return;

    layoutNode = nullptr;

    //descendant nodes are freed together with this one
    for (const std::unique_ptr<VisualElement>& child : children)
        forgetLayoutNodes(*child);

    if (parent != nullptr && parent->layoutNode != nullptr)
        YGNodeRemoveChild(parent->layoutNode, node);

    YGNodeFreeRecursive(node);
}

const std::string& VisualElement::getId() const {
    return id;
}

void VisualElement::setId(std::string id) {
    this->id = std::move(id);
}

void VisualElement::addToClassList(std::string className) {
    if (className.empty())
        return;

    if (!hasClass(className))
        classes.push_back(std::move(className));
}

bool VisualElement::hasClass(const std::string& className) const {
    for (const std::string& entry : classes) {
        if (entry == className)
            return true;
    }
    return false;
}

const std::vector<std::string>& VisualElement::getClasses() const {
    return classes;
}

VisualElement* VisualElement::getParent() const {
    return parent;
}

const std::vector<std::unique_ptr<VisualElement>>& VisualElement::getChildren() const {
    return children;
}

VisualElement* VisualElement::addChild(std::unique_ptr<VisualElement> child) {
    if (!child)
        return nullptr;

    child->parent = this;

    VisualElement* raw = child.get();
    children.push_back(std::move(child));

    markDirty(DirtyFlags::Layout);
    return raw;
}

std::unique_ptr<VisualElement> VisualElement::removeChild(VisualElement* child) {
    for (auto it = children.begin(); it != children.end(); ++it) {
        if (it->get() == child) {
            std::unique_ptr<VisualElement> owned = std::move(*it);
            owned->parent = nullptr;

            //the element keeps its node (it may be reparented); the link to
            //the old parent is dropped here
            if (layoutNode != nullptr && owned->layoutNode != nullptr)
                YGNodeRemoveChild(layoutNode, owned->layoutNode);

            children.erase(it);

            markDirty(DirtyFlags::Layout);
            return owned;
        }
    }

    return nullptr;
}

void VisualElement::clearChildren() {
    if (children.empty())
        return;

    for (auto& child : children) {
        if (layoutNode != nullptr && child->layoutNode != nullptr)
            YGNodeRemoveChild(layoutNode, child->layoutNode);
        child->parent = nullptr;
    }
    children.clear();

    markDirty(DirtyFlags::Layout);
}

unsigned int VisualElement::getChildCount() const {
    return (unsigned int)children.size();
}

unsigned int VisualElement::getSubtreeCount() const {
    unsigned int count = 1;
    for (const auto& child : children) {
        count += child->getSubtreeCount();
    }
    return count;
}

bool VisualElement::isVisible() const {
    return visible;
}

void VisualElement::setVisible(bool visible) {
    if (this->visible == visible)
        return;

    this->visible = visible;
    markDirty(DirtyFlags::Paint | DirtyFlags::Layout);
}

bool VisualElement::isEnabled() const {
    return enabled;
}

void VisualElement::setEnabled(bool enabled) {
    if (this->enabled == enabled)
        return;

    this->enabled = enabled;
    markDirty(DirtyFlags::Paint | DirtyFlags::Style);
}

void VisualElement::setOpacity(float opacity) {
    if (this->opacity == opacity)
        return;

    this->opacity = opacity;
    markDirty(DirtyFlags::Paint | DirtyFlags::Style);
}

float VisualElement::getOpacity() const {
    return opacity;
}

void VisualElement::setOverflow(Overflow overflow) {
    if (this->overflow == overflow)
        return;

    this->overflow = overflow;
    markDirty(DirtyFlags::Paint | DirtyFlags::Layout);
}

Overflow VisualElement::getOverflow() const {
    return overflow;
}

const Vector2& VisualElement::getPosition() const {
    return position;
}

void VisualElement::setPosition(Vector2 position) {
    if (this->position == position)
        return;

    this->position = position;
    //absolute placement: the flexbox equivalent of the older manual position
    style.positionType = PositionType::Absolute;
    style.left = StyleLength::point(position.x);
    style.top = StyleLength::point(position.y);
    markDirty(DirtyFlags::Paint | DirtyFlags::Layout);
}

const Vector2& VisualElement::getSize() const {
    return size;
}

void VisualElement::setSize(Vector2 size) {
    if (this->size == size)
        return;

    this->size = size;
    style.width = StyleLength::point(size.x);
    style.height = StyleLength::point(size.y);
    markDirty(DirtyFlags::Paint | DirtyFlags::Layout);
}

const Rect& VisualElement::getLayoutRect() const {
    return layoutRect;
}

void VisualElement::setLayoutRect(const Rect& rect) {
    layoutRect = rect;
}

LayoutStyle& VisualElement::getStyle() {
    return style;
}

const LayoutStyle& VisualElement::getStyle() const {
    return style;
}

void VisualElement::setDisplay(Display display) {
    style.display = display;
    markDirty(DirtyFlags::Layout);
}

Display VisualElement::getDisplay() const {
    return style.display;
}

void VisualElement::setPositionType(PositionType positionType) {
    style.positionType = positionType;
    markDirty(DirtyFlags::Layout);
}

PositionType VisualElement::getPositionType() const {
    return style.positionType;
}

void VisualElement::setFlexDirection(FlexDirection flexDirection) {
    style.flexDirection = flexDirection;
    markDirty(DirtyFlags::Layout);
}

FlexDirection VisualElement::getFlexDirection() const {
    return style.flexDirection;
}

void VisualElement::setFlexWrap(FlexWrap flexWrap) {
    style.flexWrap = flexWrap;
    markDirty(DirtyFlags::Layout);
}

FlexWrap VisualElement::getFlexWrap() const {
    return style.flexWrap;
}

void VisualElement::setJustifyContent(JustifyContent justifyContent) {
    style.justifyContent = justifyContent;
    markDirty(DirtyFlags::Layout);
}

JustifyContent VisualElement::getJustifyContent() const {
    return style.justifyContent;
}

void VisualElement::setAlignItems(AlignItems alignItems) {
    style.alignItems = alignItems;
    markDirty(DirtyFlags::Layout);
}

AlignItems VisualElement::getAlignItems() const {
    return style.alignItems;
}

void VisualElement::setAlignSelf(AlignSelf alignSelf) {
    style.alignSelf = alignSelf;
    markDirty(DirtyFlags::Layout);
}

AlignSelf VisualElement::getAlignSelf() const {
    return style.alignSelf;
}

void VisualElement::setAlignContent(AlignContent alignContent) {
    style.alignContent = alignContent;
    markDirty(DirtyFlags::Layout);
}

AlignContent VisualElement::getAlignContent() const {
    return style.alignContent;
}

void VisualElement::setFlexGrow(float flexGrow) {
    style.flexGrow = flexGrow;
    markDirty(DirtyFlags::Layout);
}

float VisualElement::getFlexGrow() const {
    return style.flexGrow;
}

void VisualElement::setFlexShrink(float flexShrink) {
    style.flexShrink = flexShrink;
    markDirty(DirtyFlags::Layout);
}

float VisualElement::getFlexShrink() const {
    return style.flexShrink;
}

void VisualElement::setFlexBasis(StyleLength flexBasis) {
    style.flexBasis = flexBasis;
    markDirty(DirtyFlags::Layout);
}

void VisualElement::setWidth(StyleLength width) {
    style.width = width;
    markDirty(DirtyFlags::Layout);
}

const StyleLength& VisualElement::getWidth() const {
    return style.width;
}

void VisualElement::setHeight(StyleLength height) {
    style.height = height;
    markDirty(DirtyFlags::Layout);
}

const StyleLength& VisualElement::getHeight() const {
    return style.height;
}

void VisualElement::setMinWidth(StyleLength minWidth) {
    style.minWidth = minWidth;
    markDirty(DirtyFlags::Layout);
}

void VisualElement::setMinHeight(StyleLength minHeight) {
    style.minHeight = minHeight;
    markDirty(DirtyFlags::Layout);
}

void VisualElement::setMaxWidth(StyleLength maxWidth) {
    style.maxWidth = maxWidth;
    markDirty(DirtyFlags::Layout);
}

void VisualElement::setMaxHeight(StyleLength maxHeight) {
    style.maxHeight = maxHeight;
    markDirty(DirtyFlags::Layout);
}

void VisualElement::setMargin(Edge edge, StyleLength margin) {
    switch (edge) {
        case Edge::Left: style.marginLeft = margin; break;
        case Edge::Top: style.marginTop = margin; break;
        case Edge::Right: style.marginRight = margin; break;
        case Edge::Bottom: style.marginBottom = margin; break;
        case Edge::Horizontal: style.marginLeft = margin; style.marginRight = margin; break;
        case Edge::Vertical: style.marginTop = margin; style.marginBottom = margin; break;
        default:
            style.marginLeft = margin;
            style.marginTop = margin;
            style.marginRight = margin;
            style.marginBottom = margin;
            break;
    }
    markDirty(DirtyFlags::Layout);
}

void VisualElement::setMargin(StyleLength margin) {
    setMargin(Edge::All, margin);
}

void VisualElement::setPadding(Edge edge, StyleLength padding) {
    switch (edge) {
        case Edge::Left: style.paddingLeft = padding; break;
        case Edge::Top: style.paddingTop = padding; break;
        case Edge::Right: style.paddingRight = padding; break;
        case Edge::Bottom: style.paddingBottom = padding; break;
        case Edge::Horizontal: style.paddingLeft = padding; style.paddingRight = padding; break;
        case Edge::Vertical: style.paddingTop = padding; style.paddingBottom = padding; break;
        default:
            style.paddingLeft = padding;
            style.paddingTop = padding;
            style.paddingRight = padding;
            style.paddingBottom = padding;
            break;
    }
    markDirty(DirtyFlags::Layout);
}

void VisualElement::setPadding(StyleLength padding) {
    setPadding(Edge::All, padding);
}

void VisualElement::setGap(float gap) {
    style.gap = gap;
    markDirty(DirtyFlags::Layout);
}

float VisualElement::getGap() const {
    return style.gap;
}

void VisualElement::setLeft(StyleLength left) {
    style.left = left;
    markDirty(DirtyFlags::Layout);
}

void VisualElement::setTop(StyleLength top) {
    style.top = top;
    markDirty(DirtyFlags::Layout);
}

void VisualElement::setRight(StyleLength right) {
    style.right = right;
    markDirty(DirtyFlags::Layout);
}

void VisualElement::setBottom(StyleLength bottom) {
    style.bottom = bottom;
    markDirty(DirtyFlags::Layout);
}

bool VisualElement::usesMeasureFunc() const {
    return false;
}

Vector2 VisualElement::measureIntrinsicSize(float availableWidth, bool widthAtMost,
                                            float availableHeight, bool heightAtMost) {
    (void)availableWidth;
    (void)widthAtMost;
    (void)availableHeight;
    (void)heightAtMost;
    return Vector2(0.0f, 0.0f);
}

bool VisualElement::updateLayoutChildren() {
    return false;
}

void VisualElement::setLayoutNode(::YGNode* node) {
    layoutNode = node;
}

::YGNode* VisualElement::getLayoutNode() const {
    return layoutNode;
}

const Vector4& VisualElement::getBackgroundColor() const {
    return backgroundColor;
}

void VisualElement::setBackgroundColor(const Vector4& color) {
    backgroundColor = color;
    markDirty(DirtyFlags::Paint);
}

const Vector4& VisualElement::getBorderColor() const {
    return borderColor;
}

void VisualElement::setBorderColor(const Vector4& color) {
    borderColor = color;
    markDirty(DirtyFlags::Paint);
}

float VisualElement::getBorderWidth() const {
    return borderWidth;
}

void VisualElement::setBorderWidth(float width) {
    borderWidth = width;
    markDirty(DirtyFlags::Paint);
}

float VisualElement::getBorderRadius() const {
    return borderRadius;
}

void VisualElement::setBorderRadius(float radius) {
    borderRadius = radius;
    markDirty(DirtyFlags::Paint);
}

const Texture& VisualElement::getTexture() const {
    return texture;
}

void VisualElement::setTexture(const Texture& texture) {
    this->texture = texture;
    markDirty(DirtyFlags::Paint);
}

const Vector4& VisualElement::getTextureColor() const {
    return textureColor;
}

void VisualElement::setTextureColor(const Vector4& color) {
    textureColor = color;
    markDirty(DirtyFlags::Paint);
}

const Vector4& VisualElement::getNineSlice() const {
    return nineSlice;
}

void VisualElement::setNineSlice(float left, float top, float right, float bottom) {
    nineSlice = Vector4(left, top, right, bottom);
    markDirty(DirtyFlags::Paint);
}

DirtyFlags VisualElement::getDirtyFlags() const {
    return dirty;
}

void VisualElement::markDirty(DirtyFlags flags) {
    dirty = dirty | flags;
}

void VisualElement::clearDirtyFlags(DirtyFlags flags) {
    dirty = dirty & static_cast<DirtyFlags>(~static_cast<unsigned int>(flags));
}

Vector4 VisualElement::applyOpacity(const Vector4& color, const PaintContext& context) {
    const float factor = context.opacity;
    return Vector4(color.x * factor, color.y * factor, color.z * factor, color.w * factor);
}

void VisualElement::paint(PaintContext& context, const Rect& rect) {
    if (context.list == nullptr || !visible)
        return;

    if (rect.getWidth() <= 0.0f || rect.getHeight() <= 0.0f)
        return;

    if (!texture.empty()) {
        const Vector4 tint = applyOpacity(textureColor, context);
        if (tint.w > 0.0f)
            context.list->addImage(rect, texture, tint, nineSlice, context.clip, context.hasClip);
    }else{
        const Vector4 background = applyOpacity(backgroundColor, context);
        if (background.w > 0.0f)
            context.list->addRect(rect, background, borderRadius, context.clip, context.hasClip);
    }

    if (borderWidth > 0.0f) {
        const Vector4 border = applyOpacity(borderColor, context);
        if (border.w > 0.0f)
            context.list->addBorder(rect, border, borderWidth, borderRadius, context.clip, context.hasClip);
    }
}
