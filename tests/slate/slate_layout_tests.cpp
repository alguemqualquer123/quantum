// (c) SR VINIX
// SPDX-License-Identifier: MIT
//
// GPU-free unit tests for the quantum::slate layout layer (engine/slate/layout/
// FlexEngine.cpp and Containers.cpp). FlexEngine::layout is called directly on
// the widget tree: no Slate runtime, no window, no GL context, and the widget
// leaves here are plain Widgets sized through their slot, so no text backend
// is touched either.
//
// NOTE ON THE DOCTEST MAIN: the task brief said slate_paint_tests.cpp does not
// define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN, but it does (see the top of that
// file). Defining it again here would give the quantum-slate-tests link two
// `main`s, so this TU deliberately includes doctest.h as a plain consumer. If
// the paint tests ever drop their main define, move it here.

//Relative include because the cheap syntax-check script does not add
//libs/doctest to the include path; this resolves under CMake too.
#include "../../libs/doctest/doctest.h"

#include <memory>

#include "math/Rect.h"
#include "slate/core/Widget.h"
#include "slate/layout/Containers.h"
#include "slate/layout/FlexEngine.h"

using namespace quantum;
using namespace quantum::slate;

namespace {

    //Plain leaf sized through its slot: FlexEngine writes the node min/max
    //from the slot, so the tree solves deterministically without a text or
    //brush backend.
    Widget* addLeaf(Widget& parent, float width, float height) {
        Widget* leaf = parent.addChild<Widget>();
        leaf->setMinSize(Vector2(width, height));
        return leaf;
    }

    bool sameRect(const Rect& a, const Rect& b) {
        return doctest::Approx(a.getX()) == b.getX() &&
            doctest::Approx(a.getY()) == b.getY() &&
            doctest::Approx(a.getWidth()) == b.getWidth() &&
            doctest::Approx(a.getHeight()) == b.getHeight();
    }

    //The root of the tree is pinned to the canvas, so a Box as root fills it.
    std::unique_ptr<Widget> keepAlive;
}

