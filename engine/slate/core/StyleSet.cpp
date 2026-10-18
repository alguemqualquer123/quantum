// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#include "slate/core/StyleSet.h"

#include <algorithm>
#include <set>
#include <utility>

#include "Engine.h"
#include "Log.h"
#include "texture/Texture.h"

namespace doriax::slate {

    namespace {
        //The one active theme. Widgets reach it through StyleSet::get, the same
        //single-pointer model the rest of the engine uses for global services.
        StyleSet* activeStyleSet = nullptr;

        //Guard against a parent chain that loops back on itself or is absurdly
        //deep: resolve() recurses through resolve(), so one counter covers both.
        const int maxParentDepth = 16;
        int resolveDepth = 0;
        bool parentCycleLogged = false;

        //Tokens and brushes are logged once per name, not once per frame: a typo
        //in a theme would otherwise flood the log from the paint walk.
        std::set<std::string> loggedMissingColors;
        std::set<std::string> loggedMissingBrushes;

        //True when any field differs from the WidgetStyle defaults. NamedStyle's
        //header exposes no "has override for state" query, and a state entry
        //whose fields all match the defaults would replace a composed style
        //with... the defaults it already has, so treating it as absent is both
        //safe and the only test the public API allows.
        bool isRestyled(const WidgetStyle& s) {
            static const WidgetStyle defaults;
            return s.backgroundColor != defaults.backgroundColor ||
                   s.borderColor != defaults.borderColor ||
                   s.textColor != defaults.textColor ||
                   !s.backgroundBrush.empty() || !s.borderBrush.empty() ||
                   s.borderWidth != defaults.borderWidth ||
                   s.borderRadius != defaults.borderRadius ||
                   s.opacity != defaults.opacity ||
                   !s.padding.isZero() ||
                   s.minSize != defaults.minSize || s.maxSize != defaults.maxSize ||
                   !s.fontFamily.empty() ||
                   s.fontSize != defaults.fontSize ||
                   s.textAlign != defaults.textAlign ||
                   s.wrap != defaults.wrap ||
                   s.overflow != defaults.overflow;
        }

        //Layers `layer` onto `out`, field by field, treating a value equal to
        //the WidgetStyle default as "the theme never set this field". There is
        //no presence bitmask in WidgetStyle (the header is frozen), so this is
        //the cheapest honest approximation: a theme can restyle one field of a
        //parent style, but cannot force a field back to its default value.
        void overlayBase(WidgetStyle& out, const WidgetStyle& layer) {
            static const WidgetStyle defaults;

            if (layer.backgroundColor != defaults.backgroundColor)
                out.backgroundColor = layer.backgroundColor;
            if (layer.borderColor != defaults.borderColor)
                out.borderColor = layer.borderColor;
            if (layer.textColor != defaults.textColor)
                out.textColor = layer.textColor;
            if (!layer.backgroundBrush.empty())
                out.backgroundBrush = layer.backgroundBrush;
            if (!layer.borderBrush.empty())
                out.borderBrush = layer.borderBrush;
            if (layer.borderWidth != defaults.borderWidth)
                out.borderWidth = layer.borderWidth;
            if (layer.borderRadius != defaults.borderRadius)
                out.borderRadius = layer.borderRadius;
            if (layer.opacity != defaults.opacity)
                out.opacity = layer.opacity;
            if (!layer.padding.isZero())
                out.padding = layer.padding;
            if (layer.minSize != defaults.minSize)
                out.minSize = layer.minSize;
            if (layer.maxSize != defaults.maxSize)
                out.maxSize = layer.maxSize;
            if (!layer.fontFamily.empty())
                out.fontFamily = layer.fontFamily;
            if (layer.fontSize != defaults.fontSize)
                out.fontSize = layer.fontSize;
            if (layer.textAlign != defaults.textAlign)
                out.textAlign = layer.textAlign;
            if (layer.wrap != defaults.wrap)
                out.wrap = layer.wrap;
            if (layer.overflow != defaults.overflow)
                out.overflow = layer.overflow;
        }
    }

    NamedStyle::NamedStyle() {
    }

    WidgetStyle& NamedStyle::base() {
        return baseStyle;
    }

    const WidgetStyle& NamedStyle::base() const {
        return baseStyle;
    }

    WidgetStyle& NamedStyle::forState(WidgetVisualState state) {
        return stateOverrides[static_cast<int>(state)];
    }

    const WidgetStyle& NamedStyle::stateStyle(WidgetVisualState state) const {
        auto it = stateOverrides.find(static_cast<int>(state));
        if (it == stateOverrides.end()) {
            static const WidgetStyle defaultStyle;
            return defaultStyle;
        }
        return it->second;
    }

    void NamedStyle::setParent(const std::string& parentName) {
        parent = parentName;
    }

    const std::string& NamedStyle::getParent() const {
        return parent;
    }

    StyleSet* StyleSet::get() {
        return activeStyleSet;
    }

