// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "../../libs/doctest/doctest.h"

#include <string>
#include <utility>
#include <vector>

#include "Input.h"
#include "slate/core/EventRouter.h"

using namespace quantum;
using namespace quantum::slate;

namespace {

class TraceWidget final : public Widget {
public:
    explicit TraceWidget(std::string name) : Widget(std::move(name)) {}

    std::vector<std::string> phases;
    bool stopCapture = false;
    bool consumeTarget = false;

    bool onPointerDownCapture(SlatePointerEvent& event) override {
        phases.push_back("capture:" + event.currentTarget->getId());
        if (stopCapture) event.stopPropagation();
        return false;
    }

    bool onPointerDown(SlatePointerEvent& event) override {
        phases.push_back("target:" + event.currentTarget->getId());
        return consumeTarget;
    }

    bool onPointerDownBubble(SlatePointerEvent& event) override {
        phases.push_back("bubble:" + event.currentTarget->getId());
        return false;
    }
};

}

TEST_CASE("slate::EventRouter capture target bubble and focus") {
    EventRouter::reset();
    TraceWidget root("root");
    auto* child = root.addChild<TraceWidget>("button");
    child->setFocusable(true);
    root.setLayoutRect(Rect(0.0f, 0.0f, 400.0f, 300.0f));
    child->setLayoutRect(Rect(20.0f, 30.0f, 120.0f, 50.0f));

    CHECK(EventRouter::pointerDown(root, Vector2(40.0f, 40.0f), D_MOUSE_BUTTON_LEFT) == false);
    CHECK(EventRouter::focused() == child);
    REQUIRE(child->phases.size() == 2u);
    CHECK(child->phases[0] == "capture:button");
    CHECK(child->phases[1] == "target:button");
    REQUIRE(root.phases.size() == 2u);
    CHECK(root.phases[0] == "capture:root");
    CHECK(root.phases[1] == "bubble:root");

    CHECK(EventRouter::pointerMove(root, Vector2(40.0f, 40.0f)) == false);
    CHECK(child->isHovered());
    CHECK(EventRouter::pointerUp(root, Vector2(40.0f, 40.0f), D_MOUSE_BUTTON_LEFT) == false);
    EventRouter::reset();
}

TEST_CASE("slate::EventRouter stops at capture and advances focus") {
    EventRouter::reset();
    TraceWidget root("root");
    auto* first = root.addChild<TraceWidget>("first");
    auto* second = root.addChild<TraceWidget>("second");
    first->setFocusable(true);
    second->setFocusable(true);
    root.setLayoutRect(Rect(0.0f, 0.0f, 400.0f, 300.0f));
    first->setLayoutRect(Rect(10.0f, 10.0f, 80.0f, 40.0f));
    second->setLayoutRect(Rect(100.0f, 10.0f, 80.0f, 40.0f));

    first->stopCapture = true;
    CHECK(EventRouter::pointerDown(root, Vector2(20.0f, 20.0f)) == false);
    REQUIRE(first->phases.size() == 1u);
    CHECK(first->phases[0] == "capture:first");
    CHECK(EventRouter::focusNext(root));
    CHECK(EventRouter::focused() == second);
    CHECK(EventRouter::focusNext(root, true));
    CHECK(EventRouter::focused() == first);
    EventRouter::reset();
}
