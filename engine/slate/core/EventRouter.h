// (c) SR VINIX
// SPDX-License-Identifier: MIT

#ifndef SlateEventRouter_h
#define SlateEventRouter_h

#include "slate/core/Widget.h"

namespace quantum::slate {

class EventRouter {
public:
    static Widget* hitTest(Widget& root, Vector2 position);
    static bool pointerDown(Widget& root, Vector2 position, int button = -1);
    static bool pointerUp(Widget& root, Vector2 position, int button = -1);
    static bool pointerMove(Widget& root, Vector2 position);
    static bool keyDown(Widget& root, int key, bool repeat = false);
    static bool focus(Widget& root, Widget* target);
    static bool focusNext(Widget& root, bool reverse = false);
    static Widget* focused();
    static void reset();

private:
    static Widget* focusedWidget;
    static Widget* capturedWidget;
    static Widget* hoveredWidget;
};

}

#endif // SlateEventRouter_h
