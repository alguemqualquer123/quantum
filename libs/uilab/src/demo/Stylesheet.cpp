// Stylesheet.cpp - miniature USS engine: text in, rules out, rules resolve to
// colours/metrics the UI Toolkit page draws with.
#include "demo/Stylesheet.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace uilab {
namespace demo {

namespace {

char LowerChar(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

std::string ToLowerAscii(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        out.push_back(LowerChar(c));
    return out;
}

bool IsSpaceChar(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string Trim(const std::string& s) {
    size_t a = 0;
    size_t b = s.size();
    while (a < b && IsSpaceChar(s[a]))
        ++a;
    while (b > a && IsSpaceChar(s[b - 1]))
        --b;
    return s.substr(a, b - a);
}

bool IsBlank(const std::string& s) {
    for (char c : s)
        if (!IsSpaceChar(c))
            return false;
    return true;
}

int LineOf(const std::string& text, size_t pos) {
    int line = 1;
    const size_t limit = pos < text.size() ? pos : text.size();
    for (size_t i = 0; i < limit; ++i)
        if (text[i] == '\n')
            ++line;
    return line;
}

// Replaces /* ... */ comments with spaces (newlines kept so line numbers hold).
std::string StripComments(const std::string& src, std::vector<std::string>& errors) {
    std::string out;
    out.reserve(src.size());
    size_t i = 0;
    while (i < src.size()) {
        if (i + 1 < src.size() && src[i] == '/' && src[i + 1] == '*') {
            size_t j = i + 2;
            bool closed = false;
            while (j + 1 < src.size()) {
                if (src[j] == '*' && src[j + 1] == '/') {
                    j += 2;
                    closed = true;
                    break;
                }
                if (src[j] == '\n')
                    out.push_back('\n');
                ++j;
            }
            if (!closed) {
                // Unterminated: swallow the remainder (counting newlines).
                for (size_t k = i; k < src.size(); ++k)
                    if (src[k] == '\n')
                        out.push_back('\n');
                j = src.size();
                errors.push_back("Unterminated comment (missing \"*/\").");
            }
            i = j;
        } else {
            out.push_back(src[i]);
            ++i;
        }
    }
    return out;
}

bool HexVal(char c, int& out) {
    if (c >= '0' && c <= '9') { out = c - '0'; return true; }
    if (c >= 'a' && c <= 'f') { out = 10 + (c - 'a'); return true; }
    if (c >= 'A' && c <= 'F') { out = 10 + (c - 'A'); return true; }
    return false;
}

std::vector<std::string> SplitOnSpace(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && IsSpaceChar(s[i]))
            ++i;
        size_t start = i;
        while (i < s.size() && !IsSpaceChar(s[i]))
            ++i;
        if (i > start)
            out.push_back(s.substr(start, i - start));
    }
    return out;
}

bool ParseNumber(const std::string& tok, double& out) {
    const std::string t = Trim(tok);
    if (t.empty())
        return false;
    char* end = nullptr;
    out = std::strtod(t.c_str(), &end);
    return end != t.c_str();
}

bool ParseNamedColor(const std::string& name, ImVec4& out) {
    struct Named { const char* name; int r, g, b, a; };
    static const Named table[] = {
        { "white", 255, 255, 255, 255 }, { "black", 0, 0, 0, 255 },
        { "red", 255, 0, 0, 255 },       { "green", 0, 128, 0, 255 },
        { "blue", 0, 0, 255, 255 },      { "orange", 255, 165, 0, 255 },
        { "yellow", 255, 255, 0, 255 },  { "purple", 128, 0, 128, 255 },
        { "gray", 128, 128, 128, 255 },  { "grey", 128, 128, 128, 255 },
        { "transparent", 0, 0, 0, 0 },
    };
    for (const Named& n : table) {
        if (name == n.name) {
            out = ImVec4(n.r / 255.0f, n.g / 255.0f, n.b / 255.0f, n.a / 255.0f);
            return true;
        }
    }
    return false;
}

void ApplyBoxSides(const std::string& value, float& top, float& right, float& bottom, float& left) {
    const std::vector<std::string> tokens = SplitOnSpace(value);
    float vals[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    if (tokens.empty() || tokens.size() == 3 || tokens.size() > 4)
        return;    // 1, 2 or 4 values are valid shorthands; anything else is ignored
    for (size_t i = 0; i < tokens.size(); ++i) {
        float v = 0.0f;
        if (!Sheet::ParseLength(tokens[i], v))
            return;
        vals[i] = v;
    }
    if (tokens.size() == 1) {
        top = right = bottom = left = vals[0];
    } else if (tokens.size() == 2) {
        top = bottom = vals[0];
        right = left = vals[1];
    } else {
        top = vals[0];
        right = vals[1];
        bottom = vals[2];
        left = vals[3];
    }
}

std::string ColorHex(ImVec4 c) {
    auto clampByte = [](float v) {
        int b = static_cast<int>(v * 255.0f + 0.5f);
        if (b < 0) b = 0;
        if (b > 255) b = 255;
        return b;
    };
    char buf[12];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", clampByte(c.x), clampByte(c.y), clampByte(c.z));
    return std::string(buf);
}

// Applies one declaration to a resolved style. Returns false for unknown names
// (they stay visible in the rule list but are ignored here).
bool ApplyProperty(ResolvedStyle& s, const std::string& name, const std::string& value) {
    float len = 0.0f;
    ImVec4 col{};
    if (name == "background-color" || name == "-unity-background-color") {
        if (Sheet::ParseColor(value, col)) {
            s.hasBackground = true;
            s.background = col;
            if (name[1] == 'u')
                s.unityBackground = true;    // property used the -unity- prefix
            s.anySet = true;
        }
        return true;
    }
    if (name == "color" || name == "-unity-text-color") {
        if (Sheet::ParseColor(value, col)) {
            s.hasColor = true;
            s.color = col;
        }
        return true;
    }
    if (name == "border-color") {
        if (Sheet::ParseColor(value, col)) {
            s.hasBorder = true;
            s.borderColor = col;
        }
        return true;
    }
    if (name == "border-width") {
        if (Sheet::ParseLength(value, len))
            s.borderWidth = len;
        s.anySet = true;
        return true;
    }
    if (name == "padding") {
        ApplyBoxSides(value, s.paddingTop, s.paddingRight, s.paddingBottom, s.paddingLeft);
        s.anySet = true;
        return true;
    }
    if (name == "padding-top" && Sheet::ParseLength(value, len)) { s.paddingTop = len; s.anySet = true; return true; }
    if (name == "padding-right" && Sheet::ParseLength(value, len)) { s.paddingRight = len; s.anySet = true; return true; }
    if (name == "padding-bottom" && Sheet::ParseLength(value, len)) { s.paddingBottom = len; s.anySet = true; return true; }
    if (name == "padding-left" && Sheet::ParseLength(value, len)) { s.paddingLeft = len; s.anySet = true; return true; }
    if (name == "margin") {
        ApplyBoxSides(value, s.marginTop, s.marginRight, s.marginBottom, s.marginLeft);
        s.anySet = true;
        return true;
    }
    if (name == "margin-top" && Sheet::ParseLength(value, len)) { s.marginTop = len; s.anySet = true; return true; }
    if (name == "margin-right" && Sheet::ParseLength(value, len)) { s.marginRight = len; s.anySet = true; return true; }
    if (name == "margin-bottom" && Sheet::ParseLength(value, len)) { s.marginBottom = len; s.anySet = true; return true; }
    if (name == "margin-left" && Sheet::ParseLength(value, len)) { s.marginLeft = len; s.anySet = true; return true; }
    if (name == "border-radius" && Sheet::ParseLength(value, len)) { s.borderRadius = len; s.anySet = true; return true; }
    if (name == "width" && Sheet::ParseLength(value, len)) { s.width = len; s.anySet = true; return true; }
    if (name == "height" && Sheet::ParseLength(value, len)) { s.height = len; s.anySet = true; return true; }
    if (name == "font-size" && Sheet::ParseLength(value, len)) { s.fontSize = len; s.anySet = true; return true; }
    if (name == "flex-grow" && Sheet::ParseLength(value, len)) { s.flexGrow = len; s.anySet = true; return true; }
    if (name == "flex-direction") {
        const std::string v = ToLowerAscii(Trim(value));
        if (v == "row") { s.flexDirection = 0; s.anySet = true; }
        else if (v == "column") { s.flexDirection = 1; s.anySet = true; }
        return true;
    }
    if (name == "align-items") {
        const std::string v = ToLowerAscii(Trim(value));
        if (v == "flex-start") { s.alignItems = 0; s.anySet = true; }
        else if (v == "center") { s.alignItems = 1; s.anySet = true; }
        else if (v == "flex-end") { s.alignItems = 2; s.anySet = true; }
        else if (v == "stretch") { s.alignItems = 3; s.anySet = true; }
        return true;
    }
    if (name == "-unity-font-style") {
        const std::string v = ToLowerAscii(Trim(value));
        s.bold = (v == "bold" || v == "bold-and-italic");
        s.italic = (v == "italic" || v == "bold-and-italic");
        if (v == "normal" || v == "bold" || v == "italic" || v == "bold-and-italic")
            s.unityBackground = s.unityBackground || (v == "bold" || v == "bold-and-italic");
        s.anySet = true;
        return true;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Sheet
// ---------------------------------------------------------------------------
bool Sheet::Parse(const std::string& uss, std::string& errorOut) {
    rules_.clear();
    errors_.clear();
    errorOut.clear();

    const std::string clean = StripComments(uss, errors_);
    bool structural = errors_.size() > static_cast<size_t>(0);    // e.g. unterminated comment

    // One rule is created per selector in a comma separated list: ".a, .b { }"
    // produces two StyleRule entries sharing the same declarations.
    size_t pos = 0;
    while (pos < clean.size()) {
        const size_t open = clean.find('{', pos);
        if (open == std::string::npos) {
            if (!IsBlank(clean.substr(pos))) {
                errorOut = "Unexpected text after the last rule at line " + std::to_string(LineOf(clean, pos)) + ".";
                errors_.push_back(errorOut);
                structural = true;
            }
            break;
        }
        const size_t close = clean.find('}', open + 1);
        if (close == std::string::npos) {
            errorOut = "Missing '}' for the rule at line " + std::to_string(LineOf(clean, open)) + ".";
            errors_.push_back(errorOut);
            structural = true;
            pos = clean.size();
            break;
        }

        const int line = LineOf(clean, open);
        const std::string selectorList = Trim(clean.substr(pos, open - pos));
        const size_t firstRule = rules_.size();
        if (selectorList.empty()) {
            errors_.push_back("Rule with an empty selector at line " + std::to_string(line) + ".");
        } else {
            size_t selPos = 0;
            while (selPos <= selectorList.size()) {
                const size_t comma = selectorList.find(',', selPos);
                const size_t end = comma == std::string::npos ? selectorList.size() : comma;
                const std::string selector = Trim(selectorList.substr(selPos, end - selPos));
                if (!selector.empty()) {
                    StyleRule rule;
                    rule.selector = selector;
                    rule.sourceLine = line;
                    rules_.push_back(rule);
                }
                if (comma == std::string::npos)
                    break;
                selPos = comma + 1;
            }
        }
        const size_t lastRule = rules_.size();

        // Declarations: split on ';', name/value on the first ':'.
        const std::string body = clean.substr(open + 1, close - open - 1);
        size_t declPos = 0;
        while (declPos <= body.size()) {
            const size_t semi = body.find(';', declPos);
            const size_t end = semi == std::string::npos ? body.size() : semi;
            const std::string decl = Trim(body.substr(declPos, end - declPos));
            if (!decl.empty()) {
                const size_t colon = decl.find(':');
                const std::string name = ToLowerAscii(Trim(decl.substr(0, colon)));
                const std::string value = Trim(colon == std::string::npos ? std::string() : decl.substr(colon + 1));
                if (colon == std::string::npos || name.empty() || value.empty()) {
                    errors_.push_back("Invalid declaration \"" + decl + "\" near line " + std::to_string(line) + ".");
                } else {
                    // Attach to the rules produced by this block only.
                    for (size_t i = firstRule; i < lastRule; ++i)
                        rules_[i].properties.emplace_back(name, value);
                }
            }
            if (semi == std::string::npos)
                break;
            declPos = semi + 1;
        }

        pos = close + 1;
    }

    if (errorOut.empty() && !errors_.empty())
        errorOut = errors_.front();
    return !structural;
}

ResolvedStyle Sheet::Resolve(const std::string& tag, const std::string& id,
                             const std::vector<std::string>& classes, bool hovered) const {
    ResolvedStyle style;
    for (const StyleRule& rule : rules_) {
        if (!rule.Matches(tag, id, classes, hovered))
            continue;
        for (const auto& prop : rule.properties)
            ApplyProperty(style, prop.first, prop.second);
    }
    return style;
}

ImVec4 Sheet::BackgroundOr(const ResolvedStyle& s, ImVec4 fallback) const {
    return s.hasBackground ? s.background : fallback;
}

int Sheet::PropertyCount() const {
    int total = 0;
    for (const StyleRule& r : rules_)
        total += static_cast<int>(r.properties.size());
    return total;
}

bool Sheet::ParseColor(const std::string& value, ImVec4& out) {
    const std::string v = ToLowerAscii(Trim(value));
    if (v.empty())
        return false;

    if (v[0] == '#') {
        const std::string hex = v.substr(1);
        for (char c : hex) {
            int d = 0;
            if (!HexVal(c, d))
                return false;
        }
        auto byteAt = [&hex](size_t index, size_t unit) -> int {
            if (unit == 1) {
                int d = 0;
                HexVal(hex[index], d);
                return d * 17;
            }
            int hi = 0;
            int lo = 0;
            HexVal(hex[index], hi);
            HexVal(hex[index + 1], lo);
            return hi * 16 + lo;
        };
        if (hex.size() != 3 && hex.size() != 6 && hex.size() != 8)
            return false;
        const size_t unit = hex.size() == 3 ? 1 : 2;
        out.x = byteAt(0, unit) / 255.0f;
        out.y = byteAt(unit == 1 ? 1 : 2, unit) / 255.0f;
        out.z = byteAt(unit == 1 ? 2 : 4, unit) / 255.0f;
        out.w = hex.size() == 8 ? byteAt(6, 2) / 255.0f : 1.0f;
        return true;
    }

    if (v.rfind("rgb(", 0) == 0 || v.rfind("rgba(", 0) == 0) {
        const size_t open = v.find('(');
        const size_t close = v.find(')', open);
        if (close == std::string::npos)
            return false;
        const std::vector<std::string> parts = [&] {
            std::vector<std::string> p;
            const std::string inner = v.substr(open + 1, close - open - 1);
            size_t i = 0;
            while (i <= inner.size()) {
                const size_t comma = inner.find(',', i);
                const size_t end = comma == std::string::npos ? inner.size() : comma;
                p.push_back(Trim(inner.substr(i, end - i)));
                if (comma == std::string::npos)
                    break;
                i = comma + 1;
            }
            return p;
        }();
        if (parts.size() < 3 || parts.size() > 4)
            return false;
        double nums[4] = { 0.0, 0.0, 0.0, 1.0 };
        for (size_t i = 0; i < parts.size(); ++i) {
            if (!ParseNumber(parts[i], nums[i]))
                return false;
        }
        out.x = static_cast<float>(nums[0]) / 255.0f;
        out.y = static_cast<float>(nums[1]) / 255.0f;
        out.z = static_cast<float>(nums[2]) / 255.0f;
        // alpha may be 0..1 or 0..255
        out.w = nums[3] <= 1.0 ? static_cast<float>(nums[3]) : static_cast<float>(nums[3]) / 255.0f;
        return out.x >= 0.0f && out.y >= 0.0f && out.z >= 0.0f;
    }

    return ParseNamedColor(v, out);
}

bool Sheet::ParseLength(const std::string& value, float& out) {
    const std::string v = Trim(value);
    if (v.empty())
        return false;
    char* end = nullptr;
    const double raw = std::strtod(v.c_str(), &end);
    if (end == v.c_str())
        return false;
    while (*end == ' ' || *end == '\t')
        ++end;
    if (*end == '%') {
        out = static_cast<float>(raw) / 100.0f;
        return true;
    }
    // "px" is the default unit; any other trailing junk is ignored.
    out = static_cast<float>(raw);
    return true;
}

const char* Sheet::DefaultUss() {
    static const char* kUss = R"USS(/* main.uss - edit this sheet and press Reload. */
.card {
    -unity-background-color: #232833;
    border-color: #3A4152;
    border-width: 1px;
    border-radius: 6px;
    padding: 10px 14px;
}

.card:hover {
    background-color: #2C3446;
    border-color: #4D79FF;
}

#title {
    -unity-text-color: #EEF2FF;
    font-size: 18px;
    -unity-font-style: bold;
}

button.primary {
    background-color: rgb(66, 120, 245);
    -unity-text-color: white;
    border-radius: 4px;
    padding: 6px 12px;
    flex-grow: 1;
}

.toolbar {
    -unity-background-color: #1B1F28;
    flex-direction: row;
    align-items: center;
    padding: 4px 8px;
    border-width: 1px;
    border-color: #2F3644;
}

label {
    color: #C9D1E0;
    font-size: 13px;
}

.danger {
    background-color: rgba(196, 60, 52, 0.9);
    border-color: red;
    color: white;
}
)USS";
    return kUss;
}

// ---------------------------------------------------------------------------
// StyleRule
// ---------------------------------------------------------------------------
bool StyleRule::Matches(const std::string& tag, const std::string& id,
                        const std::vector<std::string>& classes, bool hovered) const {
    // Grammar: tag? (#id)? (.class)* with an optional ":hover" pseudo class.
    std::string sel = ToLowerAscii(Trim(selector));
    if (sel.empty())
        return false;

    bool hoverRequired = false;
    const size_t colon = sel.find(':');
    if (colon != std::string::npos) {
        std::string pseudo = sel.substr(colon + 1);
        const size_t next = pseudo.find_first_of(".:#");
        if (next != std::string::npos)
            pseudo = pseudo.substr(0, next);
        if (ToLowerAscii(Trim(pseudo)) != "hover")
            return false;    // unknown pseudo classes never match
        hoverRequired = true;
        sel = Trim(sel.substr(0, colon));
    }
    if (hoverRequired && !hovered)
        return false;

    const std::string wantTag = ToLowerAscii(Trim(tag));
    const std::string wantId = ToLowerAscii(Trim(id));

    size_t p = 0;
    // optional tag
    const size_t tagStart = p;
    while (p < sel.size() && (std::isalnum(static_cast<unsigned char>(sel[p])) || sel[p] == '-' || sel[p] == '_'))
        ++p;
    const std::string ruleTag = sel.substr(tagStart, p - tagStart);
    if (!ruleTag.empty() && ruleTag != wantTag)
        return false;

    while (p < sel.size()) {
        if (sel[p] == '#') {
            ++p;
            const size_t start = p;
            while (p < sel.size() && sel[p] != '.' && sel[p] != '#' && sel[p] != ':' && !IsSpaceChar(sel[p]))
                ++p;
            if (sel.substr(start, p - start) != wantId || wantId.empty())
                return false;
        } else if (sel[p] == '.') {
            ++p;
            const size_t start = p;
            while (p < sel.size() && sel[p] != '.' && sel[p] != '#' && sel[p] != ':' && !IsSpaceChar(sel[p]))
                ++p;
            const std::string want = sel.substr(start, p - start);
            if (want.empty())
                return false;
            bool found = false;
            for (const std::string& c : classes)
                if (ToLowerAscii(c) == want) {
                    found = true;
                    break;
                }
            if (!found)
                return false;
        } else {
            return false;    // unsupported combinator (descendant etc.)
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// ResolvedStyle
// ---------------------------------------------------------------------------
std::string ResolvedStyle::Describe() const {
    std::string out;
    char buf[96];
    auto addPart = [&out](const std::string& part) {
        if (!out.empty())
            out += " | ";
        out += part;
    };

    if (hasBackground)
        addPart("bg " + ColorHex(background));
    if (hasColor)
        addPart("color " + ColorHex(color));
    if (hasBorder) {
        std::snprintf(buf, sizeof(buf), "%.1fpx", static_cast<double>(borderWidth));
        addPart(std::string("border ") + buf + " " + ColorHex(borderColor));
    }
    if (paddingTop != 0.0f || paddingRight != 0.0f || paddingBottom != 0.0f || paddingLeft != 0.0f) {
        std::snprintf(buf, sizeof(buf), "%g/%g/%g/%g", static_cast<double>(paddingTop),
                      static_cast<double>(paddingRight), static_cast<double>(paddingBottom),
                      static_cast<double>(paddingLeft));
        addPart(std::string("padding ") + buf);
    }
    if (marginTop != 0.0f || marginRight != 0.0f || marginBottom != 0.0f || marginLeft != 0.0f) {
        std::snprintf(buf, sizeof(buf), "%g/%g/%g/%g", static_cast<double>(marginTop),
                      static_cast<double>(marginRight), static_cast<double>(marginBottom),
                      static_cast<double>(marginLeft));
        addPart(std::string("margin ") + buf);
    }
    if (borderRadius != 0.0f) {
        std::snprintf(buf, sizeof(buf), "radius %g", static_cast<double>(borderRadius));
        addPart(buf);
    }
    if (width != 0.0f) {
        std::snprintf(buf, sizeof(buf), "width %g", static_cast<double>(width));
        addPart(buf);
    }
    if (height != 0.0f) {
        std::snprintf(buf, sizeof(buf), "height %g", static_cast<double>(height));
        addPart(buf);
    }
    if (fontSize != 0.0f) {
        std::snprintf(buf, sizeof(buf), "font-size %g", static_cast<double>(fontSize));
        addPart(buf);
    }
    if (flexGrow != 0.0f) {
        std::snprintf(buf, sizeof(buf), "flex-grow %g", static_cast<double>(flexGrow));
        addPart(buf);
    }
    if (flexDirection >= 0)
        addPart(std::string("flex-direction ") + (flexDirection == 1 ? "column" : "row"));
    if (alignItems >= 0) {
        static const char* const kAlign[4] = { "flex-start", "center", "flex-end", "stretch" };
        addPart(std::string("align-items ") + kAlign[alignItems >= 0 && alignItems < 4 ? alignItems : 0]);
    }
    if (bold || italic)
        addPart(std::string("style ") + (bold && italic ? "bold-and-italic" : (bold ? "bold" : "italic")));
    if (out.empty())
        out = "(no matching rules)";
    return out;
}

} // namespace demo
} // namespace uilab
