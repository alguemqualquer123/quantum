// (c) SR VINIX
// SPDX-License-Identifier: MIT

#ifndef Panels_h
#define Panels_h

#include <string>

#include "Export.h"
#include "ui/core/VisualElement.h"

namespace quantum::ui {

    // UMG-style layout containers over the flexbox engine. Each one is a thin
    // convenience: it prepares element styles and the YogaLayout pass does the
    // actual work, so the same containers answer to USS (the CSS phase) later.

    //Children on a row (main axis horizontal).
    class QUANTUM_API HBox : public VisualElement {
    public:
        HBox();
        explicit HBox(std::string id);
    };

    //Children on a column (main axis vertical).
    class QUANTUM_API VBox : public VisualElement {
    public:
        VBox();
        explicit VBox(std::string id);
    };

    //Row that wraps the children into new lines when the width runs out.
    class QUANTUM_API WrapBox : public VisualElement {
    public:
        WrapBox();
        explicit WrapBox(std::string id);
    };

    //Uniform grid: the width is divided into `columns` equal cells and the
    //children fill them in order, wrapping row by row. Cells always take the
    //cell width; use margins/padding inside the child for spacing.
    class QUANTUM_API GridPanel : public VisualElement {
    public:
        GridPanel();
        explicit GridPanel(std::string id);

        void setColumns(unsigned int columns);
        unsigned int getColumns() const;

    protected:
        bool updateLayoutChildren() override;

    private:
        unsigned int columns;
    };

    //Clipped viewport with a single content child (the first child). The offset
    //translates the content inside the viewport; wheel/gamepad scrolling and
    //the scrollbar arrive with the events phase.
    class QUANTUM_API ScrollBox : public VisualElement {
    public:
        ScrollBox();
        explicit ScrollBox(std::string id);

        void setContentOffset(Vector2 offset);
        const Vector2& getContentOffset() const;

    protected:
        bool updateLayoutChildren() override;

    private:
        Vector2 contentOffset;
    };

    //Constrain box: the content fills the box on the axes it does not size
    //itself (a child with its own width/height keeps it).
    class QUANTUM_API SizeBox : public VisualElement {
    public:
        SizeBox();
        explicit SizeBox(std::string id);

    protected:
        bool updateLayoutChildren() override;
    };
}

#endif /* Panels_h */
