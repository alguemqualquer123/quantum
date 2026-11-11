// (c) SR VINIX
// SPDX-License-Identifier: MIT
//
// quantum-ui-demo: development executable for the retained-mode UI module
// (engine/core/ui). Fase 2a: the demo tree is laid out by the flexbox engine
// (Yoga) through the UMG-style panels, with only the top cards pinned to
// absolute positions. Runs windowed by default; --headless builds the same
// tree and prints mesh statistics without opening a window or touching the GPU.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "Engine.h"
#include "Scene.h"
#include "ui/UIRuntime.h"
#include "ui/controls/Label.h"
#include "ui/layout/Panels.h"

#if defined(_WIN32)
    #include "QuantumWin.h"
    #include "WindowWin.h"
#elif defined(__APPLE__)
    #include "QuantumMac.h"
    #include "WindowMac.h"
#else
    #include "QuantumLinux.h"
    #include "WindowLinux.h"
#endif

using namespace quantum;
using namespace quantum::ui;

namespace {

    Scene demoScene;

    //windowed --frames counter (consumed by the onDraw auto-close)
    int autoCloseFrames = 0;

    const Vector4 kPanelBackground(0.10f, 0.12f, 0.16f, 0.95f);
    const Vector4 kPanelBorder(0.30f, 0.55f, 0.85f, 1.0f);
    const Vector4 kPanelBorderTeal(0.25f, 0.65f, 0.60f, 1.0f);
    const Vector4 kInsetBackground(0.05f, 0.06f, 0.08f, 1.0f);
    const Vector4 kTextDim(0.65f, 0.72f, 0.82f, 1.0f);
    const Vector4 kTextFaint(0.50f, 0.57f, 0.68f, 1.0f);

    void styleCard(VisualElement* card, float radius, const Vector4& background, const Vector4& border, float borderWidth) {
        card->setBackgroundColor(background);
        card->setBorderColor(border);
        card->setBorderWidth(borderWidth);
        card->setBorderRadius(radius);
    }

    Label* addCaption(VisualElement* parent, const char* id, const char* text) {
        Label* label = parent->addChild<Label>(id);
        label->setText(text);
        label->setFontSize(12);
        label->setTextColor(kTextFaint);
        return label;
    }

