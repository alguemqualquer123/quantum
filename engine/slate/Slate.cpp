// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "slate/Slate.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <numeric>
#include <vector>

#include "Engine.h"
#include "slate/layout/Containers.h"
#include "slate/layout/FlexEngine.h"
#include "slate/render/SlateRenderer.h"

namespace quantum::slate {

    namespace {

        std::unique_ptr<Widget> g_root;
        PaintList g_paintList;
        SlateMesh g_mesh;
        Slate::Stats g_stats;

        //intentionally leaked: the backend resources die with the sokol context at
        //shutdown, so deleting them during static destruction is unsafe (same
        //contract the legacy runtime keeps for its renderer, UIRuntime.h:56-58)
        ISlateRenderer* g_renderer = nullptr;

        //One index scratch per walk depth, kept alive between frames so a steady
        //state frame allocates nothing even when a subtree is z-sorted. The buffer
        //cannot be a single global: recursion happens while it is being iterated.
        struct OrderScratch {
            std::vector<unsigned int> indices;
        };
        std::vector<OrderScratch> g_orderScratch;

        //Mirrors Widget::applyPaintTransform, which is protected and therefore
        //invisible to the walker; keep the two in sync if the paint transform ever
        //grows a rotation.
        Rect transformRect(const Rect& rect, const PaintContext& context) {
            const float x0 = context.origin.x + (rect.getX() - context.origin.x) * context.scale.x;
            const float y0 = context.origin.y + (rect.getY() - context.origin.y) * context.scale.y;
            const float x1 = context.origin.x + (rect.getX() + rect.getWidth() - context.origin.x) * context.scale.x;
            const float y1 = context.origin.y + (rect.getY() + rect.getHeight() - context.origin.y) * context.scale.y;

            const float left = std::min(x0, x1);
            const float top = std::min(y0, y1);
            return Rect(left, top, std::fabs(x1 - x0), std::fabs(y1 - y0));
        }

        Rect intersectRect(const Rect& a, const Rect& b) {
            const float left = std::max(a.getX(), b.getX());
            const float top = std::max(a.getY(), b.getY());
            const float right = std::min(a.getX() + a.getWidth(), b.getX() + b.getWidth());
            const float bottom = std::min(a.getY() + a.getHeight(), b.getY() + b.getHeight());
            if (right <= left || bottom <= top) {
                return Rect(0.0f, 0.0f, 0.0f, 0.0f);
            }
            return Rect(left, top, right - left, bottom - top);
        }

        // ---------------------------------------------------------------------
        // style pass
        // ---------------------------------------------------------------------

        void styleWidget(Widget& widget, StyleSet* set, Slate::Stats& stats) {
            stats.layoutVisits++;

            //A widget authored without a style name still gets the theme's base
            //look: with the frozen Widget.h the only channel into StyleSet::resolve
            //is the style name, so "Default" is promoted here instead of being
            //hard-coded into the resolution ladder.
            if (set != nullptr && widget.getStyleName().empty() && set->findStyle("Default") != nullptr) {
                widget.setStyleName("Default");
            }

            //Widgets never cache a style across frames (the theme may be swapped),
            //so re-resolve whenever the style flag is set. applyStyle() itself is
            //cheap when neither the name nor the visual state changed.
            if ((widget.getDirtyFlags() & WidgetDirty::Style) != WidgetDirty::None) {
                widget.applyStyle();
                widget.clearDirtyFlags(WidgetDirty::Style);
            }

            const std::vector<std::unique_ptr<Widget>>& children = widget.getChildren();
            for (size_t i = 0; i < children.size(); i++) {
                styleWidget(*children[i], set, stats);
            }
        }

        void invalidateStyles(Widget& widget) {
            widget.markDirty(WidgetDirty::Style | WidgetDirty::Paint);
            const std::vector<std::unique_ptr<Widget>>& children = widget.getChildren();
            for (size_t i = 0; i < children.size(); i++) {
                invalidateStyles(*children[i]);
            }
        }

