// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "slate/core/USS.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <sstream>

namespace quantum::slate {
namespace {

std::string trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    return value;
}

bool number(const std::string& text, float& result) {
    char* end = nullptr;
    result = std::strtof(text.c_str(), &end);
    return end != text.c_str() && *end == '\0';
}

bool color(const std::string& text, Vector4& result) {
    if ((text.size() != 7 && text.size() != 9) || text[0] != '#') return false;
    if (text[0] != '#') return false;
    const char* begin = text.c_str() + 1;
    char* end = nullptr;
    const unsigned long value = std::strtoul(begin, &end, 16);
    if (*end != '\0') return false;
    if (text.size() == 7) {
        result = Vector4(((value >> 16) & 0xff) / 255.0f, ((value >> 8) & 0xff) / 255.0f,
            (value & 0xff) / 255.0f, 1.0f);
    } else {
        result = Vector4(((value >> 24) & 0xff) / 255.0f, ((value >> 16) & 0xff) / 255.0f,
            ((value >> 8) & 0xff) / 255.0f, (value & 0xff) / 255.0f);
    }
    return true;
}

bool setProperty(WidgetStyle& style, const std::string& property, const std::string& raw) {
    const std::string value = trim(raw);
    Vector4 parsedColor;
    float n = 0.0f;
    if (property == "background-color" && color(value, parsedColor)) style.backgroundColor = parsedColor;
    else if ((property == "color" || property == "text-color") && color(value, parsedColor)) style.textColor = parsedColor;
    else if (property == "border-color" && color(value, parsedColor)) style.borderColor = parsedColor;
    else if (property == "border-width" && number(value, n)) style.borderWidth = std::max(0.0f, n);
    else if (property == "border-radius" && number(value, n)) style.borderRadius = std::max(0.0f, n);
    else if (property == "opacity" && number(value, n)) style.opacity = std::clamp(n, 0.0f, 1.0f);
    else if (property == "font-size" && number(value, n)) style.fontSize = static_cast<unsigned int>(std::max(1.0f, n));
    else if (property == "font-family") style.fontFamily = value;
    else if (property == "background" || property == "background-brush") style.backgroundBrush = value;
    else if (property == "border-brush") style.borderBrush = value;
    else if (property == "min-width" && number(value, n)) style.minSize.x = std::max(0.0f, n);
    else if (property == "min-height" && number(value, n)) style.minSize.y = std::max(0.0f, n);
    else if (property == "max-width" && number(value, n)) style.maxSize.x = std::max(0.0f, n);
    else if (property == "max-height" && number(value, n)) style.maxSize.y = std::max(0.0f, n);
    else if (property == "text-align") {
        if (value == "center") style.textAlign = TextAlign::Center;
        else if (value == "right" || value == "far") style.textAlign = TextAlign::Far;
        else if (value == "left" || value == "near") style.textAlign = TextAlign::Near;
        else return false;
    } else if (property == "text-overflow") {
        if (value == "ellipsis") style.overflow = TextOverflow::Ellipsis;
        else if (value == "clip") style.overflow = TextOverflow::Clip;
        else return false;
    } else if (property == "padding") {
        std::istringstream stream(value);
        std::vector<float> values;
        while (stream >> n) values.push_back(n);
        if (values.size() == 1) style.padding = Insets(values[0]);
        else if (values.size() == 2) style.padding = Insets(values[0], values[1], values[0], values[1]);
        else if (values.size() == 4) style.padding = Insets(values[0], values[1], values[2], values[3]);
        else return false;
    } else return false;
    return true;
}

std::string expandVariables(const std::string& raw, const std::map<std::string, std::string>& variables,
                            bool& complete) {
    std::string result = raw;
    size_t cursor = 0;
    while ((cursor = result.find("var(", cursor)) != std::string::npos) {
        const size_t end = result.find(')', cursor + 4);
        if (end == std::string::npos) {
            complete = false;
            return result;
        }
        const std::string key = trim(result.substr(cursor + 4, end - cursor - 4));
        auto it = variables.find(key);
        if (it == variables.end()) {
            complete = false;
            return result;
        }
        result.replace(cursor, end - cursor + 1, it->second);
        cursor += it->second.size();
    }
    return result;
}

}

bool USSParser::parse(const std::string& source, StyleSet& styles,
                      std::vector<USSDiagnostic>* diagnostics) {
    bool success = true;
    std::map<std::string, std::string> variables;

    //Collect tokens first so a theme can reference a variable declared later.
    size_t variableCursor = 0;
    while (variableCursor < source.size()) {
        const size_t open = source.find('{', variableCursor);
        if (open == std::string::npos) break;
        const size_t close = source.find('}', open + 1);
        if (close == std::string::npos) break;
        std::istringstream declarations(source.substr(open + 1, close - open - 1));
        std::string declaration;
        while (std::getline(declarations, declaration, ';')) {
            const size_t colon = declaration.find(':');
            if (colon == std::string::npos) continue;
            const std::string property = trim(declaration.substr(0, colon));
            if (property.rfind("--", 0) == 0)
                variables[property] = trim(declaration.substr(colon + 1));
        }
        variableCursor = close + 1;
    }

    size_t cursor = 0;
    int line = 1;
    while (cursor < source.size()) {
        const size_t open = source.find('{', cursor);
        if (open == std::string::npos) break;
        const size_t close = source.find('}', open + 1);
        if (close == std::string::npos) {
            if (diagnostics) diagnostics->push_back({line, "missing '}'"});
            return false;
        }
        const std::string selectorText = trim(source.substr(cursor, open - cursor));
        const std::string body = source.substr(open + 1, close - open - 1);
        std::vector<std::pair<std::string, std::string>> properties;
        std::istringstream declarations(body);
        std::string declaration;
        while (std::getline(declarations, declaration, ';')) {
            const size_t colon = declaration.find(':');
            if (colon == std::string::npos) continue;
            properties.emplace_back(trim(declaration.substr(0, colon)), trim(declaration.substr(colon + 1)));
        }
        std::istringstream selectors(selectorText);
        std::string selector;
        while (std::getline(selectors, selector, ',')) {
            selector = trim(selector);
            if (selector.empty() || (selector != ":root" && selector.find(' ') != std::string::npos)) {
                success = false;
                if (diagnostics) diagnostics->push_back({line, "unsupported selector: " + selector});
                continue;
            }
            const std::string styleName = "__uss_" + selector;
            NamedStyle& style = styles.defineStyle(styleName);
            for (const auto& property : properties) {
                if (property.first.rfind("--", 0) == 0) continue;
                bool complete = true;
                const std::string value = expandVariables(property.second, variables, complete);
                if (!complete || !setProperty(style.base(), property.first, value)) {
                    success = false;
                    if (diagnostics) diagnostics->push_back({line, complete ?
                        "unsupported property: " + property.first :
                        "unknown USS variable in property: " + property.first});
                }
            }
            if (selector != ":root") styles.registerRule(selector, styleName);
        }
        line += static_cast<int>(std::count(source.begin() + static_cast<std::ptrdiff_t>(cursor),
            source.begin() + static_cast<std::ptrdiff_t>(close + 1), '\n'));
        cursor = close + 1;
    }
    return success;
}

}
