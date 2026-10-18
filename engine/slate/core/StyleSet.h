// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#ifndef SlateStyleSet_h
#define SlateStyleSet_h

#include <map>
#include <string>
#include <vector>

#include "Export.h"
#include "math/Vector2.h"
#include "math/Vector4.h"
#include "slate/core/SlateTypes.h"

namespace doriax::slate {

    class Widget;

    // Everything a widget paints that a theme may change. One entry per visual
    // state, resolved by StyleSet::resolve (Slate's per-state style attributes).
    struct WidgetStyle {
        Vector4 backgroundColor = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
        Vector4 borderColor = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
        Vector4 textColor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        std::string backgroundBrush;   //StyleSet brush name, empty for none
        std::string borderBrush;
        float borderWidth = 0.0f;
        float borderRadius = 0.0f;
        float opacity = 1.0f;
        Insets padding;
        Vector2 minSize = Vector2(0.0f, 0.0f);
        Vector2 maxSize = Vector2(0.0f, 0.0f);   //0 = unlimited

        //text (only read by text-bearing widgets)
        std::string fontFamily;
        unsigned int fontSize = 16;
        TextAlign textAlign = TextAlign::Near;
        bool wrap = false;
        TextOverflow overflow = TextOverflow::Clip;
    };

    // A named style: base values plus optional per-state overrides, the way a
    // Slate style set maps "Menu.Button" to Normal/Hovered/Pressed/Disabled.
    class NamedStyle {
    public:
        NamedStyle();

        WidgetStyle& base();
        const WidgetStyle& base() const;

        WidgetStyle& forState(WidgetVisualState state);
        const WidgetStyle& stateStyle(WidgetVisualState state) const;

        //Parent style this one layers on top of, so a theme can define a common
        //button look and only override the hovered colour per variant.
        void setParent(const std::string& parentName);
        const std::string& getParent() const;

    private:
        WidgetStyle baseStyle;
        //sparse: only the states a theme actually restyled are present
        std::map<int, WidgetStyle> stateOverrides;
        std::string parent;
    };

    // Theme of the Slate module: colour tokens, brushes and named styles.
    // Built in code today (like a Slate ISlateStyleModule); USS feeds the same
    // tables once the markup phase lands.
    class StyleSet {
    public:
        //The active theme. Widgets look styles up here; nullptr means every
        //widget falls back to its own defaults (used by the GPU-free tests).
        static StyleSet* get();
        static void set(StyleSet* styles);

        //Brushes are registered by name so assets can be re-pointed at runtime.
        void registerBrush(const std::string& name, const SlateBrush& brush);
        const SlateBrush* findBrush(const std::string& name) const;
        //Loads the texture lazily on first use; returns an empty brush when the
        //path cannot be resolved, so a missing icon never stops the paint pass.
        const SlateBrush& getBrush(const std::string& name);

        void registerColor(const std::string& token, const Vector4& color);
        const Vector4& getColor(const std::string& token) const;

        NamedStyle& defineStyle(const std::string& name);
        const NamedStyle* findStyle(const std::string& name) const;

        //Flattens parent + base + state override into the values a widget uses.
        //Cached per (style, state) pair; the cache is dropped when a style is
        //redefined so a hot-reloaded theme is picked up on the next frame.
        const WidgetStyle& resolve(const std::string& name, WidgetVisualState state);

        void clear();

    private:
        struct Resolved {
            WidgetStyle style;
            bool valid = false;
        };

        std::map<std::string, SlateBrush> brushes;
        std::map<std::string, Vector4> colors;
        std::map<std::string, NamedStyle> styles;
        //key = style name + state index
        std::map<std::string, Resolved> resolvedCache;
        Vector4 missingColor;

        static void compose(WidgetStyle& out, const NamedStyle& style, WidgetVisualState state);
    };
}

#endif /* SlateStyleSet_h */
