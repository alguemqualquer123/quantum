// (c) Eduardo Doria
// SPDX-License-Identifier: MIT
//
// doriax-slate-demo: gallery executable for the retained-mode Slate module
// (engine/slate). It mounts a single CanvasPanel tree exercising every widget
// and container that exists today: Box (V/H), Overlay, Border, GridPanel,
// SizeBox, ScrollBox (clip + content offset), CanvasPanel anchors (HUD corners
// that follow the canvas size), Label (title + wrapped long text), Image
// (brush over a tracked asset), Button in its four forced visual states and
// ProgressBar at 0/25/72/100. Runs windowed by default; --headless builds the
// same tree and prints Slate::getStats() plus the resolved HUD rects for
// three canvas sizes, without opening a window or touching the GPU.

// MinGW with -std=c++17 defines __STRICT_ANSI__, which hides M_PI from
// <math.h>; the engine headers only re-enable it through _USE_MATH_DEFINES
// (an MSVC-only switch), so the syntax-check pass needs _GNU_SOURCE here.
#ifndef _GNU_SOURCE
    #define _GNU_SOURCE 1
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include "Engine.h"
#include "Scene.h"

#include "slate/Slate.h"
#include "slate/core/PaintList.h"
#include "slate/core/SlateTypes.h"
#include "slate/core/StyleSet.h"
#include "slate/core/Widget.h"
#include "slate/layout/Containers.h"
#include "slate/widgets/Button.h"
#include "slate/widgets/Image.h"
#include "slate/widgets/Label.h"
#include "slate/widgets/ProgressBar.h"

#if defined(_WIN32)
    #include "DoriaxWin.h"
    #include "WindowWin.h"
#elif defined(__APPLE__)
    #include "DoriaxMac.h"
    #include "WindowMac.h"
#else
    #include "DoriaxLinux.h"
    #include "WindowLinux.h"
#endif

using namespace doriax;
using namespace doriax::slate;

namespace {

    Scene demoScene;

    //windowed --frames counter (consumed by the onDraw auto-close)
    int autoCloseFrames = 0;

    const Vector4 kTextDim(0.72f, 0.78f, 0.86f, 1.0f);
    const Vector4 kTextFaint(0.55f, 0.62f, 0.72f, 1.0f);

    // Gradient + soft shadow have a PaintList command but neither a widget nor a
    // WidgetStyle field yet (StyleSet carries no gradient/shadow tokens), so the
    // demo is the one emitting the commands through a local Widget subclass.
    // Rounded corners and the border come from the same emission so the whole
    // "card" look is covered in a single paint.
    class GradientCard : public Widget {
    public:
        explicit GradientCard(std::string id) : Widget(std::move(id)) {}

        GradientCard& setGradient(const Vector4& from, const Vector4& to) {
            gradientFrom = from;
            gradientTo = to;
            return *this;
        }
        GradientCard& setShadow(const Vector4& color, Vector2 offset, float blur) {
            shadowColor = color;
            shadowOffset = offset;
            shadowBlur = blur;
            return *this;
        }
        GradientCard& setCornerRadius(float radius) {
            radiusValue = radius;
            return *this;
        }
        GradientCard& setBorder(float width, const Vector4& color) {
            borderWidthValue = width;
            borderColorValue = color;
            return *this;
        }

        void paint(PaintContext& context) override {
            Widget::paint(context);   //style background/border when themed
            if (context.list == nullptr) {
                return;
            }
            const Rect& rect = getLayoutRect();
            context.list->addShadow(rect, shadowColor, radiusValue, shadowOffset, shadowBlur,
                                    context.clip, context.hasClip);
            context.list->addGradient(rect, gradientFrom, gradientTo, Vector2(0.0f, 1.0f),
                                      radiusValue, context.clip, context.hasClip);
            context.list->addBorder(rect, borderColorValue, borderWidthValue, radiusValue,
                                    context.clip, context.hasClip);
        }

    private:
        Vector4 gradientFrom = Vector4(0.20f, 0.35f, 0.60f, 1.0f);
        Vector4 gradientTo = Vector4(0.10f, 0.14f, 0.24f, 1.0f);
        Vector4 shadowColor = Vector4(0.0f, 0.0f, 0.0f, 0.55f);
        Vector2 shadowOffset = Vector2(4.0f, 8.0f);
        float shadowBlur = 14.0f;
        float radiusValue = 18.0f;
        float borderWidthValue = 2.0f;
        Vector4 borderColorValue = Vector4(0.45f, 0.70f, 1.00f, 1.0f);
    };

