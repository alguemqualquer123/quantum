// (c) SR VINIX
// SPDX-License-Identifier: MIT
//
// GPU-free unit tests for the quantum::slate paint list and geometry builder
// (engine/slate/core/PaintList.* and Geometry.*). No window, no GL context:
// everything asserted here is pure CPU tessellation and batching data.
//
// NOTE: if a sibling test translation unit is added to quantum-slate-tests,
// exactly one of them may define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
//Relative include because the cheap syntax-check script does not add
//libs/doctest to the include path; this resolves under CMake too.
#include "../../libs/doctest/doctest.h"

#include <string>
#include <vector>

#include "slate/core/Geometry.h"
#include "slate/core/PaintList.h"
#include "texture/Texture.h"

using namespace quantum;
using namespace quantum::slate;

namespace {

    //A texture that reports pixel dimensions without any GPU/pixel work: the
    //data-backed constructor only registers bytes in TextureDataPool.
    Texture makeSizedTexture(const std::string& id, int width, int height) {
        return Texture(id, TextureData(width, height, 0, ColorFormat::RGBA, 4, nullptr));
    }

    const Vector4 kWhite(1.0f, 1.0f, 1.0f, 1.0f);
    const Vector4 kBlack(0.0f, 0.0f, 0.0f, 1.0f);

    GlyphQuad makeGlyphQuad(float x, float y, float w, float h) {
        GlyphQuad quad;
        quad.position[0] = Vector2(x, y);
        quad.position[1] = Vector2(x + w, y);
        quad.position[2] = Vector2(x + w, y + h);
        quad.position[3] = Vector2(x, y + h);
        quad.uv[0] = Vector2(0.0f, 0.0f);
        quad.uv[1] = Vector2(0.5f, 0.0f);
        quad.uv[2] = Vector2(0.5f, 0.5f);
        quad.uv[3] = Vector2(0.0f, 0.5f);
        return quad;
    }

    //Ranges must tile the index buffer without gaps or overlaps.
    void checkRangesCoverIndices(const SlateMesh& mesh) {
        unsigned int covered = 0;
        for (const SlateDrawRange& range : mesh.ranges) {
            CHECK(range.indexOffset == covered);
            covered += range.indexCount;
        }
        CHECK(covered == (unsigned int)mesh.indices.size());
    }
}

TEST_CASE("slate::PaintList command recording") {
    PaintList list;
    list.addRect(Rect(0.0f, 0.0f, 10.0f, 10.0f), kWhite, 0.0f, Rect(), false);
    list.addBorder(Rect(0.0f, 0.0f, 10.0f, 10.0f), kWhite, 2.0f, 4.0f, Rect(), false);

    CHECK(list.getCommandCount() == 2u);
    CHECK(list.getCommands()[0].type == PaintCommand::Type::Rect);
    CHECK(list.getCommands()[1].type == PaintCommand::Type::Border);
    CHECK(list.getCommands()[1].borderWidth == 2.0f);

    std::vector<GlyphQuad> quads;
    quads.push_back(makeGlyphQuad(0.0f, 0.0f, 8.0f, 8.0f));
    quads.push_back(makeGlyphQuad(9.0f, 0.0f, 8.0f, 8.0f));
    const unsigned int offset = list.addGlyphQuads(quads.data(), 2);
    CHECK(offset == 0u);
    CHECK(list.getGlyphQuadCount() == 2u);
    CHECK(list.addGlyphQuads(quads.data(), 1) == 2u);

    list.clear();
    CHECK(list.getCommandCount() == 0u);
    //clear() must not shrink capacity (steady-state frames must not allocate)
    CHECK(list.getCommands().capacity() >= 2u);
}

