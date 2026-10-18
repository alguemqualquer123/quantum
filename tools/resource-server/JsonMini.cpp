// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#include "JsonMini.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace doriax::rs {

uint64_t JsonField::asUInt() const {
    if (isNumber) return number >= 0.0 ? static_cast<uint64_t>(number) : 0;
    return static_cast<uint64_t>(strtoull(text.c_str(), nullptr, 10));
}

bool JsonField::truthy() const {
    if (isNumber) return number != 0.0;
    return text == "true";
}

namespace {

void skipWs(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n'))
        ++i;
}

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) out.push_back(char(cp));
    else if (cp < 0x800) {
        out.push_back(char(0xC0 | (cp >> 6)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(char(0xE0 | (cp >> 12)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(char(0xF0 | (cp >> 18)));
        out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    }
}

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// i points at the opening quote. On success i is after the closing quote.
bool parseString(const std::string& s, size_t& i, std::string& out, std::string& err) {
    out.clear();
    ++i; // consume opening quote
    while (i < s.size()) {
        char c = s[i++];
        if (c == '"') return true;
        if (c != '\\') { out.push_back(c); continue; }
        if (i >= s.size()) break;
        char e = s[i++];
        switch (e) {
            case '"':  out.push_back('"');  break;
            case '\\': out.push_back('\\'); break;
            case '/':  out.push_back('/');  break;
            case 'b':  out.push_back('\b'); break;
            case 'f':  out.push_back('\f'); break;
            case 'n':  out.push_back('\n'); break;
            case 'r':  out.push_back('\r'); break;
            case 't':  out.push_back('\t'); break;
            case 'u': {
                if (i + 4 > s.size()) { err = "truncated \\u escape"; return false; }
                uint32_t cp = 0;
                for (int k = 0; k < 4; ++k) {
                    int v = hexVal(s[i + k]);
                    if (v < 0) { err = "bad \\u escape"; return false; }
                    cp = (cp << 4) | uint32_t(v);
                }
                i += 4;
                // Pair surrogates when the low one follows.
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= s.size() &&
                    s[i] == '\\' && s[i + 1] == 'u') {
                    uint32_t lo = 0;
                    bool ok = true;
                    for (int k = 0; k < 4; ++k) {
                        int v = hexVal(s[i + 2 + k]);
                        if (v < 0) { ok = false; break; }
                        lo = (lo << 4) | uint32_t(v);
                    }
                    if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        i += 6;
                    }
                }
                appendUtf8(out, cp);
                break;
            }
            default: err = std::string("bad escape \\") + e; return false;
        }
    }
    err = "unterminated string";
    return false;
}

// i points at '{' or '['. On return i is after the matching closer. Tracks
// strings so braces inside literals are ignored.
bool skipBalanced(const std::string& s, size_t& i, std::string& err) {
    char open = s[i];
    char close = (open == '{') ? '}' : ']';
    int depth = 0;
    while (i < s.size()) {
        char c = s[i];
        if (c == '"') {
            std::string tmp;
            if (!parseString(s, i, tmp, err)) return false; // i advanced past quote
            continue;
        }
        if (c == open) ++depth;
        else if (c == close) {
            --depth;
            if (depth == 0) { ++i; return true; }
        }
        ++i;
    }
    err = "unbalanced nested value";
    return false;
}

}

bool parseFlatJsonObject(const std::string& text, std::map<std::string, JsonField>& out,
                         std::string& errorOut) {
    out.clear();
    size_t i = 0;
    const std::string& s = text;
    skipWs(s, i);
    if (i >= s.size() || s[i] != '{') { errorOut = "expected a JSON object"; return false; }
    ++i;
    skipWs(s, i);
    if (i < s.size() && s[i] == '}') return true;

    for (;;) {
        skipWs(s, i);
        if (i >= s.size() || s[i] != '"') { errorOut = "expected a member name"; return false; }
        std::string key;
        if (!parseString(s, i, key, errorOut)) return false;
        skipWs(s, i);
        if (i >= s.size() || s[i] != ':') { errorOut = "expected ':' after member name"; return false; }
        ++i;
        skipWs(s, i);
        if (i >= s.size()) { errorOut = "missing member value"; return false; }

        JsonField field;
        char c = s[i];
        if (c == '"') {
            if (!parseString(s, i, field.text, errorOut)) return false;
        } else if (c == '{' || c == '[') {
            if (!skipBalanced(s, i, errorOut)) return false;
            // Nested values are skipped; record the empty field so members
            // like "filters": {...} do not break parsing.
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            size_t start = i;
            while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) ||
                   s[i] == '-' || s[i] == '+' || s[i] == '.' || s[i] == 'e' || s[i] == 'E'))
                ++i;
            field.text = s.substr(start, i - start);
            field.number = strtod(field.text.c_str(), nullptr);
            field.isNumber = true;
        } else if (s.compare(i, 4, "true") == 0) { field.text = "true"; i += 4; }
        else if (s.compare(i, 5, "false") == 0)   { field.text = "false"; i += 5; }
        else if (s.compare(i, 4, "null") == 0)    { field.text = "null"; i += 4; }
        else { errorOut = std::string("unexpected value character '") + c + "'"; return false; }
        out[key] = field;

        skipWs(s, i);
        if (i >= s.size()) { errorOut = "unterminated JSON object"; return false; }
        if (s[i] == ',') { ++i; continue; }
        if (s[i] == '}') return true;
        errorOut = "expected ',' or '}'";
        return false;
    }
}

std::string jsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    static const char* hex = "0123456789abcdef";
    for (unsigned char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out.push_back(hex[(c >> 4) & 0xF]);
                    out.push_back(hex[c & 0xF]);
                } else {
                    out.push_back(char(c));
                }
        }
    }
    return out;
}

void jsonPutString(std::string& out, const std::string& key, const std::string& value) {
    if (!out.empty()) out += ',';
    out += '"' + jsonEscape(key) + "\":\"" + jsonEscape(value) + '"';
}

void jsonPutNumber(std::string& out, const std::string& key, double value) {
    if (!out.empty()) out += ',';
    out += '"' + jsonEscape(key) + "\":";
    // Integral formatting keeps the protocol readable; %g for the rest.
    if (value == static_cast<long long>(value)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value));
        out += buf;
    } else {
        char buf[32];
        snprintf(buf, sizeof(buf), "%g", value);
        out += buf;
    }
}

}
