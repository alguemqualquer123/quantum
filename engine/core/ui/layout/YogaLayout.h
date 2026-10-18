// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#ifndef YogaLayout_h
#define YogaLayout_h

namespace doriax::ui {

    class VisualElement;

    // Bridge between the element tree and the vendored Yoga (flexbox) engine:
    // syncs styles and children into the YGNode graph, runs the layout and
    // stores the resolved rects back on the elements. Nodes are created on
    // demand; their memory is released with the owning element.
    class YogaLayout {
    public:
        //Applies every element carrying DirtyFlags::Layout (root first),
        //recalculates when anything changed and stores the absolute rects.
        //`width`/`height` are the canvas constraints: an auto-sized root gets
        //stretched to them so percent children have a definite basis.
        //Returns true when a layout pass ran.
        static bool update(VisualElement& rootElement, float width, float height);
    };
}

#endif /* YogaLayout_h */