TEST_CASE("slate::Geometry corner segments") {
    CHECK(Geometry::cornerSegmentsForRadius(-2.0f) == 0u);
    CHECK(Geometry::cornerSegmentsForRadius(0.0f) == 0u);
    CHECK(Geometry::cornerSegmentsForRadius(0.5f) == 0u);
    //at least 3 segments so small radii still read as a curve
    CHECK(Geometry::cornerSegmentsForRadius(2.0f) == 3u);
    //ceil(radius * 0.5) in between
    CHECK(Geometry::cornerSegmentsForRadius(5.0f) == 3u);
    CHECK(Geometry::cornerSegmentsForRadius(10.0f) == 5u);
    //capped at 12
    CHECK(Geometry::cornerSegmentsForRadius(1000.0f) == 12u);
}

TEST_CASE("slate::Geometry sharp rect vs rounded rect") {
    //sharp: one quad, no centre vertex
    PaintList sharp;
    sharp.addRect(Rect(100.0f, 50.0f, 40.0f, 20.0f), kWhite, 0.0f, Rect(), false);
    SlateMesh sharpMesh;
    Geometry::build(sharp, sharpMesh);
    CHECK(sharpMesh.vertices.size() == 4u);
    CHECK(sharpMesh.indices.size() == 6u);
    CHECK(sharpMesh.ranges.size() == 1u);
    for (const SlateVertex& v : sharpMesh.vertices)
        CHECK(v.position.z == 0.0f);

    //rounded r=5 -> 3 segments per corner: centre + 12 ring points, one fan triangle per edge
    PaintList rounded;
    rounded.addRect(Rect(100.0f, 50.0f, 40.0f, 20.0f), kWhite, 5.0f, Rect(), false);
    SlateMesh roundedMesh;
    Geometry::build(rounded, roundedMesh);
    const unsigned int segments = Geometry::cornerSegmentsForRadius(5.0f);
    CHECK(roundedMesh.vertices.size() == (size_t)(1u + 4u * segments));
    CHECK(roundedMesh.indices.size() == (size_t)(3u * 4u * segments));

    //fan triangles all start at the centre vertex: every pixel covered exactly
    //once, so translucent fills never darken seams (no square + circles stack)
    const uint32_t center = roundedMesh.indices[0];
    CHECK(center == 0u);
    for (size_t i = 0; i + 2 < roundedMesh.indices.size(); i += 3) {
        CHECK(roundedMesh.indices[i] == center);
        CHECK(roundedMesh.indices[i + 1] != roundedMesh.indices[i + 2]);
    }

    //radius bigger than half the rect is clamped, tessellation still valid
    PaintList pill;
    pill.addRect(Rect(0.0f, 0.0f, 40.0f, 10.0f), kWhite, 50.0f, Rect(), false);
    SlateMesh pillMesh;
    Geometry::build(pill, pillMesh);
    CHECK(pillMesh.indices.size() > 6u);
    checkRangesCoverIndices(pillMesh);

    //zero-size rect emits nothing
    PaintList empty;
    empty.addRect(Rect(0.0f, 0.0f, 0.0f, 10.0f), kWhite, 3.0f, Rect(), false);
    SlateMesh emptyMesh;
    Geometry::build(empty, emptyMesh);
    CHECK(emptyMesh.indices.empty());
    CHECK(emptyMesh.ranges.empty());
}

TEST_CASE("slate::Geometry border is an inner ring only") {
    PaintList list;
    list.addBorder(Rect(0.0f, 0.0f, 40.0f, 40.0f), kWhite, 4.0f, 0.0f, Rect(), false);
    SlateMesh mesh;
    Geometry::build(list, mesh);

    //sharp border: 4 quads around the frame, interior not emitted
    CHECK(mesh.vertices.size() == 16u);
    CHECK(mesh.indices.size() == 24u);

    //zero width strokes nothing
    PaintList none;
    none.addBorder(Rect(0.0f, 0.0f, 40.0f, 40.0f), kWhite, 0.0f, 0.0f, Rect(), false);
    SlateMesh noneMesh;
    Geometry::build(none, noneMesh);
    CHECK(noneMesh.indices.empty());
}

