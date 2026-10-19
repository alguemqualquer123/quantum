// (c) Eduardo Doria
// SPDX-License-Identifier: MIT
//
// GPU-free unit tests of the retained-mode UI module (engine/core/ui/).
// Covers the element tree, the paint list, the mesh builder and the runtime
// walker; the Sokol renderer itself is verified visually with doriax-ui-demo.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <memory>
#include <string>

#include "ui/UIRuntime.h"
#include "ui/controls/Label.h"
#include "ui/core/VisualElement.h"
#include "ui/render/FontAtlas.h"
#include "ui/render/PaintList.h"
#include "ui/render/VertexBuilder.h"

using namespace doriax;
using namespace doriax::ui;

TEST_CASE("ui::VisualElement hierarchy") {
    VisualElement root("root");
    VisualElement* a = root.addChild<VisualElement>("a");
    VisualElement* b = root.addChild<VisualElement>("b");

    CHECK(root.getChildCount() == 2u);
    CHECK(root.getSubtreeCount() == 3u);
    CHECK(a->getParent() == &root);
    CHECK(a->getId() == "a");

    std::unique_ptr<VisualElement> removed = root.removeChild(a);
    CHECK(removed.get() == a);
    CHECK(a->getParent() == nullptr);
    CHECK(root.getChildCount() == 1u);

    b->addToClassList("primary");
    b->addToClassList("primary");
    CHECK(b->hasClass("primary"));
    CHECK(!b->hasClass("missing"));
    CHECK(b->getClasses().size() == 1u);

    //b (and every other child) is destroyed here; do not touch it afterwards
    root.clearChildren();
    CHECK(root.getChildCount() == 0u);
}

TEST_CASE("ui::VisualElement defaults and dirty flags") {
    VisualElement element("e");

    CHECK(element.isVisible());
    CHECK(element.isEnabled());
    CHECK(element.getOpacity() == 1.0f);
    CHECK(element.getOverflow() == Overflow::Visible);
    CHECK(element.getBackgroundColor() == Vector4(0.0f, 0.0f, 0.0f, 0.0f));
    CHECK(element.getTextureColor() == Vector4(1.0f, 1.0f, 1.0f, 1.0f));

    element.clearDirtyFlags();
    CHECK(element.getDirtyFlags() == DirtyFlags::None);

    element.setBackgroundColor(Vector4(1.0f, 0.0f, 0.0f, 1.0f));
    CHECK((element.getDirtyFlags() & DirtyFlags::Paint) == DirtyFlags::Paint);
    CHECK((element.getDirtyFlags() & DirtyFlags::Layout) == DirtyFlags::None);

    element.setPosition(Vector2(5.0f, 5.0f));
    CHECK((element.getDirtyFlags() & DirtyFlags::Layout) == DirtyFlags::Layout);
}

TEST_CASE("ui::VisualElement::paint applies the context opacity to colors") {
    VisualElement element("e");
    element.setBackgroundColor(Vector4(1.0f, 0.5f, 0.0f, 1.0f));
    element.setOpacity(0.5f);

    PaintList list;
    PaintContext context;
    context.list = &list;
    context.opacity = 0.5f; //inherited half (the walker pre-multiplies self opacity)

    element.paint(context, Rect(10.0f, 20.0f, 100.0f, 50.0f));

    REQUIRE(list.getCommandCount() == 1u);
    const PaintCommand& command = list.getCommands()[0];
    CHECK(command.type == PaintCommand::Type::Rect);
    CHECK(command.rect.getX() == 10.0f);
    CHECK(command.rect.getWidth() == 100.0f);
    CHECK(command.color.x == doctest::Approx(0.5f));
    CHECK(command.color.y == doctest::Approx(0.25f));
    CHECK(command.color.w == doctest::Approx(0.5f));
    CHECK(!command.hasClip);

    //invisible elements and empty rects emit nothing
    element.setVisible(false);
    element.paint(context, Rect(0.0f, 0.0f, 10.0f, 10.0f));
    CHECK(list.getCommandCount() == 1u);

    element.setVisible(true);
    element.paint(context, Rect(0.0f, 0.0f, 0.0f, 10.0f));
    CHECK(list.getCommandCount() == 1u);
}

