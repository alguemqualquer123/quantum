// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "slate/core/EventRouter.h"

#include <algorithm>

namespace quantum::slate {

Widget* EventRouter::focusedWidget = nullptr;
Widget* EventRouter::capturedWidget = nullptr;
Widget* EventRouter::hoveredWidget = nullptr;

namespace {

void collectWidgets(Widget& widget, std::vector<Widget*>& result) {
    if (!widget.isVisible()) return;
    result.push_back(&widget);
    for (const auto& child : widget.getChildren()) collectWidgets(*child, result);
}

Widget* hitTestChildren(Widget& widget, Vector2 position) {
    Rect layout = widget.getLayoutRect();
    if (!widget.isVisible() || !layout.contains(position)) return nullptr;
    std::vector<Widget*> candidates;
    for (const auto& child : widget.getChildren()) {
        Widget* hit = hitTestChildren(*child, position);
        if (hit != nullptr) candidates.push_back(hit);
    }
    if (!candidates.empty()) {
        std::stable_sort(candidates.begin(), candidates.end(), [](Widget* a, Widget* b) {
            return a->getZOrder() < b->getZOrder();
        });
        return candidates.back();
    }
    return widget.isEnabled() ? &widget : nullptr;
}

template <typename Event, typename Capture, typename Target, typename Bubble>
bool route(Widget* target, Event& event, Capture capture, Target targetCall, Bubble bubble) {
    if (target == nullptr) return false;
    std::vector<Widget*> path;
    for (Widget* node = target; node != nullptr; node = node->getParent()) path.push_back(node);
    std::reverse(path.begin(), path.end());
    for (Widget* node : path) {
        event.currentTarget = node;
        event.handled = capture(*node, event) || event.handled;
        if (event.propagationStopped) return event.handled;
    }
    event.currentTarget = target;
    event.handled = targetCall(*target, event) || event.handled;
    if (event.propagationStopped) return event.handled;
    for (auto it = path.rbegin() + 1; it != path.rend(); ++it) {
        event.currentTarget = *it;
        event.handled = bubble(**it, event) || event.handled;
        if (event.propagationStopped) break;
    }
    return event.handled;
}

}

Widget* EventRouter::hitTest(Widget& root, Vector2 position) {
    return hitTestChildren(root, position);
}

bool EventRouter::focus(Widget& root, Widget* target) {
    if (target != nullptr && (target == &root || target->getParent() == nullptr || !target->isFocusable())) {
        return false;
    }
    if (target != nullptr) {
        Widget* node = target;
        while (node != nullptr && node != &root) node = node->getParent();
        if (node != &root || !target->isEnabled() || !target->isVisible()) return false;
    }
    if (focusedWidget != nullptr) focusedWidget->setFocused(false);
    focusedWidget = target;
    if (focusedWidget != nullptr) focusedWidget->setFocused(true);
    return true;
}

bool EventRouter::focusNext(Widget& root, bool reverse) {
    std::vector<Widget*> widgets;
    collectWidgets(root, widgets);
    std::vector<Widget*> focusables;
    for (Widget* widget : widgets) if (widget->isFocusable() && widget->isEnabled()) focusables.push_back(widget);
    if (focusables.empty()) return false;
    auto it = std::find(focusables.begin(), focusables.end(), focusedWidget);
    int index = it == focusables.end() ? (reverse ? static_cast<int>(focusables.size()) - 1 : 0)
        : static_cast<int>(it - focusables.begin()) + (reverse ? -1 : 1);
    if (index < 0) index = static_cast<int>(focusables.size()) - 1;
    if (index >= static_cast<int>(focusables.size())) index = 0;
    return focus(root, focusables[static_cast<size_t>(index)]);
}

bool EventRouter::pointerDown(Widget& root, Vector2 position, int button) {
    Widget* target = hitTest(root, position);
    if (target != nullptr && target->isFocusable()) focus(root, target);
    capturedWidget = target;
    SlatePointerEvent event;
    event.position = position;
    event.button = button;
    return route(target, event,
        [](Widget& widget, SlatePointerEvent& e) { return widget.onPointerDownCapture(e); },
        [](Widget& widget, SlatePointerEvent& e) { return widget.onPointerDown(e); },
        [](Widget& widget, SlatePointerEvent& e) { return widget.onPointerDownBubble(e); });
}

bool EventRouter::pointerUp(Widget& root, Vector2 position, int button) {
    Widget* target = capturedWidget != nullptr ? capturedWidget : hitTest(root, position);
    SlatePointerEvent event;
    event.position = position;
    event.button = button;
    const bool handled = route(target, event,
        [](Widget& widget, SlatePointerEvent& e) { return widget.onPointerUpCapture(e); },
        [](Widget& widget, SlatePointerEvent& e) { return widget.onPointerUp(e); },
        [](Widget& widget, SlatePointerEvent& e) { return widget.onPointerUpBubble(e); });
    capturedWidget = nullptr;
    return handled;
}

bool EventRouter::pointerMove(Widget& root, Vector2 position) {
    Widget* target = capturedWidget != nullptr ? capturedWidget : hitTest(root, position);
    if (hoveredWidget != target) {
        if (hoveredWidget != nullptr) hoveredWidget->setHovered(false);
        hoveredWidget = target;
        if (hoveredWidget != nullptr) hoveredWidget->setHovered(true);
    }
    SlatePointerEvent event;
    event.position = position;
    return route(target, event,
        [](Widget& widget, SlatePointerEvent& e) { return widget.onPointerMoveCapture(e); },
        [](Widget& widget, SlatePointerEvent& e) { return widget.onPointerMove(e); },
        [](Widget& widget, SlatePointerEvent& e) { return widget.onPointerMoveBubble(e); });
}

bool EventRouter::keyDown(Widget& root, int key, bool repeat) {
    if (focusedWidget == nullptr) return false;
    Widget* node = focusedWidget;
    while (node != nullptr && node != &root) node = node->getParent();
    if (node != &root) return false;
    SlateKeyEvent event;
    event.key = key;
    event.repeat = repeat;
    return route(focusedWidget, event,
        [](Widget& widget, SlateKeyEvent& e) { return widget.onKeyDownCapture(e); },
        [](Widget& widget, SlateKeyEvent& e) { return widget.onKeyDown(e); },
        [](Widget& widget, SlateKeyEvent& e) { return widget.onKeyDownBubble(e); });
}

Widget* EventRouter::focused() { return focusedWidget; }

void EventRouter::reset() {
    if (focusedWidget != nullptr) focusedWidget->setFocused(false);
    if (hoveredWidget != nullptr) hoveredWidget->setHovered(false);
    focusedWidget = nullptr;
    capturedWidget = nullptr;
    hoveredWidget = nullptr;
}

}