    // Fase 2a demo tree: the cards are pinned with absolute positions (the
    // legacy setPosition/setSize sugar) while everything inside them is placed
    // by the flexbox layout - HBox/VBox rows, a wrapping strip, a uniform grid
    // and a clipped scroll viewport.
    void buildDemoTree() {
        std::unique_ptr<VisualElement> root(new VisualElement("canvas"));
        root->setSize(Vector2(1000.0f, 480.0f)); //matches the demo canvas below

        //main card: a VBox with padding and gap; the children below are never
        //positioned by hand
        VBox* main = root->addChild<VBox>("panel-main");
        main->setPosition(Vector2(24.0f, 24.0f));
        main->setSize(Vector2(460.0f, 340.0f));
        main->setPadding(16.0f);
        main->setGap(10.0f);
        main->setOverflow(Overflow::Hidden);
        styleCard(main, 14.0f, kPanelBackground, kPanelBorder, 2.0f);

        Label* title = main->addChild<Label>("title");
        title->setText("Quantum UI - Fase 2a");
        title->setFontSize(24);
        title->setTextColor(Vector4(1.0f, 1.0f, 1.0f, 1.0f));

        Label* subtitle = main->addChild<Label>("subtitle");
        subtitle->setText("layout flexbox (Yoga) e paineis UMG");
        subtitle->setFontSize(13);
        subtitle->setTextColor(kTextDim);

        //radius ramp on an HBox row
        HBox* swatches = main->addChild<HBox>("swatches");
        swatches->setWidth(StyleLength::percent(100.0f));
        swatches->setGap(8.0f);
        for (int column = 0; column < 4; column++) {
            VisualElement* swatch = swatches->addChild<VisualElement>("swatch");
            swatch->setSize(Vector2(36.0f, 36.0f));
            swatch->setBackgroundColor(Vector4(0.20f + 0.18f * column, 0.45f, 0.85f - 0.15f * column, 1.0f));
            swatch->setBorderRadius(4.0f + column * 4.0f);
        }

        addCaption(main, "chips-caption", "WrapBox: quebra automatica de linha");
        WrapBox* chips = main->addChild<WrapBox>("chips");
        chips->setWidth(StyleLength::percent(100.0f));
        chips->setGap(6.0f);
        for (int i = 0; i < 9; i++) {
            VisualElement* chip = chips->addChild<VisualElement>("chip");
            chip->setSize(Vector2(60.0f, 22.0f));
            chip->setBackgroundColor(i % 2 == 0 ? Vector4(0.22f, 0.38f, 0.58f, 1.0f)
                                                : Vector4(0.16f, 0.26f, 0.40f, 1.0f));
            chip->setBorderRadius(6.0f);
        }

        addCaption(main, "grid-caption", "GridPanel: 4 colunas");
        GridPanel* grid = main->addChild<GridPanel>("grid");
        grid->setColumns(4);
        grid->setWidth(StyleLength::percent(100.0f));
        for (int i = 0; i < 8; i++) {
            VisualElement* cell = grid->addChild<VisualElement>("cell");
            cell->setHeight(StyleLength::point(26.0f));
            cell->setBackgroundColor(i % 2 == 0 ? Vector4(0.30f, 0.55f, 0.85f, 1.0f)
                                                : Vector4(0.14f, 0.30f, 0.52f, 1.0f));
        }

        //scroll card: clipped viewport with the content shifted by the offset
        VBox* scrollCard = root->addChild<VBox>("panel-scroll");
        scrollCard->setPosition(Vector2(508.0f, 24.0f));
        scrollCard->setSize(Vector2(340.0f, 300.0f));
        scrollCard->setPadding(14.0f);
        scrollCard->setGap(8.0f);
        scrollCard->setOverflow(Overflow::Hidden);
        styleCard(scrollCard, 14.0f, kPanelBackground, kPanelBorderTeal, 2.0f);

        addCaption(scrollCard, "scroll-caption", "ScrollBox: clip + offset (0, 40)");

        ScrollBox* scroll = scrollCard->addChild<ScrollBox>("scroll");
        scroll->setWidth(StyleLength::percent(100.0f));
        scroll->setHeight(StyleLength::point(170.0f));
        scroll->setBackgroundColor(kInsetBackground);
        scroll->setBorderRadius(8.0f);

        VBox* content = scroll->addChild<VBox>("scroll-content");
        for (int i = 0; i < 10; i++) {
            HBox* row = content->addChild<HBox>("row");
            row->setWidth(StyleLength::percent(100.0f));
            row->setHeight(StyleLength::point(28.0f));
            row->setPadding(Edge::Left, 10.0f);
            row->setAlignItems(AlignItems::Center);
            row->setBackgroundColor(i % 2 == 0 ? Vector4(0.10f, 0.13f, 0.19f, 1.0f)
                                               : Vector4(0.08f, 0.10f, 0.14f, 1.0f));

            Label* rowLabel = row->addChild<Label>("row-label");
            rowLabel->setText("linha " + std::to_string(i + 1));
            rowLabel->setFontSize(12);
            rowLabel->setTextColor(kTextDim);
        }
        scroll->setContentOffset(Vector2(0.0f, 40.0f));

        addCaption(scrollCard, "opacity-caption", "opacidade herdada 55%");
        HBox* opacityRow = scrollCard->addChild<HBox>("opacity-row");
        opacityRow->setWidth(StyleLength::percent(100.0f));
        opacityRow->setHeight(StyleLength::point(40.0f));
        opacityRow->setGap(8.0f);
        opacityRow->setOpacity(0.55f);
        for (int i = 0; i < 4; i++) {
            VisualElement* box = opacityRow->addChild<VisualElement>("opacity-box");
            box->setSize(Vector2(32.0f, 32.0f));
            box->setBackgroundColor(Vector4(0.85f, 0.40f + 0.12f * i, 0.25f + 0.10f * i, 1.0f));
            box->setBorderRadius(8.0f);
        }

        //border/corner card: content centered by justifyContent/alignItems
        VisualElement* borderCard = root->addChild<VisualElement>("panel-border");
        borderCard->setPosition(Vector2(508.0f, 340.0f));
        borderCard->setSize(Vector2(340.0f, 116.0f));
        borderCard->setJustifyContent(JustifyContent::Center);
        borderCard->setAlignItems(AlignItems::Center);
        styleCard(borderCard, 26.0f, Vector4(0.16f, 0.13f, 0.20f, 1.0f), Vector4(0.90f, 0.65f, 0.30f, 1.0f), 4.0f);

        Label* borderLabel = borderCard->addChild<Label>("border-label");
        borderLabel->setText("borda 4px, raio 26px");
        borderLabel->setFontSize(16);
        borderLabel->setTextColor(Vector4(1.0f, 0.92f, 0.80f, 1.0f));

        UIRuntime::setRoot(std::move(root));
    }

    void bootScene() {
        buildDemoTree();

        demoScene.setBackgroundColor(0.07f, 0.08f, 0.10f);

        Engine::setScalingMode(Scaling::FITWIDTH);
        Engine::setCanvasSize(1000, 480);
        Engine::setScene(&demoScene);
    }
}

// Registered like the runner/exported games (Engine.h:135-144); the engine
// fires it inside Engine::systemInit.
QUANTUM_INIT void init() {
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
        buildDemoTree();
        if (frames < 1)
            frames = 1;

        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        for (int i = 0; i < frames; i++) {
            UIRuntime::buildFrame();
        }
        const double elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

        const UIRuntime::Stats& stats = UIRuntime::getStats();
        std::printf("quantum-ui-demo (headless)\n");
        std::printf("  elements   : %u\n", stats.elements);
        std::printf("  commands   : %u\n", stats.commands);
        std::printf("  glyph quads: %u\n", stats.glyphQuads);
        std::printf("  vertices   : %u\n", stats.vertices);
        std::printf("  indices    : %u\n", stats.indices);
        std::printf("  draw ranges: %u\n", stats.ranges);
        std::printf("  frames     : %d\n", frames);
        std::printf("  build total: %.3f ms (%.4f ms/frame)\n", elapsedMs, elapsedMs / (double)frames);
        return 0;
    }

    //windowed: --frames N draws N frames and closes the window itself, so the
    //same binary doubles as a smoke test next to the visual check
    if (frames > 0) {
        autoCloseFrames = frames;
        Engine::onDraw.add("ui_demo_auto_close", []() {
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
    // window close (QuantumWin::init, engine/platform/win/QuantumWin.cpp).
#if defined(_WIN32)
    const int result = QuantumWin::init(argc, argv);
#elif defined(__APPLE__)
    const int result = QuantumMac::init(argc, argv);
#else
    const int result = QuantumLinux::init(argc, argv);
#endif

    const UIRuntime::Stats& stats = UIRuntime::getStats();
    std::printf("quantum-ui-demo finished (code %d): %u elements, %u commands, %u ranges\n",
                result, stats.elements, stats.commands, stats.ranges);
    return result;
}