    //All brushes/styles the gallery needs, registered on the built-in theme
    //BEFORE the tree is mounted (getTheme() creates the dark theme lazily;
    //"Menu.Button" is its own and is inherited by "Demo.Button").
    void registerDemoTheme() {
        StyleSet& theme = Slate::getTheme();

        theme.registerColor("Demo.Accent", Vector4(0.30f, 0.55f, 0.85f, 1.0f));
        theme.registerColor("Demo.AccentTeal", Vector4(0.25f, 0.65f, 0.60f, 1.0f));

        //Image brush over a repo-tracked asset; the texture is loaded lazily by
        //StyleSet::getBrush, so a headless run (no view loaded) paints an empty
        //rect. Paths are relative to System::getAssetPath(), which is "assets".
        SlateBrush sky;
        sky.name = "Demo.Sky";
        sky.texture = Texture("Character/Texture/Banana Man Albedo.png");
        sky.size = Vector2(256.0f, 128.0f);
        sky.drawMode = BrushDrawMode::Simple;
        theme.registerBrush(sky.name, sky);

        //nine-sliced brush: proves Slice mode + the margins field
        SlateBrush slice;
        slice.name = "Demo.Slice";
        slice.texture = Texture("Character/Texture/Banana Man Albedo.png");
        slice.size = Vector2(256.0f, 128.0f);
        slice.slices = Insets(16.0f);
        slice.drawMode = BrushDrawMode::Slice;
        theme.registerBrush(slice.name, slice);

        NamedStyle& title = theme.defineStyle("Demo.Title");
        title.base().textColor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        title.base().fontSize = 28;

        NamedStyle& body = theme.defineStyle("Demo.Body");
        body.base().textColor = kTextDim;
        body.base().fontSize = 14;
        body.base().wrap = true;

        NamedStyle& overlayBig = theme.defineStyle("Demo.OverlayBig");
        overlayBig.base().textColor = Vector4(1.0f, 0.85f, 0.45f, 1.0f);
        overlayBig.base().fontSize = 30;

        NamedStyle& card = theme.defineStyle("Demo.Card");
        card.setParent("Demo.Panel");
        card.base().borderColor = Vector4(0.90f, 0.65f, 0.30f, 1.0f);
        card.base().borderWidth = 3.0f;
        card.base().borderRadius = 22.0f;

        NamedStyle& panel = theme.defineStyle("Demo.Panel");
        panel.base().backgroundColor = Vector4(0.10f, 0.12f, 0.16f, 0.95f);
        panel.base().borderColor = Vector4(0.30f, 0.55f, 0.85f, 1.0f);
        panel.base().borderWidth = 2.0f;
        panel.base().borderRadius = 14.0f;
        panel.base().padding = Insets(14.0f);

        //Four visual states resolved by the same named style; the hovered /
        //pressed / disabled colours only exist here, never per widget.
        NamedStyle& button = theme.defineStyle("Demo.Button");
        button.setParent("Menu.Button");
        button.base().borderRadius = 8.0f;
        button.base().padding = Insets(8.0f, 6.0f, 8.0f, 6.0f);
        button.base().minSize = Vector2(0.0f, 34.0f);
        button.base().backgroundColor = Vector4(0.20f, 0.34f, 0.52f, 1.0f);
        button.base().textColor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        button.forState(WidgetVisualState::Hovered).backgroundColor = Vector4(0.30f, 0.50f, 0.74f, 1.0f);
        button.forState(WidgetVisualState::Pressed).backgroundColor = Vector4(0.12f, 0.22f, 0.36f, 1.0f);
        button.forState(WidgetVisualState::Disabled).backgroundColor = Vector4(0.14f, 0.15f, 0.17f, 1.0f);
        button.forState(WidgetVisualState::Disabled).textColor = Vector4(0.45f, 0.48f, 0.52f, 1.0f);

        NamedStyle& hud = theme.defineStyle("Demo.HudCorner");
        hud.base().backgroundColor = Vector4(0.05f, 0.07f, 0.10f, 0.80f);
        hud.base().borderColor = Vector4(0.25f, 0.65f, 0.60f, 1.0f);
        hud.base().borderWidth = 1.0f;
        hud.base().borderRadius = 6.0f;
        hud.base().padding = Insets(6.0f, 4.0f, 6.0f, 4.0f);
        hud.base().textColor = Vector4(0.80f, 0.92f, 1.0f, 1.0f);
        hud.base().fontSize = 13;

        NamedStyle& cell = theme.defineStyle("Demo.Cell");
        cell.base().backgroundColor = Vector4(0.14f, 0.22f, 0.34f, 1.0f);
        cell.base().borderColor = Vector4(0.30f, 0.55f, 0.85f, 0.60f);
        cell.base().borderWidth = 1.0f;
        cell.base().borderRadius = 4.0f;
        cell.base().padding = Insets(6.0f);
        cell.base().textColor = kTextDim;
        cell.base().fontSize = 12;

        //Track/fill brush names are set on the style: whichever surface the
        //ProgressBar implementation ends up reading, the gallery provides.
        NamedStyle& progress = theme.defineStyle("Demo.Progress");
        progress.base().backgroundColor = Vector4(0.05f, 0.06f, 0.08f, 1.0f);
        progress.base().borderColor = Vector4(0.30f, 0.55f, 0.85f, 1.0f);
        progress.base().borderWidth = 1.0f;
        progress.base().borderRadius = 6.0f;
        progress.base().minSize = Vector2(0.0f, 18.0f);
        progress.base().backgroundBrush = "Demo.ProgressTrack";
        progress.base().borderBrush = "Demo.ProgressFill";

        NamedStyle& scroll = theme.defineStyle("Demo.Scroll");
        scroll.base().backgroundColor = Vector4(0.05f, 0.06f, 0.08f, 1.0f);
        scroll.base().borderColor = Vector4(0.25f, 0.65f, 0.60f, 1.0f);
        scroll.base().borderWidth = 1.0f;
        scroll.base().borderRadius = 8.0f;
    }