        // ---------------------------------------------------------------------
        // paint walk
        // ---------------------------------------------------------------------

        //True when the children are already in (zOrder, insertion) order, which is
        //the case for every tree nobody restacked: then no index buffer is needed.
        bool insertionOrderIsStable(const std::vector<std::unique_ptr<Widget>>& children) {
            int previous = 0;
            for (size_t i = 0; i < children.size(); i++) {
                const int z = children[i]->getZOrder();
                if (i > 0 && z < previous) {
                    return false;
                }
                previous = z;
            }
            return true;
        }

        void paintWidget(Widget& widget, const PaintContext& parentContext,
                         Slate::Stats& stats, size_t depth) {
            if (!widget.isVisible()) {
                return;
            }
            stats.widgets++;

            PaintContext context = parentContext;
            //nested opacities multiply; the style opacity lets a theme dim a whole
            //widget without touching the authoring
            context.opacity = parentContext.opacity * widget.getOpacity() * widget.getStyle().opacity;

            widget.paint(context);

            const std::vector<std::unique_ptr<Widget>>& children = widget.getChildren();
            if (children.empty()) {
                return;
            }

            //ScrollBox::paint walks its own content so the clip it builds covers the
            //whole subtree (see the note in Containers.cpp); recursing here would
            //emit everything inside the viewport twice.
            if (dynamic_cast<ScrollBox*>(&widget) != nullptr) {
                return;
            }

            PaintContext childContext = context;

            //Clip chain: clips accumulate by intersection, but only a widget that
            //clips narrows them. Everything else is overflow-visible, exactly like
            //UMG, so the inherited clip travels unchanged.
            if (ScaleBox* scale = dynamic_cast<ScaleBox*>(&widget)) {
                //the child rects are in design space: multiply the incoming scale
                //and rebase around this box's own on-screen origin, then keep the
                //scaled content inside the box it was fitted to
                const Rect box = transformRect(widget.getLayoutRect(), context);
                childContext.origin = Vector2(box.getX(), box.getY());
                childContext.scale = Vector2(context.scale.x * scale->getScale().x,
                    context.scale.y * scale->getScale().y);
                childContext.clip = childContext.hasClip ? intersectRect(childContext.clip, box) : box;
                childContext.hasClip = true;
            }

            if (insertionOrderIsStable(children)) {
                for (size_t i = 0; i < children.size(); i++) {
                    paintWidget(*children[i], childContext, stats, depth + 1);
                }
                return;
            }

            if (g_orderScratch.size() <= depth) {
                g_orderScratch.resize(depth + 1);
            }
            std::vector<unsigned int>& order = g_orderScratch[depth].indices;
            order.resize(children.size());
            std::iota(order.begin(), order.end(), 0u);
            //stable: equal zOrder keeps insertion order
            std::stable_sort(order.begin(), order.end(),
                [&children](unsigned int a, unsigned int b) {
                    return children[a]->getZOrder() < children[b]->getZOrder();
                });

            for (size_t i = 0; i < order.size(); i++) {
                paintWidget(*children[order[i]], childContext, stats, depth + 1);
            }
        }

        //Canvas size the frame is solved against. With no view loaded the engine
        //reports an unknown canvas, so the tree sizes itself to its own content and
        //the GPU-free passes still produce rects.
        Vector2 frameCanvasSize(const Widget& root) {
            float width = static_cast<float>(Engine::getCanvasWidth());
            float height = static_cast<float>(Engine::getCanvasHeight());
            if (width > 0.0f && height > 0.0f) {
                return Vector2(width, height);
            }
            const Vector2 content = root.getDesiredSize();
            return Vector2(std::max(width, content.x), std::max(height, content.y));
        }

        // ---------------------------------------------------------------------
        // built-in theme
        // ---------------------------------------------------------------------

        void paintStyle(WidgetStyle& style, const StyleSet& theme,
                        const char* background, const char* text, const char* border) {
            style.backgroundColor = theme.getColor(background);
            style.textColor = theme.getColor(text);
            style.borderColor = theme.getColor(border);
        }

