// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#include "ui/layout/Panels.h"

#include <utility>

using namespace doriax;
using namespace doriax::ui;

HBox::HBox() {
    setFlexDirection(FlexDirection::Row);
}

HBox::HBox(std::string id) : VisualElement(std::move(id)) {
    setFlexDirection(FlexDirection::Row);
}

VBox::VBox() {
    setFlexDirection(FlexDirection::Column);
}

VBox::VBox(std::string id) : VisualElement(std::move(id)) {
    setFlexDirection(FlexDirection::Column);
}

WrapBox::WrapBox() {
    setFlexDirection(FlexDirection::Row);
    setFlexWrap(FlexWrap::Wrap);
}

WrapBox::WrapBox(std::string id) : VisualElement(std::move(id)) {
    setFlexDirection(FlexDirection::Row);
    setFlexWrap(FlexWrap::Wrap);
}

GridPanel::GridPanel() :
    columns(2) {
    setFlexDirection(FlexDirection::Row);
    setFlexWrap(FlexWrap::Wrap);
}

GridPanel::GridPanel(std::string id) : GridPanel() {
    setId(std::move(id));
}

void GridPanel::setColumns(unsigned int columns) {
    if (this->columns == columns)
        return;

    this->columns = columns;
    markDirty(DirtyFlags::Layout);
}

unsigned int GridPanel::getColumns() const {
    return columns;
}

bool GridPanel::updateLayoutChildren() {
    if (columns == 0)
        return false;

    const float cellWidth = 100.0f / (float)columns;
    for (const std::unique_ptr<VisualElement>& child : getChildren()) {
        child->getStyle().width = StyleLength::percent(cellWidth);
    }

    return true;
}

ScrollBox::ScrollBox() :
    contentOffset(0.0f, 0.0f) {
    setFlexDirection(FlexDirection::Column);
    setOverflow(Overflow::Hidden);
}

ScrollBox::ScrollBox(std::string id) : ScrollBox() {
    setId(std::move(id));
}

void ScrollBox::setContentOffset(Vector2 offset) {
    if (contentOffset == offset)
        return;

    contentOffset = offset;
    markDirty(DirtyFlags::Layout);
}

const Vector2& ScrollBox::getContentOffset() const {
    return contentOffset;
}

bool ScrollBox::updateLayoutChildren() {
    if (getChildCount() == 0)
        return false;

    //the content is pinned to the viewport and shifted by the scroll offset;
    //absolute positioning keeps it out of the flex flow of the box itself
    LayoutStyle& contentStyle = getChildren()[0]->getStyle();
    contentStyle.positionType = PositionType::Absolute;
    contentStyle.left = StyleLength::point(-contentOffset.x);
    contentStyle.top = StyleLength::point(-contentOffset.y);
    if (contentStyle.width.unit == StyleLength::Unit::Auto)
        contentStyle.width = StyleLength::percent(100.0f);

    return true;
}

SizeBox::SizeBox() {
    setFlexDirection(FlexDirection::Column);
}

SizeBox::SizeBox(std::string id) : VisualElement(std::move(id)) {
    setFlexDirection(FlexDirection::Column);
}

bool SizeBox::updateLayoutChildren() {
    for (const std::unique_ptr<VisualElement>& child : getChildren()) {
        LayoutStyle& childStyle = child->getStyle();
        if (childStyle.width.unit == StyleLength::Unit::Auto)
            childStyle.width = StyleLength::percent(100.0f);
        if (childStyle.height.unit == StyleLength::Unit::Auto)
            childStyle.height = StyleLength::percent(100.0f);
    }

    return true;
}