TEST_CASE("ui::UIRuntime clips children of overflow hidden elements") {
    std::unique_ptr<VisualElement> root(new VisualElement("root"));

    VisualElement* outer = root->addChild<VisualElement>("outer");
    outer->setPosition(Vector2(10.0f, 10.0f));
    outer->setSize(Vector2(50.0f, 50.0f));
    outer->setOverflow(Overflow::Hidden);

    VisualElement* inner = outer->addChild<VisualElement>("inner");
    inner->setSize(Vector2(200.0f, 200.0f));
    inner->setOverflow(Overflow::Hidden);

    VisualElement* leaf = inner->addChild<VisualElement>("leaf");
    leaf->setSize(Vector2(10.0f, 10.0f));
    leaf->setBackgroundColor(Vector4(1.0f, 1.0f, 1.0f, 1.0f));
    leaf->setOpacity(0.5f); //the walker must fold this into the command color

    UIRuntime::setRoot(std::move(root));
    UIRuntime::buildFrame();

    const UIRuntime::Stats& stats = UIRuntime::getStats();
    CHECK(stats.elements == 4u);
    CHECK(stats.commands == 1u);

    REQUIRE(UIRuntime::getPaintList().getCommandCount() == 1u);
    const PaintCommand& command = UIRuntime::getPaintList().getCommands()[0];
    CHECK(command.hasClip);
    CHECK(command.clip.getX() == 10.0f);
    CHECK(command.clip.getY() == 10.0f);
    CHECK(command.clip.getWidth() == 50.0f);
    CHECK(command.clip.getHeight() == 50.0f);
    CHECK(command.color.w == doctest::Approx(0.5f));

    //leaf rect is (10,10,10,10): the mesh must keep the clip for the GPU scissor
    REQUIRE(UIRuntime::getMesh().ranges.size() == 1u);
    CHECK(UIRuntime::getMesh().ranges[0].hasClip);
    CHECK(UIRuntime::getMesh().ranges[0].clip.getWidth() == 50.0f);

    UIRuntime::clear();
}

TEST_CASE("ui::VertexBuilder solid rect emits one quad and one range") {
    PaintList list;
    list.addRect(Rect(0.0f, 0.0f, 100.0f, 40.0f), Vector4(1.0f, 1.0f, 1.0f, 1.0f), 0.0f, Rect(), false);

    UIMesh mesh;
    VertexBuilder::build(list, mesh);

    CHECK(mesh.vertices.size() == 4u);
    CHECK(mesh.indices.size() == 6u);
    REQUIRE(mesh.ranges.size() == 1u);
    CHECK(mesh.ranges[0].indexOffset == 0u);
    CHECK(mesh.ranges[0].indexCount == 6u);
    CHECK(!mesh.ranges[0].fontAtlas);
    CHECK(mesh.vertices[0].position.x == 0.0f);
    CHECK(mesh.vertices[0].position.y == 0.0f);
    CHECK(mesh.vertices[2].position.x == 100.0f);
    CHECK(mesh.vertices[2].position.y == 40.0f);
}

TEST_CASE("ui::VertexBuilder rounded rect emits a triangle fan") {
    PaintList list;
    list.addRect(Rect(0.0f, 0.0f, 100.0f, 100.0f), Vector4(1.0f, 1.0f, 1.0f, 1.0f), 20.0f, Rect(), false);

    UIMesh mesh;
    VertexBuilder::build(list, mesh);

    //one center vertex plus 4 arcs of the same segment count
    CHECK(mesh.vertices.size() > 4u);
    CHECK((mesh.vertices.size() - 1u) % 4u == 0u);
    CHECK(mesh.indices.size() == (mesh.vertices.size() - 1u) * 3u);
}

TEST_CASE("ui::VertexBuilder border emits a matched ring") {
    PaintList list;
    list.addBorder(Rect(0.0f, 0.0f, 50.0f, 50.0f), Vector4(1.0f, 1.0f, 1.0f, 1.0f), 5.0f, 0.0f, Rect(), false);

    UIMesh mesh;
    VertexBuilder::build(list, mesh);

    //square ring: 4 outline points -> 4 quads
    CHECK(mesh.vertices.size() == 16u);
    CHECK(mesh.indices.size() == 24u);

    //rounded ring keeps matching point counts (outer and inner arcs)
    PaintList rounded;
    rounded.addBorder(Rect(0.0f, 0.0f, 60.0f, 40.0f), Vector4(1.0f, 1.0f, 1.0f, 1.0f), 3.0f, 10.0f, Rect(), false);

    UIMesh roundedMesh;
    VertexBuilder::build(rounded, roundedMesh);
    CHECK(roundedMesh.vertices.size() % 4u == 0u);
    CHECK(roundedMesh.indices.size() == (roundedMesh.vertices.size() / 4u) * 6u);
}

