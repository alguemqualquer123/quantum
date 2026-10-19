#include "core/Validation.h"

#include <cctype>
#include <cstdlib>

namespace uilab {

namespace {
const char* kRules[] = { "none", "nonempty", "integer", "float", "minlength",
                         "maxlength", "pattern", "identifier", "range" };

bool IsDigits(const std::string& s) {
    if (s.empty())
        return false;
    for (char c : s)
        if (!std::isdigit(static_cast<unsigned char>(c)))
            return false;
    return true;
}

bool MatchClass(const char*& p, char c) {
    // p points at '['; advance past the class.
    bool negate = false;
    ++p;
    if (*p == '!' || *p == '^') {
        negate = true;
        ++p;
    }
    bool matched = false;
    char prev = 0;
    bool first = true;
    while (*p && (*p != ']' || first)) {
        first = false;
        if (*p == '-' && prev != 0 && *(p + 1) && *(p + 1) != ']') {
            const char hi = *(p + 1);
            if (c >= prev && c <= hi)
                matched = true;
            prev = 0;
            p += 2;
            continue;
        }
        if (*p == c)
            matched = true;
        prev = *p;
        ++p;
    }
    if (*p == ']')
        ++p;
    return negate ? !matched : matched;
}

bool MatchImpl(const char* pattern, const char* text) {
    while (*pattern) {
        if (*pattern == '*') {
            while (*pattern == '*')
                ++pattern;
            if (!*pattern)
                return true;
            for (const char* t = text; ; ++t) {
                if (MatchImpl(pattern, t))
                    return true;
                if (!*t)
                    return false;
            }
        }
        if (!*text)
            return false;
        if (*pattern == '?') {
            ++pattern;
            ++text;
            continue;
        }
        if (*pattern == '[') {
            const char* classStart = pattern;
            if (!MatchClass(pattern, *text))
                return false;
            if (pattern == classStart) // malformed class, treat '[' literally
                ++pattern;
            ++text;
            continue;
        }
        if (*pattern != *text)
            return false;
        ++pattern;
        ++text;
    }
    return *text == '\0';
}
} // namespace

bool MatchPattern(const std::string& pattern, const std::string& text) {
    if (pattern.empty())
        return text.empty();
    return MatchImpl(pattern.c_str(), text.c_str());
}

bool ValidateText(const std::string& value, const std::string& rule, std::string& errorOut) {
    return ValidateText(value, rule, std::string(), errorOut);
}

bool ValidateText(const std::string& value, const std::string& rule, const std::string& pattern,
                  std::string& errorOut) {
    errorOut.clear();
    if (rule.empty() || rule == "none")
        return true;

    if (rule == "nonempty") {
        for (char c : value)
            if (!std::isspace(static_cast<unsigned char>(c)))
                return true;
        errorOut = "Value must not be empty.";
        return false;
    }
    if (rule == "integer") {
        std::string body = value;
        size_t i = 0;
        if (!body.empty() && (body[0] == '-' || body[0] == '+'))
            i = 1;
        if (IsDigits(body.substr(i)))
            return true;
        errorOut = "Value must be an integer (digits only).";
        return false;
    }
    if (rule == "float") {
        if (value.empty()) {
            errorOut = "Value must be a number.";
            return false;
        }
        std::string normalized = value;
        for (char& c : normalized)
            if (c == ',')
                c = '.';
        char* end = nullptr;
        std::strtod(normalized.c_str(), &end);
        if (end && *end == '\0') {
            for (char c : value)
                if (!std::isdigit(static_cast<unsigned char>(c)) && c != '.' && c != ',' &&
                    c != '-' && c != '+') {
                    errorOut = "Value must be a number.";
                    return false;
                }
            return true;
        }
        errorOut = "Value must be a number (example: 12.5).";
        return false;
    }
    if (rule == "minlength") {
        if (value.size() >= 4)
            return true;
        errorOut = "Use at least 4 characters.";
        return false;
    }
    if (rule == "maxlength") {
        if (value.size() <= 24)
            return true;
        errorOut = "Use at most 24 characters.";
        return false;
    }
    if (rule == "pattern") {
        if (pattern.empty() || MatchPattern(pattern, value))
            return true;
        errorOut = "Value must match the pattern \"" + pattern + "\".";
        return false;
    }
    if (rule == "identifier") {
        if (value.empty()) {
            errorOut = "Identifier must not be empty.";
            return false;
        }
        const char first = value[0];
        if (!(std::isalpha(static_cast<unsigned char>(first)) || first == '_')) {
            errorOut = "Identifier must start with a letter or '_'.";
            return false;
        }
        for (char c : value)
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) {
                errorOut = "Identifier allows letters, digits and '_' only.";
                return false;
            }
        return true;
    }
    if (rule == "range") {
        return ValidateFloatRange(static_cast<float>(std::atof(value.c_str())), 0.0f, 100.0f, errorOut);
    }
    errorOut = "Unknown validation rule '" + rule + "'.";
    return false;
}

const char* ValidationRuleHint(const std::string& rule) {
    if (rule == "nonempty") return "At least one non-space character (assets need a name).";
    if (rule == "integer")  return "Digits with an optional leading sign.";
    if (rule == "float")    return "Number with an optional decimal part.";
    if (rule == "minlength") return "Minimum of 4 characters.";
    if (rule == "maxlength") return "Maximum of 24 characters.";
    if (rule == "pattern")   return "Must match the wildcard pattern (example: BP_[A-Za-z0-9_]*).";
    if (rule == "identifier") return "C++/Blueprint style identifier.";
    if (rule == "range")     return "Value must be inside the configured range.";
    return "No restriction.";
}

const char* const* ValidationRules(int& countOut) {
    countOut = static_cast<int>(sizeof(kRules) / sizeof(kRules[0]));
    return kRules;
}

bool ValidateFloatRange(float value, float minValue, float maxValue, std::string& errorOut) {
    if (minValue > maxValue)
        std::swap(minValue, maxValue);
    if (value < minValue || value > maxValue) {
        errorOut = "Value must be between " + std::to_string(minValue) + " and " + std::to_string(maxValue) + ".";
        return false;
    }
    errorOut.clear();
    return true;
}

bool ValidateIntRange(int value, int minValue, int maxValue, std::string& errorOut) {
    if (minValue > maxValue)
        std::swap(minValue, maxValue);
    if (value < minValue || value > maxValue) {
        errorOut = "Value must be between " + std::to_string(minValue) + " and " + std::to_string(maxValue) + ".";
        return false;
    }
    errorOut.clear();
    return true;
}

} // namespace uilab
