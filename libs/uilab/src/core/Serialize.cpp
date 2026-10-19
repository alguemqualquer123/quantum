#include "core/Serialize.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#endif

namespace uilab {

namespace {
std::string Trim(const std::string& s) {
    const char* ws = " \t\r\n";
    const size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos)
        return std::string();
    const size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

// Values are written one per line, so embedded newlines (multiline text fields)
// and backslashes are escaped on write and restored on read.
std::string EscapeValue(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c; break;
        }
    }
    return out;
}

std::string UnescapeValue(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 == s.size()) {
            out += s[i];
            continue;
        }
        const char next = s[++i];
        switch (next) {
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case '\\': out += '\\'; break;
        default: out += '\\'; out += next; break;
        }
    }
    return out;
}

std::string JoinVec4(const ImVec4& v) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%.6g,%.6g,%.6g,%.6g",
                  static_cast<double>(v.x), static_cast<double>(v.y),
                  static_cast<double>(v.z), static_cast<double>(v.w));
    return std::string(buf);
}

std::vector<float> SplitFloats(const std::string& s) {
    std::vector<float> out;
    std::stringstream ss(s);
    std::string token;
    while (std::getline(ss, token, ',')) {
        token = Trim(token);
        if (token.empty())
            continue;
        out.push_back(static_cast<float>(std::strtod(token.c_str(), nullptr)));
    }
    return out;
}
} // namespace

void KeyValueDoc::Set(const std::string& key, const std::string& value) {
    for (auto& kv : items_) {
        if (kv.first == key) {
            kv.second = value;
            return;
        }
    }
    items_.emplace_back(key, value);
}

void KeyValueDoc::SetFloat(const std::string& key, float value) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6g", static_cast<double>(value));
    Set(key, buf);
}

void KeyValueDoc::SetInt(const std::string& key, int value) {
    Set(key, std::to_string(value));
}

void KeyValueDoc::SetBool(const std::string& key, bool value) {
    Set(key, value ? "1" : "0");
}

void KeyValueDoc::SetVec2(const std::string& key, const ImVec2& v) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%.6g,%.6g", static_cast<double>(v.x), static_cast<double>(v.y));
    Set(key, buf);
}

void KeyValueDoc::SetVec4(const std::string& key, const ImVec4& v) { Set(key, JoinVec4(v)); }

bool KeyValueDoc::Has(const std::string& key) const {
    for (const auto& kv : items_)
        if (kv.first == key)
            return true;
    return false;
}

std::string KeyValueDoc::Get(const std::string& key, const std::string& def) const {
    for (const auto& kv : items_)
        if (kv.first == key)
            return kv.second;
    return def;
}

float KeyValueDoc::GetFloat(const std::string& key, float def) const {
    if (!Has(key))
        return def;
    return static_cast<float>(std::strtod(Get(key).c_str(), nullptr));
}

int KeyValueDoc::GetInt(const std::string& key, int def) const {
    if (!Has(key))
        return def;
    return static_cast<int>(std::strtol(Get(key).c_str(), nullptr, 10));
}

bool KeyValueDoc::GetBool(const std::string& key, bool def) const {
    if (!Has(key))
        return def;
    const std::string v = Get(key);
    return v == "1" || v == "true" || v == "True";
}

ImVec2 KeyValueDoc::GetVec2(const std::string& key, const ImVec2& def) const {
    const std::vector<float> v = SplitFloats(Get(key));
    if (v.size() < 2)
        return def;
    return ImVec2(v[0], v[1]);
}

ImVec4 KeyValueDoc::GetVec4(const std::string& key, const ImVec4& def) const {
    const std::vector<float> v = SplitFloats(Get(key));
    if (v.size() < 4)
        return def;
    return ImVec4(v[0], v[1], v[2], v[3]);
}

std::string KeyValueDoc::ToText(const std::string& header) const {
    std::ostringstream os;
    if (!header.empty())
        os << "# " << header << "\n";
    for (const auto& kv : items_)
        os << kv.first << " = " << EscapeValue(kv.second) << "\n";
    return os.str();
}

bool KeyValueDoc::Parse(const std::string& text, std::string& errorOut) {
    std::istringstream is(text);
    std::string line;
    int lineNo = 0;
    while (std::getline(is, line)) {
        ++lineNo;
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            errorOut = "line " + std::to_string(lineNo) + ": missing '=' in '" + line + "'";
            return false;
        }
        const std::string key = Trim(line.substr(0, eq));
        const std::string value = UnescapeValue(Trim(line.substr(eq + 1)));
        if (key.empty()) {
            errorOut = "line " + std::to_string(lineNo) + ": empty key";
            return false;
        }
        Set(key, value);
    }
    errorOut.clear();
    return true;
}

bool KeyValueDoc::SaveToFile(const std::string& path, const std::string& header, std::string& errorOut) const {
    return WriteTextFile(path, ToText(header), errorOut);
}

bool KeyValueDoc::LoadFromFile(const std::string& path, std::string& errorOut) {
    std::string text;
    if (!ReadTextFile(path, text, errorOut))
        return false;
    Clear();
    return Parse(text, errorOut);
}

bool ReadTextFile(const std::string& path, std::string& out, std::string& errorOut) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        errorOut = "cannot open '" + path + "' for reading";
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    errorOut.clear();
    return true;
}

bool EnsureParentDirectory(const std::string& path, std::string& errorOut) {
    const size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos)
        return true;
    const std::string dir = path.substr(0, slash);
    if (dir.empty())
        return true;
#if defined(_WIN32)
    const int rc = _mkdir(dir.c_str());
#else
    const int rc = mkdir(dir.c_str(), 0755);
#endif
    if (rc != 0 && errno != EEXIST) {
        errorOut = "cannot create directory '" + dir + "'";
        return false;
    }
    return true;
}

bool WriteTextFile(const std::string& path, const std::string& text, std::string& errorOut) {
    if (!EnsureParentDirectory(path, errorOut))
        return false;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        errorOut = "cannot open '" + path + "' for writing";
        return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) {
        errorOut = "write error on '" + path + "'";
        return false;
    }
    errorOut.clear();
    return true;
}

} // namespace uilab