TEST_CASE("ui::VertexBuilder merges ranges by shared state") {
    PaintList list;
    list.addRect(Rect(0.0f, 0.0f, 10.0f, 10.0f), Vector4(1.0f, 1.0f, 1.0f, 1.0f), 0.0f, Rect(), false);
    list.addRect(Rect(10.0f, 0.0f, 10.0f, 10.0f), Vector4(1.0f, 1.0f, 1.0f, 1.0f), 0.0f, Rect(), false);

    UIMesh mesh;
    VertexBuilder::build(list, mesh);

    REQUIRE(mesh.ranges.size() == 1u);
    CHECK(mesh.ranges[0].indexCount == 12u);

    PaintList clipped;
    clipped.addRect(Rect(0.0f, 0.0f, 10.0f, 10.0f), Vector4(1.0f, 1.0f, 1.0f, 1.0f), 0.0f, Rect(0.0f, 0.0f, 5.0f, 5.0f), true);
    clipped.addRect(Rect(0.0f, 0.0f, 10.0f, 10.0f), Vector4(1.0f, 1.0f, 1.0f, 1.0f), 0.0f, Rect(0.0f, 0.0f, 6.0f, 6.0f), true);
    clipped.addRect(Rect(0.0f, 0.0f, 10.0f, 10.0f), Vector4(1.0f, 1.0f, 1.0f, 1.0f), 0.0f, Rect(0.0f, 0.0f, 6.0f, 6.0f), true);

    UIMesh clippedMesh;
    VertexBuilder::build(clipped, clippedMesh);

    REQUIRE(clippedMesh.ranges.size() == 2u);
    CHECK(clippedMesh.ranges[0].indexOffset == 0u);
    CHECK(clippedMesh.ranges[0].indexCount == 6u);
    CHECK(clippedMesh.ranges[1].indexOffset == 6u);
    CHECK(clippedMesh.ranges[1].indexCount == 12u);
    CHECK(clippedMesh.ranges[1].hasClip);
}

TEST_CASE("ui::VertexBuilder stretches and nine-slices textures") {
    TextureData textureData(32, 32, 0u, ColorFormat::RGBA, 4, nullptr);
    Texture texture;
    texture.setData("doriax_ui_tests_nineslice", textureData);
    REQUIRE(texture.getWidth() == 32u);
    REQUIRE(texture.getHeight() == 32u);

    //plain stretch
    PaintList plain;
    plain.addImage(Rect(0.0f, 0.0f, 100.0f, 100.0f), texture, Vector4(1.0f, 1.0f, 1.0f, 1.0f), Vector4(0.0f, 0.0f, 0.0f, 0.0f), Rect(), false);

    UIMesh plainMesh;
    VertexBuilder::build(plain, plainMesh);
    CHECK(plainMesh.vertices.size() == 4u);
    REQUIRE(plainMesh.ranges.size() == 1u);
    CHECK(!plainMesh.ranges[0].fontAtlas);

    //nine-slice: 3x3 quads
    PaintList sliced;
    sliced.addImage(Rect(0.0f, 0.0f, 100.0f, 100.0f), texture, Vector4(1.0f, 1.0f, 1.0f, 1.0f), Vector4(8.0f, 8.0f, 8.0f, 8.0f), Rect(), false);

    UIMesh slicedMesh;
    VertexBuilder::build(sliced, slicedMesh);
    CHECK(slicedMesh.vertices.size() == 36u);
    CHECK(slicedMesh.indices.size() == 54u);

    //both are textured commands: they share the texture and therefore merge
    PaintList both;
    both.addImage(Rect(0.0f, 0.0f, 100.0f, 100.0f), texture, Vector4(1.0f, 1.0f, 1.0f, 1.0f), Vector4(0.0f, 0.0f, 0.0f, 0.0f), Rect(), false);
    both.addImage(Rect(0.0f, 0.0f, 100.0f, 100.0f), texture, Vector4(1.0f, 1.0f, 1.0f, 1.0f), Vector4(8.0f, 8.0f, 8.0f, 8.0f), Rect(), false);

    UIMesh bothMesh;
    VertexBuilder::build(both, bothMesh);
    REQUIRE(bothMesh.ranges.size() == 1u);
    CHECK(bothMesh.ranges[0].indexCount == 60u);
}