    //UMG-style offsets helper: with point anchors (min == max) the rect is
    //left/top plus a negative right/bottom that encodes size + inset (this is
    //exactly what resolveAnchorRect implements).
    Widget& place(Widget& w, float minX, float minY,
                  float x, float y, float width, float height) {
        w.setAnchors(minX, minY, minX, minY);
        w.setOffsets(x, y, -(x + width), -(y + height));
        return w;
    }

    //The whole gallery, hung off one 1280x720 canvas.
    void buildDemoTree() {
        std::unique_ptr<CanvasPanel> root(new CanvasPanel("canvas"));

        // ---- main layout column: VBox of title / wrapped text / image /
        // ---- four button states / four progress bars
        Box* panel = root->addChild<Box>(Orientation::Vertical, "main-panel");
        panel->setStyleName("Demo.Panel");
        panel->setGap(10.0f);
        place(*panel, 0.0f, 0.0f, 24.0f, 24.0f, 480.0f, 644.0f);

        Label* title = panel->addChild<Label>("title");
        title->setText("Doriax Slate - gallery");
        title->setStyleName("Demo.Title");

        Label* body = panel->addChild<Label>("body");
        body->setText("Retained-mode UI in the style of Slate and UMG: styles resolve per "
                      "visual state, layout runs through the slot (alignment, growth, basis) "
                      "while the canvas pins every direct child with anchors and pixel "
                      "offsets, and the paint walk emits GPU-free commands.");
        body->setStyleName("Demo.Body");
        body->setWrap(true);

        Image* sky = panel->addChild<Image>("sky-image");
        sky->setBrush("Demo.Sky");
        sky->setHAlign(HAlign::Center);
        sky->setVAlign(VAlign::Top);

        //sliced version of the same brush, nine-slice margins from the theme
        Image* sliced = panel->addChild<Image>("slice-image");
        sliced->setBrush("Demo.Slice");
        sliced->setHAlign(HAlign::Center);
        sliced->setVAlign(VAlign::Top);

        Box* buttons = panel->addChild<Box>(Orientation::Horizontal, "buttons");
        buttons->setGap(8.0f);
        struct Variant {
            const char* id;
            const char* text;
            WidgetVisualState state;
        };
        const Variant variants[4] = {
            { "btn-normal",  "Normal",  WidgetVisualState::Normal },
            { "btn-hovered", "Hovered", WidgetVisualState::Hovered },
            { "btn-pressed", "Pressed", WidgetVisualState::Pressed },
            { "btn-disabled", "Disabled", WidgetVisualState::Disabled },
        };
        for (const Variant& variant : variants) {
            Button* button = buttons->addChild<Button>(variant.id);
            button->setText(variant.text);
            button->setStyleName("Demo.Button");
            button->setVisualState(variant.state);
            button->setGrow(1.0f).setBasis(0.0f);
            if (variant.state == WidgetVisualState::Disabled) {
                //the disabled look must win over the forced state (header rule)
                button->setEnabled(false);
            }
        }

        Box* bars = panel->addChild<Box>(Orientation::Horizontal, "progress");
        bars->setGap(8.0f);
        const float values[4] = { 0.0f, 25.0f, 72.0f, 100.0f };
        for (int i = 0; i < 4; i++) {
            ProgressBar* bar = bars->addChild<ProgressBar>("bar-" + std::to_string(i));
            bar->setRange(0.0f, 100.0f);
            bar->setValue(values[i]);
            bar->setTextFormat("{0}%");
            bar->setStyleName("Demo.Progress");
            bar->setGrow(1.0f).setBasis(0.0f);
        }

        // ---- horizontal strip: Overlay (two stacked labels) + Border around a
        // ---- label + the gradient/shadow card
        Box* strip = root->addChild<Box>(Orientation::Horizontal, "strip");
        strip->setGap(12.0f);
        strip->setAlignItems(Align::Stretch);
        place(*strip, 0.0f, 0.0f, 528.0f, 24.0f, 728.0f, 150.0f);

        Overlay* overlay = strip->addChild<Overlay>("overlay");
        overlay->setStyleName("Demo.Panel");
        overlay->setGrow(1.0f).setBasis(0.0f);
        Label* overlayBack = overlay->addChild<Label>("overlay-back");
        overlayBack->setText("OVERLAY");
        overlayBack->setStyleName("Demo.OverlayBig");
        overlayBack->setHAlign(HAlign::Center).setVAlign(VAlign::Center);
        Label* overlayFront = overlay->addChild<Label>("overlay-front");
        overlayFront->setText("two children, one rect");
        overlayFront->setStyleName("Demo.Body");
        overlayFront->setHAlign(HAlign::Right).setVAlign(VAlign::Bottom);

        Border* border = strip->addChild<Border>("border-card");
        border->setStyleName("Demo.Card");
        border->setGrow(1.0f).setBasis(0.0f);
        Label* borderLabel = border->addChild<Label>("border-label");
        borderLabel->setText("Border 3px / radius 22");
        borderLabel->setStyleName("Demo.Body");

        GradientCard* gradientCard = strip->addChild<GradientCard>("gradient-card");
        gradientCard->setGradient(Vector4(0.24f, 0.40f, 0.66f, 1.0f),
                                  Vector4(0.10f, 0.13f, 0.22f, 1.0f));
        gradientCard->setShadow(Vector4(0.0f, 0.0f, 0.0f, 0.55f), Vector2(5.0f, 9.0f), 14.0f);
        gradientCard->setCornerRadius(18.0f);
        gradientCard->setBorder(2.0f, Vector4(0.45f, 0.70f, 1.00f, 1.0f));
        gradientCard->setGrow(1.0f).setBasis(0.0f).setMinSize(Vector2(220.0f, 120.0f));

        // ---- 3x3 grid with row/column weights (0 = size to content)
        GridPanel* grid = root->addChild<GridPanel>("grid");
        grid->setStyleName("Demo.Panel");
        place(*grid, 0.0f, 0.0f, 528.0f, 194.0f, 728.0f, 224.0f);
        grid->setRows(3).setColumns(3).setGap(6.0f, 6.0f);
        grid->setColumnWeight(0, 1.0f);
        grid->setColumnWeight(1, 2.0f);
        grid->setColumnWeight(2, 1.0f);
        grid->setRowWeight(2, 1.0f);
        for (unsigned int row = 0; row < 3; row++) {
            for (unsigned int column = 0; column < 3; column++) {
                Label* cellLabel = grid->addAt<Label>(row, column,
                                                      "cell-" + std::to_string(row * 3 + column));
                cellLabel->setText("r" + std::to_string(row) + " c" + std::to_string(column));
                cellLabel->setStyleName("Demo.Cell");
                cellLabel->setTextAlign(TextAlign::Center);
            }
        }

        // ---- SizeBox (fixed 180x200) + ScrollBox of 40 labels, proving clip
        // ---- and the content offset
        Box* scrollRow = root->addChild<Box>(Orientation::Horizontal, "scroll-row");
        scrollRow->setGap(12.0f);
        place(*scrollRow, 0.0f, 0.0f, 528.0f, 438.0f, 728.0f, 230.0f);

        SizeBox* sizeBox = scrollRow->addChild<SizeBox>("sizebox");
        sizeBox->setStyleName("Demo.Panel");
        sizeBox->setWidth(180.0f).setHeight(200.0f);
        sizeBox->setVAlign(VAlign::Top);
        Label* sizeBoxLabel = sizeBox->addChild<Label>("sizebox-label");
        sizeBoxLabel->setText("SizeBox pinned to 180x200");
        sizeBoxLabel->setStyleName("Demo.Body");

        ScrollBox* scroll = scrollRow->addChild<ScrollBox>("scroll");
        scroll->setStyleName("Demo.Scroll");
        scroll->setOrientation(Orientation::Vertical);
        scroll->setGap(4.0f);
        scroll->setGrow(1.0f).setBasis(0.0f);
        for (int i = 0; i < 40; i++) {
            Label* item = scroll->addChild<Label>("scroll-item-" + std::to_string(i));
            item->setText("scroll line " + std::to_string(i + 1));
            item->setStyleName("Demo.Body");
        }
        //the input phase (wheel/drag) is not wired yet: offset set by hand
        scroll->setContentOffset(Vector2(0.0f, 64.0f));

        // ---- HUD: five canvas-anchored labels (4 corners + center). These are
        // ---- the widgets whose rects the --headless run re-measures per size.
        // ---- Offsets follow resolveAnchorRect: each one is a signed distance
        // ---- from its anchored edge, so edges glued to the max side take
        // ---- negative left/top and positive right/bottom insets.
        struct Corner {
            const char* id;
            const char* text;
            float ax;
            float ay;
            float left, top, right, bottom;
        };
        const Corner corners[5] = {
            { "hud-top-left",     "HUD top-left",     0.0f, 0.0f,  12.0f,  12.0f, -162.0f, -40.0f },
            { "hud-top-right",    "HUD top-right",    1.0f, 0.0f, -162.0f,  12.0f,  12.0f, -40.0f },
            { "hud-bottom-left",  "HUD bottom-left",  0.0f, 1.0f,  12.0f, -40.0f, -162.0f,  12.0f },
            { "hud-bottom-right", "HUD bottom-right", 1.0f, 1.0f, -162.0f, -40.0f,  12.0f,  12.0f },
            { "hud-center",       "HUD center",       0.5f, 0.5f, -80.0f, -14.0f, -80.0f, -14.0f },
        };
        for (const Corner& corner : corners) {
            Label* hud = root->addChild<Label>(corner.id);
            hud->setText(corner.text);
            hud->setStyleName("Demo.HudCorner");
            hud->setZOrder(10);
            hud->setPivot(0.5f, 0.5f);
            hud->setAnchors(corner.ax, corner.ay, corner.ax, corner.ay);
            hud->setOffsets(corner.left, corner.top, corner.right, corner.bottom);
        }

        Slate::setRoot(std::move(root));
    }