        //NamedStyle layers a restyled state on top of the whole composed style, so
        //every state entry repeats the fields it wants to keep (see compose() in
        //StyleSet.cpp).
        void fillButtonStates(NamedStyle& named, const StyleSet& theme, const WidgetStyle& normal) {
            named.forState(WidgetVisualState::Normal) = normal;

            WidgetStyle hovered = normal;
            hovered.backgroundColor = theme.getColor("Slate.Button.Hovered");
            hovered.borderColor = theme.getColor("Slate.Button.Border.Hovered");
            named.forState(WidgetVisualState::Hovered) = hovered;

            WidgetStyle pressed = normal;
            pressed.backgroundColor = theme.getColor("Slate.Button.Pressed");
            pressed.borderColor = theme.getColor("Slate.Button.Border.Hovered");
            pressed.textColor = theme.getColor("Slate.Text.Muted");
            named.forState(WidgetVisualState::Pressed) = pressed;

            WidgetStyle focused = normal;
            focused.borderColor = theme.getColor("Slate.Accent");
            focused.borderWidth = 2.0f;
            named.forState(WidgetVisualState::Focused) = focused;

            WidgetStyle disabled = normal;
            disabled.backgroundColor = theme.getColor("Slate.Button.Disabled");
            disabled.textColor = theme.getColor("Slate.Text.Muted");
            disabled.borderColor = theme.getColor("Slate.Border");
            disabled.opacity = 0.6f;
            named.forState(WidgetVisualState::Disabled) = disabled;
        }
    }

    void Slate::setRoot(std::unique_ptr<Widget> root) {
        if (g_root != nullptr) {
            //the Yoga nodes are owned by the widgets but released here explicitly:
            //destruction order at shutdown is undefined (FlexEngine.h:23-25)
            FlexEngine::dispose(*g_root);
            g_root.reset();
        }
        g_root = std::move(root);

        //make sure the tree has something to look up its colours in
        getTheme();

        if (g_root != nullptr) {
            g_root->markSubtreeDirty(WidgetDirty::All);
        }
    }

    Widget* Slate::getRoot() {
        return g_root.get();
    }

    bool Slate::hasRoot() {
        return g_root != nullptr;
    }