TEST_CASE("ui::Label lays out glyph quads with the built-in font") {
    std::unique_ptr<VisualElement> root(new VisualElement("root"));
    Label* label = root->addChild<Label>(std::string("Hello UI"));
    label->setPosition(Vector2(5.0f, 7.0f));
    label->setFontSize(18u);
    label->setTextColor(Vector4(1.0f, 1.0f, 1.0f, 1.0f));

    UIRuntime::setRoot(std::move(root));
    UIRuntime::buildFrame();

    const UIRuntime::Stats& stats = UIRuntime::getStats();
    CHECK(stats.elements == 2u);
    //space produces no quad, so at least the 7 visible glyphs
    CHECK(stats.glyphQuads >= 7u);

    unsigned int textCommands = 0;
    for (const PaintCommand& command : UIRuntime::getPaintList().getCommands()) {
        if (command.type == PaintCommand::Type::Text)
            textCommands++;
    }
    CHECK(textCommands == 1u);

    //glyph quads start at the label origin horizontally (plus side bearing);
    //vertically the label anchors the text on its top-left box, so the whole
    //line sits between the top edge and one font ascent below it
    REQUIRE(UIRuntime::getMesh().vertices.size() >= 4u);
    const std::vector<UIVertex>& vertices = UIRuntime::getMesh().vertices;
    float minY = vertices[0].position.y;
    float maxY = minY;
    for (const UIVertex& vertex : vertices) {
        minY = vertex.position.y < minY ? vertex.position.y : minY;
        maxY = vertex.position.y > maxY ? vertex.position.y : maxY;
    }

    FontAtlas* atlas = FontAtlas::get(std::string(), 18u);
    REQUIRE(atlas != nullptr);
    const float ascent = atlas->getAscent();
    CHECK(ascent > 0.0f);

    CHECK(vertices[0].position.x >= 5.0f);
    CHECK(vertices[0].position.x <= 10.0f);
    CHECK(minY >= 7.0f);
    CHECK(minY <= 7.0f + ascent);
    CHECK(maxY > 7.0f + ascent - 4.0f); //descender-less text still crosses the baseline

    UIRuntime::clear();
}

TEST_CASE("ui::text ranges split by tint and merge when equal") {
    std::unique_ptr<VisualElement> root(new VisualElement("root"));
    Label* a = root->addChild<Label>(std::string("AA"));
    a->setFontSize(16u);
    a->setTextColor(Vector4(1.0f, 1.0f, 1.0f, 1.0f));
    Label* b = root->addChild<Label>(std::string("BB"));
    b->setFontSize(16u);
    b->setTextColor(Vector4(1.0f, 0.0f, 0.0f, 1.0f));

    UIRuntime::setRoot(std::move(root));
    UIRuntime::buildFrame(); //first frame rasterizes A/B and may grow the atlas
    UIRuntime::buildFrame(); //second frame: glyphs cached, one atlas version

    //different tints cannot share the FS uniform, so they must stay apart
    REQUIRE(UIRuntime::getMesh().ranges.size() == 2u);
    CHECK(UIRuntime::getMesh().ranges[0].fontAtlas);
    CHECK(UIRuntime::getMesh().ranges[0].color == Vector4(1.0f, 1.0f, 1.0f, 1.0f));
    CHECK(UIRuntime::getMesh().ranges[1].fontAtlas);
    CHECK(UIRuntime::getMesh().ranges[1].color == Vector4(1.0f, 0.0f, 0.0f, 1.0f));
    UIRuntime::clear();

    //same tint + texture + clip merges into a single draw
    std::unique_ptr<VisualElement> mergedRoot(new VisualElement("root"));
    Label* c = mergedRoot->addChild<Label>(std::string("CC"));
    c->setFontSize(16u);
    Label* d = mergedRoot->addChild<Label>(std::string("DD"));
    d->setFontSize(16u);

    UIRuntime::setRoot(std::move(mergedRoot));
    UIRuntime::buildFrame(); //warm the C/D glyphs
    UIRuntime::buildFrame();

    REQUIRE(UIRuntime::getMesh().ranges.size() == 1u);
    CHECK(UIRuntime::getMesh().ranges[0].fontAtlas);

    UIRuntime::clear();
}

TEST_CASE("ui::UIRuntime stats survive repeated builds") {
    std::unique_ptr<VisualElement> root(new VisualElement("root"));
    VisualElement* panel = root->addChild<VisualElement>("panel");
    panel->setSize(Vector2(100.0f, 100.0f));
    panel->setBackgroundColor(Vector4(0.0f, 0.0f, 0.0f, 1.0f));

    UIRuntime::setRoot(std::move(root));

    for (int frame = 0; frame < 3; frame++) {
        UIRuntime::buildFrame();
        CHECK(UIRuntime::getStats().elements == 2u);
        CHECK(UIRuntime::getStats().commands == 1u);
        CHECK(UIRuntime::getMesh().vertices.size() == 4u);
    }

    UIRuntime::clear();
    CHECK(!UIRuntime::hasRoot());
}