TEST_CASE("slate::Geometry image modes") {
    const Texture atlas = makeSizedTexture("slate_test_atlas", 64, 64);

    //Simple: one full-UV quad
    SlateBrush simple;
    simple.texture = atlas;
    simple.size = Vector2(64.0f, 64.0f);
    simple.drawMode = BrushDrawMode::Simple;
    PaintList simpleList;
    simpleList.addImage(Rect(0.0f, 0.0f, 64.0f, 64.0f), simple, kWhite, Rect(), false);
    SlateMesh simpleMesh;
    Geometry::build(simpleList, simpleMesh);
    CHECK(simpleMesh.vertices.size() == 4u);
    CHECK(simpleMesh.vertices[2].uv.x == doctest::Approx(1.0f));
    CHECK(simpleMesh.vertices[2].uv.y == doctest::Approx(1.0f));

    //Slice: 9 quads with the margin UVs at 16/64 and 1 - 16/64
    SlateBrush sliced;
    sliced.texture = atlas;
    sliced.size = Vector2(64.0f, 64.0f);
    sliced.slices = Insets(16.0f);
    sliced.drawMode = BrushDrawMode::Slice;
    PaintList sliceList;
    sliceList.addImage(Rect(0.0f, 0.0f, 64.0f, 64.0f), sliced, kWhite, Rect(), false);
    SlateMesh sliceMesh;
    Geometry::build(sliceList, sliceMesh);
    CHECK(sliceMesh.vertices.size() == 36u);
    CHECK(sliceMesh.indices.size() == 54u);
    //first quad is the top-left corner: its right edge samples 16/64
    CHECK(sliceMesh.vertices[1].uv.x == doctest::Approx(0.25));
    CHECK(sliceMesh.vertices[1].uv.y == doctest::Approx(0.0f));
    //its bottom edge samples 16/64 too
    CHECK(sliceMesh.vertices[3].uv.x == doctest::Approx(0.0f));
    CHECK(sliceMesh.vertices[3].uv.y == doctest::Approx(0.25));

    //Slice with margins larger than the rect: middle strips collapse to zero
    //area and are skipped, leaving only the four corner quads
    SlateBrush fat;
    fat.texture = makeSizedTexture("slate_test_fat", 128, 128);
    fat.size = Vector2(128.0f, 128.0f);
    fat.slices = Insets(30.0f);
    fat.drawMode = BrushDrawMode::Slice;
    PaintList fatList;
    fatList.addImage(Rect(0.0f, 0.0f, 40.0f, 40.0f), fat, kWhite, Rect(), false);
    SlateMesh fatMesh;
    Geometry::build(fatList, fatMesh);
    CHECK(fatMesh.vertices.size() == 16u);
    CHECK(fatMesh.indices.size() == 24u);

    //Tile: 25x10 rect at a 10x10 period -> 3 columns, the last one partial
    SlateBrush tiled;
    tiled.texture = atlas;
    tiled.size = Vector2(10.0f, 10.0f);
    tiled.drawMode = BrushDrawMode::Tile;
    PaintList tileList;
    tileList.addImage(Rect(0.0f, 0.0f, 25.0f, 10.0f), tiled, kWhite, Rect(), false);
    SlateMesh tileMesh;
    Geometry::build(tileList, tileMesh);
    CHECK(tileMesh.vertices.size() == 12u);
    CHECK(tileMesh.indices.size() == 18u);
    //third tile spans x in [20, 25] and samples u up to 0.5
    CHECK(tileMesh.vertices[8].position.x == doctest::Approx(20.0f));
    CHECK(tileMesh.vertices[9].uv.x == doctest::Approx(0.5f));
}

TEST_CASE("slate::Geometry clip culling") {
    const Rect clip(100.0f, 100.0f, 50.0f, 50.0f);

    PaintList list;
    //fully outside: culled
    list.addRect(Rect(0.0f, 0.0f, 10.0f, 10.0f), kWhite, 0.0f, clip, true);
    //fully outside even including the blur ring: culled
    list.addShadow(Rect(0.0f, 0.0f, 10.0f, 10.0f), kBlack, 2.0f, Vector2(-30.0f, 0.0f), 8.0f, clip, true);
    //overlapping: kept
    list.addRect(Rect(120.0f, 120.0f, 10.0f, 10.0f), kWhite, 0.0f, clip, true);

    SlateMesh mesh;
    Geometry::build(list, mesh);

    //one surviving rect
    CHECK(mesh.vertices.size() == 4u);
    CHECK(mesh.indices.size() == 6u);
    CHECK(mesh.ranges.size() == 1u);
    CHECK(mesh.ranges[0].hasClip);
    CHECK(mesh.ranges[0].clip.getX() == 100.0f);
    checkRangesCoverIndices(mesh);
}