    StyleSet& Slate::getTheme() {
        static StyleSet theme;
        static bool built = false;

        if (!built) {
            //colour tokens first: every named style below reads them, which is what
            //lets a project re-skin by re-registering tokens only
            theme.registerColor("Slate.Background", Vector4(0.078f, 0.086f, 0.109f, 1.0f));
            theme.registerColor("Slate.Panel", Vector4(0.125f, 0.141f, 0.176f, 1.0f));
            theme.registerColor("Slate.Box", Vector4(0.0f, 0.0f, 0.0f, 0.0f));
            theme.registerColor("Slate.Text", Vector4(0.937f, 0.945f, 0.965f, 1.0f));
            theme.registerColor("Slate.Text.Muted", Vector4(0.627f, 0.655f, 0.718f, 1.0f));
            theme.registerColor("Slate.Accent", Vector4(0.231f, 0.620f, 1.0f, 1.0f));
            theme.registerColor("Slate.Border", Vector4(0.251f, 0.278f, 0.341f, 1.0f));
            theme.registerColor("Slate.Button.Normal", Vector4(0.180f, 0.204f, 0.259f, 1.0f));
            theme.registerColor("Slate.Button.Hovered", Vector4(0.243f, 0.282f, 0.361f, 1.0f));
            theme.registerColor("Slate.Button.Pressed", Vector4(0.137f, 0.157f, 0.200f, 1.0f));
            theme.registerColor("Slate.Button.Disabled", Vector4(0.157f, 0.173f, 0.200f, 1.0f));
            theme.registerColor("Slate.Button.Border.Hovered", Vector4(0.404f, 0.478f, 0.639f, 1.0f));
            theme.registerColor("Slate.Hud.Text", Vector4(1.0f, 1.0f, 1.0f, 1.0f));
            theme.registerColor("Slate.Progress.Track", Vector4(0.094f, 0.106f, 0.133f, 1.0f));
            theme.registerColor("Slate.Progress.Fill", Vector4(0.290f, 0.765f, 0.431f, 1.0f));
            theme.registerColor("Slate.Image.Tint", Vector4(1.0f, 1.0f, 1.0f, 1.0f));

            WidgetStyle base;
            paintStyle(base, theme, "Slate.Box", "Slate.Text", "Slate.Border");
            base.fontSize = 16;
            base.textAlign = TextAlign::Near;
            base.overflow = TextOverflow::Clip;
            theme.defineStyle("Default").base() = base;

            WidgetStyle panel = base;
            panel.backgroundColor = theme.getColor("Slate.Panel");
            panel.borderColor = theme.getColor("Slate.Border");
            panel.borderWidth = 1.0f;
            panel.borderRadius = 6.0f;
            panel.padding = Insets(12.0f);
            NamedStyle& panelStyle = theme.defineStyle("Panel");
            panelStyle.setParent("Default");
            panelStyle.base() = panel;
            panelStyle.forState(WidgetVisualState::Normal) = panel;

            WidgetStyle box = base;
            box.padding = Insets(4.0f);
            NamedStyle& boxStyle = theme.defineStyle("Box");
            boxStyle.setParent("Default");
            boxStyle.base() = box;
            boxStyle.forState(WidgetVisualState::Normal) = box;

            WidgetStyle label = base;
            label.textColor = theme.getColor("Slate.Hud.Text");
            label.fontSize = 18;
            label.padding = Insets(2.0f);
            NamedStyle& labelStyle = theme.defineStyle("Hud.Label");
            labelStyle.setParent("Default");
            labelStyle.base() = label;
            labelStyle.forState(WidgetVisualState::Normal) = label;
            WidgetStyle labelDisabled = label;
            labelDisabled.textColor = theme.getColor("Slate.Text.Muted");
            labelDisabled.opacity = 0.5f;
            labelStyle.forState(WidgetVisualState::Disabled) = labelDisabled;

            WidgetStyle button = base;
            button.backgroundColor = theme.getColor("Slate.Button.Normal");
            button.borderColor = theme.getColor("Slate.Border");
            button.borderWidth = 1.0f;
            button.borderRadius = 5.0f;
            button.padding = Insets(Insets(8.0f, 6.0f, 8.0f, 6.0f));
            button.textAlign = TextAlign::Center;
            button.fontSize = 16;
            button.minSize = Vector2(0.0f, 28.0f);
            NamedStyle& buttonStyle = theme.defineStyle("Menu.Button");
            buttonStyle.setParent("Default");
            buttonStyle.base() = button;
            fillButtonStates(buttonStyle, theme, button);

            WidgetStyle progress = base;
            progress.backgroundColor = theme.getColor("Slate.Progress.Track");
            //WidgetStyle has no fill slot, so the progress bar reads its fill from
            //the style's second colour/brush pair (see widgets/ProgressBar.cpp)
            progress.borderColor = theme.getColor("Slate.Progress.Fill");
            progress.borderRadius = 4.0f;
            progress.minSize = Vector2(160.0f, 16.0f);
            progress.textAlign = TextAlign::Center;
            progress.fontSize = 12;
            NamedStyle& progressStyle = theme.defineStyle("Hud.ProgressBar");
            progressStyle.setParent("Default");
            progressStyle.base() = progress;
            progressStyle.forState(WidgetVisualState::Normal) = progress;
            WidgetStyle progressDisabled = progress;
            progressDisabled.borderColor = theme.getColor("Slate.Text.Muted");
            progressDisabled.opacity = 0.5f;
            progressStyle.forState(WidgetVisualState::Disabled) = progressDisabled;

            WidgetStyle image = base;
            //a brush is tinted by the style background colour; transparent would
            //erase it, so the theme tints with white and a brush name is authored
            //per instance (Image::setBrush)
            image.backgroundColor = theme.getColor("Slate.Image.Tint");
            NamedStyle& imageStyle = theme.defineStyle("Hud.Image");
            imageStyle.setParent("Default");
            imageStyle.base() = image;
            imageStyle.forState(WidgetVisualState::Normal) = image;
            WidgetStyle imageDisabled = image;
            imageDisabled.opacity = 0.35f;
            imageStyle.forState(WidgetVisualState::Disabled) = imageDisabled;

            built = true;
        }

        if (StyleSet::get() == nullptr) {
            StyleSet::set(&theme);
        }
        return theme;
    }

