// (c) SR VINIX
// SPDX-License-Identifier: MIT

#ifndef Slate_h
#define Slate_h

#include <memory>

#include "Export.h"
#include "slate/core/Geometry.h"
#include "slate/core/PaintList.h"
#include "slate/core/StyleSet.h"
#include "slate/core/Widget.h"

namespace quantum::slate {

    class ISlateRenderer;

    // Screen-space entry point of the Slate module: one widget tree ("canvas"),
    // styled, laid out and painted after the scenes each frame. It coexists with
    // the legacy scene UI and with quantum::ui, and never touches Dear ImGui.
    class Slate {
    public:
        struct Stats {
            unsigned int widgets = 0;     //visited by the paint pass
            unsigned int commands = 0;
            unsigned int vertices = 0;
            unsigned int indices = 0;
            unsigned int ranges = 0;      //draw calls
            unsigned int glyphQuads = 0;
            unsigned int layoutVisits = 0;
        };

        static void setRoot(std::unique_ptr<Widget> root);
        static Widget* getRoot();
        static bool hasRoot();

        //Theme used when no StyleSet is installed. getTheme() creates the
        //built-in dark theme on first use so widgets always have a look.
        static StyleSet& getTheme();
        static void setTheme(StyleSet* styles);

        //Style -> layout -> paint -> geometry, no GPU calls: safe with no window,
        //which is what the unit tests and --headless run.
        static void buildFrame();

        //Submits the built frame; called by Engine::systemDraw after the scenes.
        static void render();

        //Last built frame (debugging and tests).
        static const PaintList& getPaintList();
        static const SlateMesh& getMesh();
        static const Stats& getStats();

        //Drops the tree, the mesh and the stats (tests and scene changes).
        static void clear();
    };
}

#endif /* Slate_h */