TEST_CASE("slate::Geometry batching by texture and clip") {
    const Texture texA = makeSizedTexture("slate_batch_a", 32, 32);
    const Texture texB = makeSizedTexture("slate_batch_b", 32, 32);

    SlateBrush brushA;
    brushA.texture = texA;
    brushA.size = Vector2(32.0f, 32.0f);
    SlateBrush brushB;
    brushB.texture = texB;
    brushB.size = Vector2(32.0f, 32.0f);

    const Rect clip(0.0f, 0.0f, 200.0f, 200.0f);
    //overlaps both quads but is a different box: the batch has to split, and
    //neither command may be culled first
    const Rect otherClip(0.0f, 0.0f, 100.0f, 100.0f);

    //same texture + same clip -> one range
    PaintList sameList;
    sameList.addImage(Rect(0.0f, 0.0f, 16.0f, 16.0f), brushA, kWhite, clip, true);
    sameList.addImage(Rect(20.0f, 0.0f, 16.0f, 16.0f), brushA, kWhite, clip, true);
    SlateMesh sameMesh;
    Geometry::build(sameList, sameMesh);
    CHECK(sameMesh.ranges.size() == 1u);
    CHECK(sameMesh.ranges[0].indexCount == 12u);

    //different textures -> two ranges
    PaintList texList;
    texList.addImage(Rect(0.0f, 0.0f, 16.0f, 16.0f), brushA, kWhite, clip, true);
    texList.addImage(Rect(20.0f, 0.0f, 16.0f, 16.0f), brushB, kWhite, clip, true);
    SlateMesh texMesh;
    Geometry::build(texList, texMesh);
    CHECK(texMesh.ranges.size() == 2u);
    CHECK(texMesh.ranges[0].indexCount == 6u);
    CHECK(texMesh.ranges[1].indexCount == 6u);
    CHECK(texMesh.ranges[0].texture == texA);
    CHECK(texMesh.ranges[1].texture == texB);
    checkRangesCoverIndices(texMesh);

    //same texture, different clip -> two ranges
    PaintList clipList;
    clipList.addImage(Rect(0.0f, 0.0f, 16.0f, 16.0f), brushA, kWhite, clip, true);
    clipList.addImage(Rect(20.0f, 0.0f, 16.0f, 16.0f), brushA, kWhite, otherClip, true);
    SlateMesh clipMesh;
    Geometry::build(clipList, clipMesh);
    CHECK(clipMesh.ranges.size() == 2u);

    //a culled command between two batchable commands does not split the batch
    PaintList splitList;
    splitList.addImage(Rect(0.0f, 0.0f, 16.0f, 16.0f), brushA, kWhite, clip, true);
    splitList.addImage(Rect(0.0f, 0.0f, 16.0f, 16.0f), brushB, kWhite, Rect(900.0f, 900.0f, 8.0f, 8.0f), true);
    splitList.addImage(Rect(20.0f, 0.0f, 16.0f, 16.0f), brushA, kWhite, clip, true);
    SlateMesh splitMesh;
    Geometry::build(splitList, splitMesh);
    CHECK(splitMesh.ranges.size() == 1u);
    CHECK(splitMesh.ranges[0].indexCount == 12u);
    checkRangesCoverIndices(splitMesh);
}

