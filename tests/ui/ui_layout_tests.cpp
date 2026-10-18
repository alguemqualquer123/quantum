// (c) Eduardo Doria
// SPDX-License-Identifier: MIT
//
// GPU-free unit tests of the flexbox layout pass (Yoga) and of the UMG-style
// layout containers. Rects are compared against the resolved layout stored on
// the elements; the GPU side is verified visually with doriax-ui-demo.

#include "doctest.h"

#include <memory>

#include "ui/UIRuntime.h"
#include "ui/controls/Label.h"
#include "ui/core/VisualElement.h"
#include "ui/layout/Panels.h"

using namespace doriax;
using namespace doriax::ui;

TEST_CASE("ui::layout stacks a column with padding and gap") {
    std::unique_ptr<VisualElement> root(new VisualElement("root"));
    root->setSize(Vector2(400.0f, 300.0f));

    VBox* column = root->addChild<VBox>("column");
    column->setPadding(10.0f);
    column->setGap(5.0f);

    VisualElement* first = column->addChild<VisualElement>("first");
    first->setSize(Vector2(100.0f, 20.0f));
    VisualElement* second = column->addChild<VisualElement>("second");
    second->setSize(Vector2(100.0f, 20.0f));

    UIRuntime::setRoot(std::move(root));
    UIRuntime::buildFrame();

    CHECK(first->getLayoutRect().getX() == doctest::Approx(10.0f));
    CHECK(first->getLayoutRect().getY() == doctest::Approx(10.0f));
    CHECK(first->getLayoutRect().getWidth() == doctest::Approx(100.0f));
    CHECK(second->getLayoutRect().getX() == doctest::Approx(10.0f));
    CHECK(second->getLayoutRect().getY() == doctest::Approx(35.0f)); //10 + 20 + 5

    UIRuntime::clear();
}

TEST_CASE("ui::layout grows items on the main axis") {
    std::unique_ptr<VisualElement> root(new VisualElement("root"));
    root->setSize(Vector2(400.0f, 100.0f));

    VisualElement* fixed = root->addChild<VisualElement>("fixed");
    fixed->setSize(Vector2(100.0f, 40.0f));

    VisualElement* grown = root->addChild<VisualElement>("grown");
    grown->setWidth(StyleLength::point(100.0f));
    grown->setHeight(StyleLength::point(40.0f));
    grown->setFlexGrow(1.0f);

    UIRuntime::setRoot(std::move(root));
    UIRuntime::buildFrame();

    CHECK(fixed->getLayoutRect().getX() == doctest::Approx(0.0f));
    CHECK(fixed->getLayoutRect().getWidth() == doctest::Approx(100.0f));
    CHECK(grown->getLayoutRect().getX() == doctest::Approx(100.0f));
    //400 - 100 fixed
    CHECK(grown->getLayoutRect().getWidth() == doctest::Approx(300.0f));

    UIRuntime::clear();
}

TEST_CASE("ui::layout resolves percent sizes against the parent") {
    std::unique_ptr<VisualElement> root(new VisualElement("root"));
    root->setSize(Vector2(400.0f, 300.0f));

    VisualElement* half = root->addChild<VisualElement>("half");
    half->setWidth(StyleLength::percent(50.0f));
    half->setHeight(StyleLength::percent(25.0f));

    UIRuntime::setRoot(std::move(root));
    UIRuntime::buildFrame();

    CHECK(half->getLayoutRect().getWidth() == doctest::Approx(200.0f));
    CHECK(half->getLayoutRect().getHeight() == doctest::Approx(75.0f));

    UIRuntime::clear();
}

TEST_CASE("ui::layout keeps the legacy absolute setPosition/setSize") {
    std::unique_ptr<VisualElement> root(new VisualElement("root"));
    root->setSize(Vector2(400.0f, 300.0f));

    VisualElement* pinned = root->addChild<VisualElement>("pinned");
    pinned->setPosition(Vector2(5.0f, 7.0f));
    pinned->setSize(Vector2(10.0f, 10.0f));

    //absolute placement takes the element out of the flex flow: a sibling
    //added before it still starts at the origin
    VisualElement* flow = root->addChild<VisualElement>("flow");
    flow->setSize(Vector2(20.0f, 20.0f));

    UIRuntime::setRoot(std::move(root));
    UIRuntime::buildFrame();

    CHECK(pinned->getLayoutRect().getX() == doctest::Approx(5.0f));
    CHECK(pinned->getLayoutRect().getY() == doctest::Approx(7.0f));
    CHECK(pinned->getLayoutRect().getWidth() == doctest::Approx(10.0f));
    CHECK(flow->getLayoutRect().getX() == doctest::Approx(0.0f));
    CHECK(flow->getLayoutRect().getY() == doctest::Approx(0.0f));

    UIRuntime::clear();
}