    void StyleSet::set(StyleSet* styles) {
        activeStyleSet = styles;
    }

    void StyleSet::registerBrush(const std::string& name, const SlateBrush& brush) {
        if (name.empty())
            return;

        SlateBrush stored = brush;
        stored.name = name;
        brushes[name] = stored;
    }

    const SlateBrush* StyleSet::findBrush(const std::string& name) const {
        auto it = brushes.find(name);
        if (it == brushes.end())
            return nullptr;
        return &it->second;
    }

    const SlateBrush& StyleSet::getBrush(const std::string& name) {
        static const SlateBrush emptyBrush;

        auto it = brushes.find(name);
        if (it == brushes.end())
            return emptyBrush;

        //Lazy upload: a brush is registered with a path (or pre-set data) and
        //only hits TextureDataPool on first paint. Texture::load is idempotent:
        //once pixels are in hand (or the attempt failed and needLoad was
        //cleared) it returns without touching the disk again.
        //
        //With no view loaded the resource system is not running (System::instance
        //aborts, the VFS has no mount points), so the brush paints empty: that is
        //what keeps the layout/paint walk runnable in a GPU-free test.
        if (!Engine::isViewLoaded())
            return emptyBrush;

        Texture& tex = it->second.texture;
        if (!tex.isFramebuffer() && !tex.hasData()) {
            TextureLoadResult result = tex.load();
            if (!result && !tex.hasData()) {
                if (loggedMissingBrushes.insert(name).second) {
                    Log::error("Slate StyleSet: brush '%s' could not load texture '%s'",
                               name.c_str(), tex.getPath().c_str());
                }
                return emptyBrush;
            }
        }

        return it->second;
    }

    void StyleSet::registerColor(const std::string& token, const Vector4& color) {
        colors[token] = color;
    }

    const Vector4& StyleSet::getColor(const std::string& token) const {
        auto it = colors.find(token);
        if (it == colors.end()) {
            if (loggedMissingColors.insert(token).second) {
                Log::error("Slate StyleSet: colour token '%s' is not registered",
                           token.c_str());
            }
            return missingColor;
        }
        return it->second;
    }

    NamedStyle& StyleSet::defineStyle(const std::string& name) {
        //A theme is rebuilt by redefining its styles (cold or hot reload), so
        //dropping the whole resolved cache here is both the simplest and the
        //safest invalidation: per-name keys cannot see transitive parent edits.
        resolvedCache.clear();
        return styles[name];
    }

    const NamedStyle* StyleSet::findStyle(const std::string& name) const {
        auto it = styles.find(name);
        if (it == styles.end())
            return nullptr;
        return &it->second;
    }

    const WidgetStyle& StyleSet::resolve(const std::string& name, WidgetVisualState state) {
        static const WidgetStyle defaultStyle;

        if (name.empty())
            return defaultStyle;

        const NamedStyle* style = findStyle(name);
        if (style == nullptr)
            return defaultStyle;

        const std::string key = name + ":" + std::to_string(static_cast<int>(state));
        auto cached = resolvedCache.find(key);
        if (cached != resolvedCache.end() && cached->second.valid)
            return cached->second.style;

        Resolved resolved;
        resolved.style = WidgetStyle();

        if (resolveDepth >= maxParentDepth) {
            if (!parentCycleLogged) {
                parentCycleLogged = true;
                Log::error("Slate StyleSet: parent chain is too deep or cyclic near '%s'; "
                           "ignoring further ancestors (logged once)", name.c_str());
            }
        } else if (!style->getParent().empty()) {
            //Seed with the parent's fully composed values; compose layers this
            //style on top. resolve() memoises the parent, so a shared ancestor
            //is flattened once per (parent, state) pair.
            resolveDepth++;
            resolved.style = resolve(style->getParent(), state);
            resolveDepth--;
        }

        compose(resolved.style, *style, state);

        if (cached == resolvedCache.end()) {
            cached = resolvedCache.emplace(key, Resolved()).first;
        }
        cached->second.style = std::move(resolved.style);
        cached->second.valid = true;
        return cached->second.style;
    }

    void StyleSet::compose(WidgetStyle& out, const NamedStyle& style, WidgetVisualState state) {
        overlayBase(out, style.base());

        //Whole-state override, by decision: WidgetStyle carries no "field was
        //set" flags, so a restyled state replaces the entire composed style.
        //Limitation: a theme that restyles one field for Hovered must repeat
        //every field it wants kept from the base.
        const WidgetStyle& overrideStyle = style.stateStyle(state);
        if (isRestyled(overrideStyle)) {
            out = overrideStyle;
        }
    }

    void StyleSet::clear() {
        brushes.clear();
        colors.clear();
        styles.clear();
        resolvedCache.clear();
        loggedMissingColors.clear();
        loggedMissingBrushes.clear();
        parentCycleLogged = false;
    }
}