TEST_CASE("slate::Geometry gradient") {
    //horizontal ramp on a sharp rect: corner colours only
    PaintList flat;
    flat.addGradient(Rect(0.0f, 0.0f, 40.0f, 20.0f), kBlack, kWhite, Vector2(1.0f, 0.0f), 0.0f, Rect(), false);
    SlateMesh flatMesh;
    Geometry::build(flat, flatMesh);
    CHECK(flatMesh.vertices.size() == 4u);
    CHECK(flatMesh.indices.size() == 6u);
    CHECK(flatMesh.vertices[0].color.x == doctest::Approx(0.0f));
    CHECK(flatMesh.vertices[1].color.x == doctest::Approx(1.0f));
    CHECK(flatMesh.vertices[2].color.x == doctest::Approx(1.0f));
    CHECK(flatMesh.vertices[3].color.x == doctest::Approx(0.0f));

    //vertical ramp with radius: same tessellation as Rect, centre vertex at t=0.5
    PaintList round;
    round.addGradient(Rect(0.0f, 0.0f, 40.0f, 40.0f), kBlack, kWhite, Vector2(0.0f, 1.0f), 6.0f, Rect(), false);
    SlateMesh roundMesh;
    Geometry::build(round, roundMesh);
    const unsigned int segments = Geometry::cornerSegmentsForRadius(6.0f);
    CHECK(roundMesh.vertices.size() == (size_t)(1u + 4u * segments));
    CHECK(roundMesh.indices.size() == (size_t)(3u * 4u * segments));
    //first vertex is the fan centre at (20, 20): midpoint of the ramp
    CHECK(roundMesh.vertices[0].position.x == doctest::Approx(20.0f));
    CHECK(roundMesh.vertices[0].color.x == doctest::Approx(0.5f));
    CHECK(roundMesh.vertices[0].color.w == doctest::Approx(1.0f));
    checkRangesCoverIndices(roundMesh);

    //degenerate direction falls back to the vertical axis instead of nan colours
    PaintList degen;
    degen.addGradient(Rect(0.0f, 0.0f, 40.0f, 40.0f), kBlack, kWhite, Vector2(0.0f, 0.0f), 0.0f, Rect(), false);
    SlateMesh degenMesh;
    Geometry::build(degen, degenMesh);
    //same tessellation and same corner colours as an explicit vertical ramp
    PaintList vertical;
    vertical.addGradient(Rect(0.0f, 0.0f, 40.0f, 40.0f), kBlack, kWhite, Vector2(0.0f, 1.0f), 0.0f, Rect(), false);
    SlateMesh verticalMesh;
    Geometry::build(vertical, verticalMesh);

    REQUIRE(degenMesh.vertices.size() == verticalMesh.vertices.size());
    for (size_t i = 0; i < degenMesh.vertices.size(); i++) {
        CHECK(degenMesh.vertices[i].color.x == doctest::Approx(verticalMesh.vertices[i].color.x));
        CHECK(degenMesh.vertices[i].color.x == degenMesh.vertices[i].color.x);  //never nan
    }
    CHECK(degenMesh.vertices[0].color.x == doctest::Approx(0.0f));
    CHECK(degenMesh.vertices[2].color.x == doctest::Approx(1.0f));
}

TEST_CASE("slate::Geometry shadow rings") {
    const Vector4 shadowColor(0.0f, 0.0f, 0.0f, 0.8f);

    //blur 0: just the offset core, identical to a plain rect fill
    PaintList hard;
    hard.addShadow(Rect(0.0f, 0.0f, 40.0f, 40.0f), shadowColor, 5.0f, Vector2(2.0f, 2.0f), 0.0f, Rect(), false);
    SlateMesh hardMesh;
    Geometry::build(hard, hardMesh);
    const unsigned int segments = Geometry::cornerSegmentsForRadius(5.0f);
    CHECK(hardMesh.vertices.size() == (size_t)(1u + 4u * segments));
    CHECK(hardMesh.indices.size() == (size_t)(3u * 4u * segments));
    CHECK(hardMesh.vertices[0].position.x == doctest::Approx(22.0f)); //core centre incl. offset

    //blur 8 on a radius-0 rect: core quad + 4 annuli at 4 segments (16 ring points)
    //core = 6 indices, each annulus = 16 quads * 6 = 96 indices
    PaintList soft;
    soft.addShadow(Rect(0.0f, 0.0f, 40.0f, 40.0f), shadowColor, 0.0f, Vector2(2.0f, 2.0f), 8.0f, Rect(), false);
    SlateMesh softMesh;
    Geometry::build(soft, softMesh);
    CHECK(Geometry::cornerSegmentsForRadius(8.0f) == 4u);
    CHECK(softMesh.indices.size() == 6u + 4u * 96u);
    CHECK(softMesh.vertices.size() == 4u + 4u * 64u);
    CHECK(softMesh.ranges.size() == 1u);

    //bands are non-overlapping and alpha falls geometrically: the outermost
    //band (emitted last) carries 0.8 * 0.5^4
    const size_t last = softMesh.vertices.size() - 1;
    CHECK(softMesh.vertices[last].color.w == doctest::Approx(0.8f * 0.0625f));
    //no vertex may be darker than the requested shadow alpha
    for (const SlateVertex& v : softMesh.vertices) {
        CHECK(v.color.w <= 0.8f + 0.0001f);
        CHECK(v.position.z == 0.0f);
    }
    checkRangesCoverIndices(softMesh);
}