    void printStats(const Slate::Stats& stats, int frames, double elapsedMs) {
        std::printf("  widgets     : %u\n", stats.widgets);
        std::printf("  commands    : %u\n", stats.commands);
        std::printf("  glyph quads : %u\n", stats.glyphQuads);
        std::printf("  vertices    : %u\n", stats.vertices);
        std::printf("  indices     : %u\n", stats.indices);
        std::printf("  draw ranges : %u\n", stats.ranges);
        std::printf("  layout visits: %u\n", stats.layoutVisits);
        std::printf("  frames      : %d\n", frames);
        std::printf("  build total : %.3f ms (%.4f ms/frame)\n",
                    elapsedMs, frames > 0 ? elapsedMs / (double)frames : 0.0);
    }

    void bootScene() {
        registerDemoTheme();
        buildDemoTree();

        demoScene.setBackgroundColor(0.07f, 0.08f, 0.10f);

        Engine::setScalingMode(Scaling::FITWIDTH);
        Engine::setCanvasSize(1280, 720);
        Engine::setScene(&demoScene);

        //The engine does not drive Slate yet, so the demo builds and submits the
        //frame itself. onPostDraw is the slot that works: onDraw runs before the
        //scenes draw and the composite pass begins, so UI painted there is
        //covered by the scene draw.
        Engine::onPostDraw.add("slate_demo_frame", []() {
            Slate::buildFrame();
            Slate::render();
        });
    }

