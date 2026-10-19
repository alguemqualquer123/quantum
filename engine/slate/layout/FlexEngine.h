// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#ifndef SlateFlexEngine_h
#define SlateFlexEngine_h

#include "Export.h"

namespace doriax::slate {

    class Widget;

    // Yoga-backed flexbox pass. Each widget owns a YGNode created and released
    // here; the node is rebuilt from the widget's Slot plus its container's flow
    // properties whenever the layout flag is set, then absolute rects are pushed
    // back into the widgets.
    class FlexEngine {
    public:
        //Runs layout for the whole tree and writes every getLayoutRect().
        //Returns false when the tree has no measurable root size.
        static bool layout(Widget& root, float canvasWidth, float canvasHeight);

        //Drops the Yoga nodes (widget destruction order is undefined at
        //shutdown, so nodes are freed explicitly by the runtime).
        static void dispose(Widget& root);
    };
}

#endif /* SlateFlexEngine_h */
