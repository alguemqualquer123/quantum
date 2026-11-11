// Stylesheet.h - a miniature USS/CSS engine for the UI Toolkit page.
//
// Rules are parsed from text at runtime (the page lets the user edit the sheet)
// and resolved into colours/metrics that the page uses to draw its elements.
// Supported properties are intentionally a small subset of Unity's USS.
#pragma once

#include <string>
#include <vector>

#include <imgui.h>

namespace uilab {
namespace demo {

// A resolved style: the properties of one rule (defaults are "unset" values).
struct ResolvedStyle {
    bool hasBackground = false;
    ImVec4 background = ImVec4(0, 0, 0, 0);
    bool hasColor = false;
    ImVec4 color = ImVec4(0, 0, 0, 0);
    bool hasBorder = false;
    ImVec4 borderColor = ImVec4(0, 0, 0, 0);
    float borderWidth = 0.0f;

    float paddingTop = 0.0f, paddingRight = 0.0f, paddingBottom = 0.0f, paddingLeft = 0.0f;
    float marginTop = 0.0f, marginRight = 0.0f, marginBottom = 0.0f, marginLeft = 0.0f;
    float borderRadius = 0.0f;
    float width = 0.0f, height = 0.0f;       // 0 = auto
    float fontSize = 0.0f;                   // 0 = inherit
    float flexGrow = 0.0f;
    int flexDirection = 0;                   // 0 = row, 1 = column (-1 = unset)
    int alignItems = -1;                     // 0 start, 1 center, 2 end, 3 stretch
    bool unityBackground = false;            // -unity-font-style: bold marker
    bool bold = false;
    bool italic = false;

    bool anySet = false;
    std::string Describe() const;            // short "prop=value" summary
};

struct StyleRule {
    std::string selector;                    // ".card", "#title", "button.primary", ":hover" suffix allowed
    std::vector<std::pair<std::string, std::string>> properties;
    int sourceLine = 0;

    // Matches a simple element description (tag + id + classes + hovered).
    bool Matches(const std::string& tag, const std::string& id, const std::vector<std::string>& classes,
                 bool hovered) const;
};

class Sheet {
public:
    // Parses "selector { property: value; ... }" text. Unknown properties are
    // kept (shown by the editor) but ignored when resolving. Returns false and
    // fills errorOut on a syntax error (unterminated block / missing brace).
    bool Parse(const std::string& uss, std::string& errorOut);
    void Clear() { rules_.clear(); errors_.clear(); }

    const std::vector<StyleRule>& Rules() const { return rules_; }
    const std::vector<std::string>& Errors() const { return errors_; }
    int RuleCount() const { return static_cast<int>(rules_.size()); }
    int PropertyCount() const;

    // Applies every matching rule in source order (later rules win).
    ResolvedStyle Resolve(const std::string& tag, const std::string& id,
                          const std::vector<std::string>& classes, bool hovered) const;

    // Convenience for the page: resolve and blend onto a base colour.
    ImVec4 BackgroundOr(const ResolvedStyle& s, ImVec4 fallback) const;

    static bool ParseColor(const std::string& value, ImVec4& out);   // #rgb, #rrggbb, #rrggbbaa, rgb(), names
    static bool ParseLength(const std::string& value, float& out);    // "12px", "1.5", "50%" -> 0.5
    static const char* DefaultUss();

private:
    std::vector<StyleRule> rules_;
    std::vector<std::string> errors_;
};

} // namespace demo
} // namespace uilab
