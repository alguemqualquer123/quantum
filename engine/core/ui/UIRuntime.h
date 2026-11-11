// (c) SR VINIX
// SPDX-License-Identifier: MIT

#ifndef UIRuntime_h
#define UIRuntime_h

#include <memory>

#include "Export.h"
#include "ui/core/VisualElement.h"
#include "ui/render/PaintList.h"
#include "ui/render/VertexBuilder.h"

namespace quantum::ui {

    class UIRenderer;

    // Global screen-space UI runtime of the new UI module: one root element
    // tree ("canvas"), painted after the scenes each frame. Layout arrives with
    // the flexbox phase; today elements are placed explicitly through
    // VisualElement::setPosition/setSize.
    class QUANTUM_API UIRuntime {
    public:
        struct Stats {
            unsigned int elements = 0;   //visited by the paint pass
            unsigned int commands = 0;
            unsigned int vertices = 0;
            unsigned int indices = 0;
            unsigned int ranges = 0;     //draw calls
            unsigned int glyphQuads = 0;
        };

        static void setRoot(std::unique_ptr<VisualElement> root);
        static VisualElement* getRoot();
        static bool hasRoot();

        // Paint pass + mesh build only; no GPU calls, safe without a view.
        static void buildFrame();
        static const Stats& getStats();

        // Last built frame (debugging and unit tests).
        static const PaintList& getPaintList();
        static const UIMesh& getMesh();

        // Called by Engine::systemDraw after the scenes drew.
        static void render();

        // Drops the tree and stats (tests).
        static void clear();

    private:
        static std::unique_ptr<VisualElement> root;
        static PaintList paintList;
        static UIMesh mesh;

        //intentionally leaked: the backend resources die with the sokol context
        //at shutdown, so deleting them during static destruction is unsafe
        static UIRenderer* renderer;

        static Stats stats;
    };
}

#endif /* UIRuntime_h */