TEST_CASE("slate::Geometry text quads") {
    std::vector<GlyphQuad> quads;
    quads.push_back(makeGlyphQuad(10.0f, 10.0f, 8.0f, 8.0f));
    quads.push_back(makeGlyphQuad(20.0f, 10.0f, 8.0f, 8.0f));

    const Vector4 tint(1.0f, 1.0f, 1.0f, 1.0f);
    const Vector4 otherTint(0.5f, 0.25f, 0.1f, 1.0f);
    //a path texture: non-empty, and no GPU or resource system needed to build it
    const Texture atlas("slate_test_atlas.png");

    PaintList list;
    list.addText(quads, 0, 2, atlas, tint, Rect(), false);
    SlateMesh mesh;
    Geometry::build(list, mesh);

    //4 vertices + 6 indices per glyph quad
    CHECK(mesh.vertices.size() == 8u);
    CHECK(mesh.indices.size() == 12u);
    CHECK(mesh.ranges.size() == 1u);
    CHECK(mesh.ranges[0].fontAtlas);
    CHECK(mesh.ranges[0].color == tint);
    //the glyph UVs index the font atlas, so the range must carry it: an empty
    //texture here means the renderer silently binds nothing and text vanishes
    CHECK(mesh.ranges[0].texture == atlas);
    //two triangles per quad, both starting from the quad's first vertex
    CHECK(mesh.indices[0] == 0u);
    CHECK(mesh.indices[3] == 0u);
    CHECK(mesh.indices[6] == 4u);
    CHECK(mesh.vertices[0].uv == Vector2(0.0f, 0.0f));
    CHECK(mesh.vertices[1].uv == Vector2(0.5f, 0.0f));

    //per-quad clip: the second glyph sits outside the clip and is dropped
    PaintList clipped;
    clipped.addText(quads, 0, 2, atlas, tint, Rect(0.0f, 0.0f, 16.0f, 20.0f), true);
    SlateMesh clippedMesh;
    Geometry::build(clipped, clippedMesh);
    CHECK(clippedMesh.indices.size() == 6u);

    //the tint travels in the Ftx uniform, so a tint change breaks the batch
    PaintList tinted;
    tinted.addText(quads, 0, 1, atlas, tint, Rect(), false);
    tinted.addText(quads, 1, 1, atlas, otherTint, Rect(), false);
    SlateMesh tintedMesh;
    Geometry::build(tinted, tintedMesh);
    CHECK(tintedMesh.ranges.size() == 2u);
}