    // --headless proof: the bottom-right/center HUD rects for three canvas
    // sizes. arrange() is the public CanvasPanel API ("the canvas rect is the
    // reference"), so this runs with no engine loop, window or GL.
    void printAnchoredRects() {
        const int sizes[3][2] = { { 1280, 720 }, { 800, 600 }, { 2560, 1440 } };
        CanvasPanel* canvas = static_cast<CanvasPanel*>(Slate::getRoot());
        if (canvas == nullptr) {
            std::printf("  (no root installed)\n");
            return;
        }
        Widget* bottomRight = canvas->findById("hud-bottom-right");
        Widget* center = canvas->findById("hud-center");
        for (const int (&size)[2] : sizes) {
            const Rect reference(0.0f, 0.0f, (float)size[0], (float)size[1]);
            canvas->markDirty(WidgetDirty::All);
            canvas->arrange(reference);
            std::printf("  canvas %dx%d:", size[0], size[1]);
            if (bottomRight != nullptr) {
                const Rect& r = bottomRight->getLayoutRect();
                std::printf(" hud-bottom-right=(%.0f,%.0f %.0fx%.0f)",
                            r.getX(), r.getY(), r.getWidth(), r.getHeight());
            }
            if (center != nullptr) {
                const Rect& r = center->getLayoutRect();
                std::printf(" hud-center=(%.0f,%.0f %.0fx%.0f)",
                            r.getX(), r.getY(), r.getWidth(), r.getHeight());
            }
            std::printf("\n");
        }
    }
}