TEST_CASE("ui::layout measures labels from the shaped text") {
    std::unique_ptr<VisualElement> root(new VisualElement("root"));
    root->setSize(Vector2(400.0f, 300.0f));

    VBox* column = root->addChild<VBox>("column");
    //content-sized labels keep their measured width instead of stretching
    column->setAlignItems(AlignItems::FlexStart);

    Label* title = column->addChild<Label>("title");
    title->setText("Doriax UI");
    title->setFontSize(24);

    Label* subtitle = column->addChild<Label>("subtitle");
    subtitle->setText("Fase 2a");
    subtitle->setFontSize(12);

    UIRuntime::setRoot(std::move(root));
    UIRuntime::buildFrame();

    CHECK(title->getLayoutRect().getWidth() > 0.0f);
    CHECK(title->getLayoutRect().getHeight() > 0.0f);
    CHECK(title->getLayoutRect().getWidth() < 400.0f);
    CHECK(subtitle->getLayoutRect().getWidth() > 0.0f);
    //stacked right below the first label
    CHECK(subtitle->getLayoutRect().getY() == doctest::Approx(title->getLayoutRect().getHeight()));

    UIRuntime::clear();
}

TEST_CASE("ui::layout wraps the overflow into a new line") {
    std::unique_ptr<VisualElement> root(new VisualElement("root"));
    root->setSize(Vector2(150.0f, 200.0f));

    WrapBox* wrap = root->addChild<WrapBox>("wrap");
    wrap->setWidth(StyleLength::percent(100.0f));
    //lines keep their natural cross size, independent of the free space
    wrap->setAlignContent(AlignContent::FlexStart);

    VisualElement* first = wrap->addChild<VisualElement>("first");
    first->setSize(Vector2(60.0f, 20.0f));
    VisualElement* second = wrap->addChild<VisualElement>("second");
    second->setSize(Vector2(60.0f, 20.0f));
    VisualElement* third = wrap->addChild<VisualElement>("third");
    third->setSize(Vector2(60.0f, 20.0f));

    UIRuntime::setRoot(std::move(root));
    UIRuntime::buildFrame();

    CHECK(first->getLayoutRect().getY() == doctest::Approx(0.0f));
    CHECK(second->getLayoutRect().getX() == doctest::Approx(60.0f));
    //60 + 60 + 60 does not fit in 150: the third item wraps
    CHECK(third->getLayoutRect().getX() == doctest::Approx(0.0f));
    CHECK(third->getLayoutRect().getY() == doctest::Approx(20.0f));

    UIRuntime::clear();
}

TEST_CASE("ui::layout divides the grid panel width into columns") {
    std::unique_ptr<VisualElement> root(new VisualElement("root"));
    root->setSize(Vector2(400.0f, 200.0f));

    GridPanel* grid = root->addChild<GridPanel>("grid");
    grid->setColumns(4);
    grid->setWidth(StyleLength::percent(100.0f));
    grid->setAlignContent(AlignContent::FlexStart);

    VisualElement* cells[8];
    for (int i = 0; i < 8; i++) {
        cells[i] = grid->addChild<VisualElement>("cell");
        cells[i]->setHeight(StyleLength::point(40.0f));
    }

    UIRuntime::setRoot(std::move(root));
    UIRuntime::buildFrame();

    for (int i = 0; i < 4; i++) {
        CHECK(cells[i]->getLayoutRect().getX() == doctest::Approx((float)i * 100.0f));
        CHECK(cells[i]->getLayoutRect().getY() == doctest::Approx(0.0f));
        CHECK(cells[i]->getLayoutRect().getWidth() == doctest::Approx(100.0f));
    }
    for (int i = 4; i < 8; i++) {
        CHECK(cells[i]->getLayoutRect().getX() == doctest::Approx((float)(i - 4) * 100.0f));
        CHECK(cells[i]->getLayoutRect().getY() == doctest::Approx(40.0f));
    }

    UIRuntime::clear();
}