TEST_CASE("slate::Geometry mesh reuse keeps capacity") {
    PaintList list;
    const Texture atlas = makeSizedTexture("slate_reuse_atlas", 64, 64);
    SlateBrush brush;
    brush.texture = atlas;
    brush.size = Vector2(64.0f, 64.0f);

    for (int i = 0; i < 50; i++) {
        list.addRect(Rect((float)i, 0.0f, 20.0f, 20.0f), kWhite, 4.0f, Rect(), false);
        list.addImage(Rect((float)i, 30.0f, 16.0f, 16.0f), brush, kWhite, Rect(), false);
    }

    SlateMesh mesh;
    Geometry::build(list, mesh);
    const size_t vertexCapacity = mesh.vertices.capacity();
    const size_t indexCapacity = mesh.indices.capacity();
    const size_t rangeCapacity = mesh.ranges.capacity();
    const size_t vertexCount = mesh.vertices.size();
    const size_t indexCount = mesh.indices.size();
    CHECK(vertexCapacity >= vertexCount);

    mesh.clear();
    CHECK(mesh.vertices.empty());
    CHECK(mesh.indices.empty());
    CHECK(mesh.ranges.empty());
    CHECK(mesh.vertices.capacity() == vertexCapacity);
    CHECK(mesh.indices.capacity() == indexCapacity);
    CHECK(mesh.ranges.capacity() == rangeCapacity);

    //steady state: rebuilding the same frame must not reallocate anything
    Geometry::build(list, mesh);
    CHECK(mesh.vertices.capacity() == vertexCapacity);
    CHECK(mesh.indices.capacity() == indexCapacity);
    CHECK(mesh.ranges.capacity() == rangeCapacity);
    CHECK(mesh.vertices.size() == vertexCount);
    CHECK(mesh.indices.size() == indexCount);
    checkRangesCoverIndices(mesh);

    //build appends to whatever the caller left in the mesh (caller owns clear)
    const size_t before = mesh.indices.size();
    PaintList one;
    one.addRect(Rect(0.0f, 0.0f, 10.0f, 10.0f), kWhite, 0.0f, Rect(), false);
    Geometry::build(one, mesh);
    CHECK(mesh.indices.size() == before + 6u);
    checkRangesCoverIndices(mesh);
}

TEST_CASE("slate::Geometry mixed frame") {
    const Texture atlas = makeSizedTexture("slate_mixed_atlas", 64, 64);
    SlateBrush brush;
    brush.texture = atlas;
    brush.size = Vector2(64.0f, 64.0f);
    brush.drawMode = BrushDrawMode::Slice;
    brush.slices = Insets(8.0f);

    std::vector<GlyphQuad> quads;
    quads.push_back(makeGlyphQuad(5.0f, 5.0f, 6.0f, 6.0f));

    PaintList list;
    list.addShadow(Rect(10.0f, 10.0f, 100.0f, 40.0f), Vector4(0.0f, 0.0f, 0.0f, 0.5f), 6.0f, Vector2(1.0f, 1.0f), 4.0f, Rect(), false);
    list.addRect(Rect(10.0f, 10.0f, 100.0f, 40.0f), Vector4(0.2f, 0.3f, 0.4f, 1.0f), 6.0f, Rect(), false);
    list.addBorder(Rect(10.0f, 10.0f, 100.0f, 40.0f), kWhite, 2.0f, 6.0f, Rect(), false);
    list.addGradient(Rect(10.0f, 10.0f, 100.0f, 40.0f), kBlack, kWhite, Vector2(1.0f, 0.0f), 6.0f, Rect(), false);
    list.addImage(Rect(120.0f, 10.0f, 48.0f, 48.0f), brush, kWhite, Rect(), false);
    list.addText(quads, 0, 1, Texture(), kWhite, Rect(), false);

    SlateMesh mesh;
    Geometry::build(list, mesh);

    CHECK(!mesh.vertices.empty());
    CHECK(mesh.indices.size() % 3u == 0u);
    checkRangesCoverIndices(mesh);

    //every index must address a real vertex
    for (uint32_t index : mesh.indices)
        CHECK(index < (uint32_t)mesh.vertices.size());

    //text is its own fontAtlas batch; the solid commands before it share the
    //no-texture batch even across types
    CHECK(mesh.ranges.back().fontAtlas);
    bool anySolid = false;
    for (size_t i = 0; i + 1 < mesh.ranges.size(); i++) {
        CHECK(!mesh.ranges[i].fontAtlas);
        anySolid = true;
    }
    CHECK(anySolid);
}