// Registered like the runner/exported games (Engine.h:135-144); the engine
// fires it inside Engine::systemInit.
DORIAX_INIT void init() {
    bootScene();
}

int main(int argc, char** argv) {
    bool headless = false;
    int frames = 0; //0 = run until the window is closed

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--headless") == 0) {
            headless = true;
        }else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames = std::atoi(argv[++i]);
        }
    }

    if (headless) {
        //Unbuffered: this run is a smoke test, so the lines before an abort have
        //to reach the log.
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        registerDemoTheme();
        buildDemoTree();
        if (frames < 1)
            frames = 1;

        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        for (int i = 0; i < frames; i++) {
            Slate::buildFrame();
        }
        const double elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

        std::printf("doriax-slate-demo (headless)\n");
        printStats(Slate::getStats(), frames, elapsedMs);
        printAnchoredRects();

        Slate::clear();
        return 0;
    }

    //windowed: --frames N draws N frames and closes the window itself, so the
    //same binary doubles as a smoke test next to the visual check
    if (frames > 0) {
        autoCloseFrames = frames;
        Engine::onDraw.add("slate_demo_auto_close", []() {
            if (autoCloseFrames > 0 && --autoCloseFrames == 0) {
#if defined(_WIN32)
                WindowWin::quit();
#elif defined(__APPLE__)
                WindowMac::quit();
#else
                WindowLinux::quit();
#endif
            }
        });
    }

    // The platform init owns the frame loop and shuts the engine down on
    // window close (DoriaxWin::init, engine/platform/win/DoriaxWin.cpp).
#if defined(_WIN32)
    const int result = DoriaxWin::init(argc, argv);
#elif defined(__APPLE__)
    const int result = DoriaxMac::init(argc, argv);
#else
    const int result = DoriaxLinux::init(argc, argv);
#endif

    const Slate::Stats& stats = Slate::getStats();
    std::printf("doriax-slate-demo finished (code %d): %u widgets, %u commands, %u ranges\n",
                result, stats.widgets, stats.commands, stats.ranges);
    Slate::clear();
    return result;
}