TEST_CASE("ui::layout scrolls the scroll box content by the offset") {
    std::unique_ptr<VisualElement> root(new VisualElement("root"));
    root->setSize(Vector2(400.0f, 300.0f));

    ScrollBox* box = root->addChild<ScrollBox>("box");
    box->setSize(Vector2(200.0f, 100.0f));

    VBox* content = box->addChild<VBox>("content");
    content->setAlignItems(AlignItems::FlexStart);
    for (int i = 0; i < 10; i++) {
        VisualElement* row = content->addChild<VisualElement>("row");
        row->setSize(Vector2(180.0f, 30.0f));
    }

    //positive offset scrolls down: the content moves up inside the viewport
    box->setContentOffset(Vector2(0.0f, 60.0f));

    UIRuntime::setRoot(std::move(root));
    UIRuntime::buildFrame();

    CHECK(box->getLayoutRect().getWidth() == doctest::Approx(200.0f));
    CHECK(content->getLayoutRect().getY() == doctest::Approx(-60.0f));
    CHECK(content->getLayoutRect().getWidth() == doctest::Approx(200.0f));
    CHECK(content->getLayoutRect().getHeight() == doctest::Approx(300.0f)); //10 rows * 30

    box->setContentOffset(Vector2(0.0f, 0.0f));
    UIRuntime::buildFrame();
    CHECK(content->getLayoutRect().getY() == doctest::Approx(0.0f));

    UIRuntime::clear();
}

TEST_CASE("ui::layout size box fills its content") {
    std::unique_ptr<VisualElement> root(new VisualElement("root"));
    root->setSize(Vector2(400.0f, 300.0f));

    SizeBox* box = root->addChild<SizeBox>("box");
    box->setSize(Vector2(200.0f, 120.0f));

    VisualElement* content = box->addChild<VisualElement>("content");
    content->setBackgroundColor(Vector4(0.0f, 0.0f, 0.0f, 1.0f));

    UIRuntime::setRoot(std::move(root));
    UIRuntime::buildFrame();

    CHECK(content->getLayoutRect().getWidth() == doctest::Approx(200.0f));
    CHECK(content->getLayoutRect().getHeight() == doctest::Approx(120.0f));

    UIRuntime::clear();
}

TEST_CASE("ui::layout relayouts only after dirty changes") {
    std::unique_ptr<VisualElement> root(new VisualElement("root"));
    root->setSize(Vector2(400.0f, 300.0f));

    VisualElement* child = root->addChild<VisualElement>("child");
    child->setSize(Vector2(100.0f, 50.0f));

    VisualElement* rootPtr = root.get();
    UIRuntime::setRoot(std::move(root));

    UIRuntime::buildFrame();
    CHECK(child->getLayoutRect().getWidth() == doctest::Approx(100.0f));

    //clean frames keep the resolved rects
    UIRuntime::buildFrame();
    CHECK(child->getLayoutRect().getWidth() == doctest::Approx(100.0f));

    child->setWidth(StyleLength::point(200.0f));
    UIRuntime::buildFrame();
    CHECK(child->getLayoutRect().getWidth() == doctest::Approx(200.0f));

    //a child added at run time enters the flow on the next frame
    VisualElement* added = rootPtr->addChild<VisualElement>("added");
    added->setSize(Vector2(40.0f, 40.0f));
    UIRuntime::buildFrame();
    CHECK(added->getLayoutRect().getWidth() == doctest::Approx(40.0f));
    CHECK(added->getLayoutRect().getX() == doctest::Approx(200.0f));

    //removing (and destroying) the element releases its Yoga node safely
    std::unique_ptr<VisualElement> removed = rootPtr->removeChild(added);
    CHECK(removed.get() == added);
    removed.reset();

    UIRuntime::buildFrame();
    CHECK(rootPtr->getChildCount() == 1u);

    UIRuntime::clear();
}