TEST_SUITE("slate/layout") {
    TEST_CASE("FlexEngine rejects a zero-sized canvas") {
        Box root(Orientation::Horizontal);
        Widget* child = addLeaf(root, 50.0f, 50.0f);

        //a widget that was never solved still holds the default Rect, which in
        //quantum is the 1x1 unit rect rather than a zero rect
        const Rect before = child->getLayoutRect();

        CHECK_FALSE(FlexEngine::layout(root, 0.0f, 100.0f));
        CHECK_FALSE(FlexEngine::layout(root, 100.0f, 0.0f));
        CHECK(sameRect(child->getLayoutRect(), before));

        CHECK(FlexEngine::layout(root, 200.0f, 100.0f));
        CHECK_FALSE(sameRect(child->getLayoutRect(), before));

        FlexEngine::dispose(root);
    }

    TEST_CASE("Box gap and grow") {
        Box root(Orientation::Horizontal);
        root.setGap(10.0f);

        Widget* grower = root.addChild<Widget>();
        grower->setGrow(1.0f);
        Widget* fixed = root.addChild<Widget>();
        fixed->setBasis(100.0f);

        REQUIRE(FlexEngine::layout(root, 410.0f, 600.0f));

        //410 - 10 gap - 100 basis = 300 for the growing child
        CHECK(sameRect(grower->getLayoutRect(), Rect(0.0f, 0.0f, 300.0f, 600.0f)));
        CHECK(sameRect(fixed->getLayoutRect(), Rect(310.0f, 0.0f, 100.0f, 600.0f)));

        FlexEngine::dispose(root);
    }

    TEST_CASE("Box justify space-between and stretch cross axis") {
        Box root(Orientation::Horizontal);
        root.setJustify(Justify::SpaceBetween);

        Widget* first = addLeaf(root, 50.0f, 20.0f);
        Widget* second = addLeaf(root, 50.0f, 20.0f);
        Widget* third = addLeaf(root, 50.0f, 20.0f);

        REQUIRE(FlexEngine::layout(root, 600.0f, 300.0f));

        //600 - 3*50 = 450 of free space split over 2 gutters = 225 each...
        //positions: 0, 50+225, 2*275+50
        CHECK(sameRect(first->getLayoutRect(), Rect(0.0f, 0.0f, 50.0f, 300.0f)));
        CHECK(sameRect(second->getLayoutRect(), Rect(275.0f, 0.0f, 50.0f, 300.0f)));
        CHECK(sameRect(third->getLayoutRect(), Rect(550.0f, 0.0f, 50.0f, 300.0f)));

        FlexEngine::dispose(root);
    }

    TEST_CASE("Box align-items center defers from a Fill slot") {
        Box root(Orientation::Vertical);
        root.setAlignItems(Align::Center);

        Widget* child = addLeaf(root, 100.0f, 50.0f);

        REQUIRE(FlexEngine::layout(root, 800.0f, 600.0f));

        //vertical flow: main axis is height (50), cross axis centered
        CHECK(sameRect(child->getLayoutRect(), Rect(350.0f, 0.0f, 100.0f, 50.0f)));

        FlexEngine::dispose(root);
    }

    TEST_CASE("Slot min and max clamp the resolved size") {
        Box root(Orientation::Horizontal);

        Widget* capped = root.addChild<Widget>();
        capped->setBasis(500.0f);
        capped->setMaxSize(Vector2(200.0f, 0.0f));

        Widget* floored = root.addChild<Widget>();
        floored->setBasis(10.0f);
        floored->setMinSize(Vector2(100.0f, 0.0f));

        REQUIRE(FlexEngine::layout(root, 800.0f, 200.0f));

        CHECK(sameRect(capped->getLayoutRect(), Rect(0.0f, 0.0f, 200.0f, 200.0f)));
        CHECK(sameRect(floored->getLayoutRect(), Rect(200.0f, 0.0f, 100.0f, 200.0f)));

        FlexEngine::dispose(root);
    }

    TEST_CASE("Absolute slot leaves the flow") {
        Box root(Orientation::Horizontal);

        Widget* inFlow = addLeaf(root, 100.0f, 50.0f);
        Widget* overlay = addLeaf(root, 40.0f, 40.0f);
        overlay->setAbsolute(true);

        REQUIRE(FlexEngine::layout(root, 400.0f, 300.0f));

        //the flow item sees no gap for the absolute one; the absolute item is
        //pulled out of the line and sits at the container origin
        CHECK(sameRect(inFlow->getLayoutRect(), Rect(0.0f, 0.0f, 100.0f, 300.0f)));
        CHECK(sameRect(overlay->getLayoutRect(), Rect(0.0f, 0.0f, 40.0f, 40.0f)));

        FlexEngine::dispose(root);
    }

    TEST_CASE("GridPanel column weights share the leftover width") {
        GridPanel grid;
        grid.setColumns(3);
        grid.setGap(30.0f, 10.0f);

        Widget* left = grid.addAt<Widget>(GridPanel::Cell{ 0u, 0u, 1u, 1u });
        left->setMinSize(Vector2(0.0f, 40.0f));
        Widget* content = grid.addAt<Widget>(0u, 1u);
        content->setMinSize(Vector2(100.0f, 40.0f));
        Widget* right = grid.addAt<Widget>(0u, 2u);
        right->setMinSize(Vector2(0.0f, 40.0f));

        grid.setColumnWeight(0, 1.0f);
        grid.setColumnWeight(2, 2.0f);

        REQUIRE(FlexEngine::layout(grid, 600.0f, 600.0f));

        //free width = 600 - 2*30 gaps - 100 content = 440 split 1:2
        const float w0 = 440.0f / 3.0f;
        const float w2 = 440.0f * 2.0f / 3.0f;
        CHECK(sameRect(left->getLayoutRect(), Rect(0.0f, 0.0f, w0, 40.0f)));
        CHECK(sameRect(content->getLayoutRect(), Rect(w0 + 30.0f, 0.0f, 100.0f, 40.0f)));
        CHECK(sameRect(right->getLayoutRect(), Rect(w0 + 30.0f + 100.0f + 30.0f, 0.0f, w2, 40.0f)));

        FlexEngine::dispose(grid);
    }

    TEST_CASE("GridPanel row weights share the leftover height") {
        GridPanel grid;
        grid.setColumns(1);
        grid.setGap(0.0f, 10.0f);
        grid.setRowWeight(0, 1.0f);
        grid.setRowWeight(1, 2.0f);

        Widget* top = grid.addAt<Widget>(0u, 0u);
        Widget* bottom = grid.addAt<Widget>(1u, 0u);

        REQUIRE(FlexEngine::layout(grid, 600.0f, 600.0f));

        //600 - 10 gap split 1:2 over the two weighted rows
        CHECK(doctest::Approx(top->getLayoutRect().getHeight()) == 590.0f / 3.0f);
        CHECK(doctest::Approx(top->getLayoutRect().getY()) == 0.0f);
        CHECK(doctest::Approx(bottom->getLayoutRect().getY()) == 590.0f / 3.0f + 10.0f);
        CHECK(doctest::Approx(bottom->getLayoutRect().getHeight()) == 590.0f * 2.0f / 3.0f);

        FlexEngine::dispose(grid);
    }

    TEST_CASE("CanvasPanel anchors survive a resize") {
        CanvasPanel canvas;

        //top-right corner badge: pinned with a fixed 120x50 size
        Widget* badge = canvas.addChild<Widget>();
        badge->setAnchors(1.0f, 0.0f, 1.0f, 0.0f);
        badge->setOffsets(-120.0f, 20.0f, 0.0f, -70.0f);

        //stretched inset panel: anchors cover the whole canvas, 10/20/30/40
        //pixels from each edge
        Widget* panel = canvas.addChild<Widget>();
        panel->setAnchors(0.0f, 0.0f, 1.0f, 1.0f);
        panel->setOffsets(10.0f, 20.0f, 30.0f, 40.0f);

        REQUIRE(FlexEngine::layout(canvas, 800.0f, 600.0f));
        CHECK(sameRect(badge->getLayoutRect(), Rect(680.0f, 20.0f, 120.0f, 50.0f)));
        CHECK(sameRect(panel->getLayoutRect(), Rect(10.0f, 20.0f, 760.0f, 540.0f)));

        //resize: the badge stays glued to the corner, the panel reflows
        REQUIRE(FlexEngine::layout(canvas, 1024.0f, 768.0f));
        CHECK(sameRect(badge->getLayoutRect(), Rect(904.0f, 20.0f, 120.0f, 50.0f)));
        CHECK(sameRect(panel->getLayoutRect(), Rect(10.0f, 20.0f, 984.0f, 708.0f)));

        FlexEngine::dispose(canvas);
    }

    TEST_CASE("SizeBox forces its dimensions onto the child") {
        Box root(Orientation::Vertical);

        SizeBox* sizeBox = root.addChild<SizeBox>();
        sizeBox->setWidth(150.0f);
        sizeBox->setHeight(60.0f);
        Widget* inner = addLeaf(*sizeBox, 0.0f, 0.0f);

        REQUIRE(FlexEngine::layout(root, 400.0f, 300.0f));

        CHECK(sameRect(sizeBox->getLayoutRect(), Rect(0.0f, 0.0f, 150.0f, 60.0f)));
        CHECK(sameRect(inner->getLayoutRect(), Rect(0.0f, 0.0f, 150.0f, 60.0f)));

        FlexEngine::dispose(root);
    }

    TEST_CASE("ScrollBox measures its content and applies the offset") {
        Box root(Orientation::Vertical);

        ScrollBox* scroll = root.addChild<ScrollBox>();
        scroll->setOrientation(Orientation::Vertical);
        scroll->setGap(10.0f);
        //a scroll viewport never asks for its content height (that is what the
        //scroll axis is for), so it has to grow into the parent to be visible
        scroll->setGrow(1.0f);
        Widget* rowA = scroll->addChild<Widget>();
        rowA->setMinSize(Vector2(0.0f, 100.0f));
        Widget* rowB = scroll->addChild<Widget>();
        rowB->setMinSize(Vector2(0.0f, 100.0f));
        Widget* rowC = scroll->addChild<Widget>();
        rowC->setMinSize(Vector2(0.0f, 100.0f));

        REQUIRE(FlexEngine::layout(root, 400.0f, 300.0f));

        //viewport = the whole canvas (grow fills the vertical box), content
        //keeps its natural height of 3*100 + 2*10
        CHECK(sameRect(scroll->getLayoutRect(), Rect(0.0f, 0.0f, 400.0f, 300.0f)));
        CHECK(doctest::Approx(scroll->getContentSize().x) == 400.0f);
        CHECK(doctest::Approx(scroll->getContentSize().y) == 320.0f);
        CHECK(doctest::Approx(rowA->getLayoutRect().getY()) == 0.0f);
        CHECK(doctest::Approx(rowB->getLayoutRect().getY()) == 110.0f);
        CHECK(doctest::Approx(rowC->getLayoutRect().getY()) == 220.0f);

        scroll->setContentOffset(Vector2(0.0f, 120.0f));
        REQUIRE(FlexEngine::layout(root, 400.0f, 300.0f));

        CHECK(doctest::Approx(rowA->getLayoutRect().getY()) == -120.0f);
        CHECK(doctest::Approx(rowB->getLayoutRect().getY()) == -10.0f);
        CHECK(doctest::Approx(rowC->getLayoutRect().getY()) == 100.0f);

        scroll->scrollToEnd();
        REQUIRE(FlexEngine::layout(root, 400.0f, 300.0f));
        CHECK(doctest::Approx(scroll->getContentOffset().y) == 20.0f);
        //Rect exposes no bottom edge accessor, so the last row is checked as
        //origin + size against the viewport bottom
        CHECK(doctest::Approx(rowC->getLayoutRect().getY() + rowC->getLayoutRect().getHeight()) == 300.0f);

        //the clipped paint path emits the content through the viewport rect
        PaintList list;
        PaintContext context;
        context.list = &list;
        context.clip = Rect(0.0f, 0.0f, 400.0f, 300.0f);
        context.hasClip = false;
        context.opacity = 1.0f;
        scroll->paint(context);
        CHECK(list.getCommandCount() == 0u);   //transparent leaves paint nothing

        FlexEngine::dispose(root);
    }

    TEST_CASE("Overlay children share one rect") {
        Box root(Orientation::Horizontal);

        Overlay* overlay = root.addChild<Overlay>();
        Widget* big = addLeaf(*overlay, 200.0f, 80.0f);
        //not named `small`: rpcndr.h (through the platform headers) defines it
        //as a macro, so the identifier would expand to 1 under MSVC
        Widget* little = addLeaf(*overlay, 100.0f, 50.0f);

        REQUIRE(FlexEngine::layout(root, 600.0f, 600.0f));

        //the overlay measures to its largest child on the main axis and the
        //cross axis stretches; both children get that exact rect
        CHECK(sameRect(big->getLayoutRect(), Rect(0.0f, 0.0f, 200.0f, 600.0f)));
        CHECK(sameRect(little->getLayoutRect(), Rect(0.0f, 0.0f, 200.0f, 600.0f)));

        FlexEngine::dispose(root);
    }

    TEST_CASE("ScaleBox keeps the design size and reports the paint scale") {
        CanvasPanel canvas;

        ScaleBox* scale = canvas.addChild<ScaleBox>();
        //a canvas child is placed by its anchors; the default (0,0,0,0) anchors
        //are a point in the corner, so a full-bleed element asks for the whole
        //canvas
        scale->setAnchors(0.0f, 0.0f, 1.0f, 1.0f);
        scale->setDesignSize(Vector2(1920.0f, 1080.0f));
        Widget* inner = addLeaf(*scale, 100.0f, 50.0f);

        REQUIRE(FlexEngine::layout(canvas, 1280.0f, 720.0f));

        CHECK(sameRect(scale->getLayoutRect(), Rect(0.0f, 0.0f, 1280.0f, 720.0f)));
        //the tree under the ScaleBox solves at the design size; the renderer
        //multiplies it by getScale() through the PaintContext
        CHECK(sameRect(inner->getLayoutRect(), Rect(0.0f, 0.0f, 1920.0f, 1080.0f)));
        CHECK(doctest::Approx(scale->getScale().x) == 1280.0f / 1920.0f);
        CHECK(doctest::Approx(scale->getScale().y) == 720.0f / 1080.0f);

        FlexEngine::dispose(canvas);
    }

    TEST_CASE("dispose frees every node and layout rebuilds after it") {
        Box root(Orientation::Horizontal);
        Widget* child = addLeaf(root, 80.0f, 30.0f);

        REQUIRE(FlexEngine::layout(root, 300.0f, 200.0f));
        CHECK(root.getLayoutNode() != nullptr);
        CHECK(child->getLayoutNode() != nullptr);

        FlexEngine::dispose(root);
        CHECK(root.getLayoutNode() == nullptr);
        CHECK(child->getLayoutNode() == nullptr);

        //a rebuilt tree must solve again from scratch without touching freed
        //memory (the new nodes come from the widgets' current state)
        REQUIRE(FlexEngine::layout(root, 300.0f, 200.0f));
        CHECK(root.getLayoutNode() != nullptr);
        CHECK(sameRect(child->getLayoutRect(), Rect(0.0f, 0.0f, 80.0f, 200.0f)));

        FlexEngine::dispose(root);
    }

    TEST_CASE("layout stays stable when nothing is dirty") {
        Box root(Orientation::Horizontal);
        root.setGap(10.0f);
        Widget* a = addLeaf(root, 60.0f, 40.0f);
        Widget* b = addLeaf(root, 60.0f, 40.0f);

        REQUIRE(FlexEngine::layout(root, 500.0f, 200.0f));
        const Rect firstA = a->getLayoutRect();

        REQUIRE(FlexEngine::layout(root, 500.0f, 200.0f));
        CHECK(sameRect(a->getLayoutRect(), firstA));

        root.setGap(30.0f);
        REQUIRE(FlexEngine::layout(root, 500.0f, 200.0f));
        CHECK(doctest::Approx(b->getLayoutRect().getX()) == 90.0f);

        FlexEngine::dispose(root);
    }

    TEST_CASE("Slot margins offset the flow") {
        Box root(Orientation::Horizontal);

        Widget* a = addLeaf(root, 50.0f, 20.0f);
        a->setMargin(Insets(0.0f, 0.0f, 10.0f, 0.0f));
        Widget* b = addLeaf(root, 30.0f, 20.0f);
        b->setMargin(Insets(20.0f, 5.0f, 0.0f, 0.0f));

        REQUIRE(FlexEngine::layout(root, 400.0f, 100.0f));

        CHECK(sameRect(a->getLayoutRect(), Rect(0.0f, 0.0f, 50.0f, 100.0f)));
        //30 wide: a's 10 right margin plus b's own 20 left margin; the stretched
        //cross size is the line minus the vertical margins
        CHECK(sameRect(b->getLayoutRect(), Rect(80.0f, 5.0f, 30.0f, 95.0f)));

        FlexEngine::dispose(root);
    }

    TEST_CASE("A scroll viewport claims no space on its own scroll axis") {
        Box root(Orientation::Vertical);

        ScrollBox* scroll = root.addChild<ScrollBox>();
        scroll->setGap(4.0f);
        scroll->setGrow(1.0f);
        Widget* tall = addLeaf(*scroll, 0.0f, 400.0f);
        Widget* footer = addLeaf(root, 0.0f, 60.0f);

        REQUIRE(FlexEngine::layout(root, 300.0f, 200.0f));

        //content far taller than the box: the footer keeps its 60 and the
        //viewport takes the rest; the row keeps its natural 400 inside, clipped
        //at paint time
        CHECK(sameRect(scroll->getLayoutRect(), Rect(0.0f, 0.0f, 300.0f, 140.0f)));
        CHECK(sameRect(footer->getLayoutRect(), Rect(0.0f, 140.0f, 300.0f, 60.0f)));
        CHECK(sameRect(tall->getLayoutRect(), Rect(0.0f, 0.0f, 300.0f, 400.0f)));
        CHECK(doctest::Approx(scroll->getContentSize().y) == 400.0f);

        FlexEngine::dispose(root);
    }

    TEST_CASE("Box::arrange places the flow without the engine") {
        Box root(Orientation::Horizontal);
        root.setGap(10.0f);
        root.setJustify(Justify::End);

        Widget* a = addLeaf(root, 60.0f, 20.0f);
        Widget* b = addLeaf(root, 40.0f, 20.0f);

        //200 - (60 + 10 + 40) = 90 of free space pushed to the head
        root.arrange(Rect(0.0f, 0.0f, 200.0f, 100.0f));

        CHECK(sameRect(a->getLayoutRect(), Rect(90.0f, 0.0f, 60.0f, 100.0f)));
        CHECK(sameRect(b->getLayoutRect(), Rect(160.0f, 0.0f, 40.0f, 100.0f)));
    }

    TEST_CASE("GridPanel::arrange resolves cells and weights by hand") {
        GridPanel grid;
        grid.setColumns(2);
        grid.setRows(2);
        grid.setGap(10.0f, 6.0f);
        grid.setColumnWeight(1, 1.0f);

        Widget* a = grid.addAt<Widget>(0u, 0u);
        a->setMinSize(Vector2(50.0f, 20.0f));
        Widget* b = grid.addAt<Widget>(0u, 1u);
        b->setMinSize(Vector2(0.0f, 20.0f));
        Widget* c = grid.addAt<Widget>(1u, 0u);
        c->setMinSize(Vector2(0.0f, 30.0f));

        grid.arrange(Rect(0.0f, 0.0f, 300.0f, 100.0f));

        //column 0 sizes to its content (50), the weighted column takes the rest
        CHECK(sameRect(a->getLayoutRect(), Rect(0.0f, 0.0f, 50.0f, 20.0f)));
        CHECK(sameRect(b->getLayoutRect(), Rect(60.0f, 0.0f, 240.0f, 20.0f)));
        CHECK(sameRect(c->getLayoutRect(), Rect(0.0f, 26.0f, 50.0f, 30.0f)));
    }

    TEST_CASE("CanvasPanel::arrange resolves the anchors by hand") {
        CanvasPanel canvas;

        Widget* badge = canvas.addChild<Widget>();
        badge->setAnchors(1.0f, 0.0f, 1.0f, 0.0f);
        badge->setOffsets(-140.0f, 12.0f, 0.0f, -52.0f);

        canvas.arrange(Rect(0.0f, 0.0f, 500.0f, 250.0f));

        CHECK(sameRect(badge->getLayoutRect(), Rect(360.0f, 12.0f, 140.0f, 40.0f)));
    }

    TEST_CASE("ScrollBox::arrange stacks and shifts the content by hand") {
        ScrollBox scroll;
        scroll.setOrientation(Orientation::Vertical);
        scroll.setGap(8.0f);

        Widget* a = scroll.addChild<Widget>();
        a->setMinSize(Vector2(0.0f, 50.0f));
        Widget* b = scroll.addChild<Widget>();
        b->setMinSize(Vector2(0.0f, 50.0f));
        Widget* c = scroll.addChild<Widget>();
        c->setMinSize(Vector2(0.0f, 50.0f));

        scroll.arrange(Rect(0.0f, 0.0f, 300.0f, 120.0f));
        CHECK(sameRect(a->getLayoutRect(), Rect(0.0f, 0.0f, 300.0f, 50.0f)));
        CHECK(sameRect(b->getLayoutRect(), Rect(0.0f, 58.0f, 300.0f, 50.0f)));

        scroll.setContentOffset(Vector2(0.0f, 100.0f));
        scroll.arrange(Rect(0.0f, 0.0f, 300.0f, 120.0f));
        CHECK(sameRect(c->getLayoutRect(), Rect(0.0f, 16.0f, 300.0f, 50.0f)));
    }
}