    void Slate::setTheme(StyleSet* styles) {
        StyleSet::set(styles);
        if (g_root != nullptr) {
            //the resolved cache is per StyleSet, so every node has to re-resolve and
            //the padding of the new theme can change the arrangement
            invalidateStyles(*g_root);
            g_root->markSubtreeDirty(WidgetDirty::Layout);
        }
    }

    void Slate::buildFrame() {
        g_stats = Stats();
        g_paintList.clear();
        g_mesh.clear();

        if (g_root == nullptr) {
            return;
        }

        //style
        styleWidget(*g_root, StyleSet::get(), g_stats);

        //layout
        const float requestedWidth = static_cast<float>(Engine::getCanvasWidth());
        const float requestedHeight = static_cast<float>(Engine::getCanvasHeight());
        if (requestedWidth > 0.0f && requestedHeight > 0.0f) {
            FlexEngine::layout(*g_root, requestedWidth, requestedHeight);
        } else {
            //no canvas to solve against (headless run, GPU-free test): the manual
            //arrange path still produces absolute rects sized to the content
            const Vector2 content = g_root->measure(Vector2(-1.0f, -1.0f));
            g_root->arrange(Rect(0.0f, 0.0f, content.x, content.y));
        }

        //paint
        PaintContext context;
        context.list = &g_paintList;
        const Vector2 canvas = frameCanvasSize(*g_root);
        if (canvas.x > 0.0f && canvas.y > 0.0f) {
            //everything off-canvas is culled by Geometry instead of being submitted
            context.clip = Rect(0.0f, 0.0f, canvas.x, canvas.y);
            context.hasClip = true;
        }
        paintWidget(*g_root, context, g_stats, 0);

        //geometry
        Geometry::build(g_paintList, g_mesh);

        g_stats.commands = g_paintList.getCommandCount();
        g_stats.glyphQuads = g_paintList.getGlyphQuadCount();
        g_stats.vertices = static_cast<unsigned int>(g_mesh.vertices.size());
        g_stats.indices = static_cast<unsigned int>(g_mesh.indices.size());
        g_stats.ranges = static_cast<unsigned int>(g_mesh.ranges.size());
    }

    void Slate::render() {
        if (g_root == nullptr || !Engine::isViewLoaded()) {
            return;
        }

        buildFrame();

        if (g_mesh.ranges.empty()) {
            return;
        }

        if (g_renderer == nullptr) {
            g_renderer = createSlateRenderer().release();
        }

        const float canvasWidth = static_cast<float>(Engine::getCanvasWidth());
        const float canvasHeight = static_cast<float>(Engine::getCanvasHeight());

        g_renderer->render(g_mesh, Vector2(canvasWidth, canvasHeight),
            Engine::getViewRect(), Engine::getFramebuffer());
    }

    const PaintList& Slate::getPaintList() {
        return g_paintList;
    }

    const SlateMesh& Slate::getMesh() {
        return g_mesh;
    }

    const Slate::Stats& Slate::getStats() {
        return g_stats;
    }

    void Slate::clear() {
        if (g_root != nullptr) {
            //the nodes must outlive the walk that frees them and go before the tree
            //is dropped, because a widget destructor cannot release a node its
            //siblings may still reference
            FlexEngine::dispose(*g_root);
            g_root.reset();
        }
        g_paintList.clear();
        g_mesh.clear();
        g_stats = Stats();
    }
}
