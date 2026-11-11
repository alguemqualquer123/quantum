// quantum-reflector: file helpers + regex utilities.
#include "reflector.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace refl {

bool readFile(const std::string& path, std::string& out, std::string& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "cannot open file: " + path;
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    // Normalize CRLF so parsers see the same text Python's universal newlines give.
    std::string fixed;
    fixed.reserve(out.size());
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '\r' && i + 1 < out.size() && out[i + 1] == '\n')
            continue;
        fixed.push_back(out[i]);
    }
    out.swap(fixed);
    return true;
}

std::vector<std::string> listBindingCppFiles(const std::string& dir) {
    std::vector<std::string> files;
    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    if (ec) return files;
    for (const auto& e : it) {
        if (!e.is_regular_file(ec)) continue;
        if (e.path().extension() == ".cpp") {
            fs::path abs = fs::absolute(e.path(), ec);
            files.push_back((ec ? e.path() : abs).generic_string());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::vector<std::string> listHeadersRecursive(const std::string& dir) {
    std::vector<std::string> files;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec) && it->path().extension() == ".h") {
            files.push_back(it->path().generic_string());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::string stripComments(const std::string& content) {
    // Pass 1 (matches re.sub(r'//.*', '', content)): truncate each line at
    // its first '//' occurrence.
    std::string out;
    out.reserve(content.size());
    size_t pos = 0;
    while (pos <= content.size()) {
        size_t nl = content.find('\n', pos);
        std::string line = (nl == std::string::npos)
                               ? content.substr(pos)
                               : content.substr(pos, nl - pos);
        size_t slash = line.find("//");
        if (slash != std::string::npos) line.erase(slash);
        out += line;
        if (nl == std::string::npos) break;
        out += '\n';
        pos = nl + 1;
    }
    // Pass 2 (matches re.sub(r'/\*.*?\*/', '', content, flags=DOTALL)):
    // remove each /* ... */ span; leave an unterminated /* alone.
    std::string res;
    res.reserve(out.size());
    for (size_t i = 0; i < out.size();) {
        if (i + 1 < out.size() && out[i] == '/' && out[i + 1] == '*') {
            size_t end = out.find("*/", i + 2);
            if (end != std::string::npos) {
                i = end + 2;
                continue;
            }
        }
        res.push_back(out[i++]);
    }
    return res;
}

const char* kindName(SymKind kind) {
    switch (kind) {
        case SymKind::Enum: return "Enum";
        case SymKind::EnumMember: return "EnumMember";
        case SymKind::Class: return "Class";
        case SymKind::Constructor: return "Constructor";
        case SymKind::Method: return "Method";
        case SymKind::StaticMethod: return "StaticMethod";
        case SymKind::Property: return "Property";
        case SymKind::Constant: return "Constant";
        case SymKind::Event: return "Event";
        case SymKind::CppMethod: return "CppMethod";
    }
    return "Variable";
}

const char* suggestionKind(SymKind kind) {
    switch (kind) {
        case SymKind::Class:
        case SymKind::Constructor: return "Class";
        case SymKind::Enum: return "Enum";
        case SymKind::EnumMember: return "EnumMember";
        case SymKind::Method: return "Method";
        case SymKind::CppMethod: return "CppMethod";
        case SymKind::StaticMethod: return "Function";
        case SymKind::Property:
        case SymKind::Event: return "Property";
        case SymKind::Constant: return "Constant";
    }
    return "Variable";
}

std::string escapeCppString(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else out.push_back(c);
    }
    return out;
}

int argCountFromDetail(const std::string& detail) {
    size_t open = detail.find('(');
    if (open == std::string::npos) return 0;
    int depth = 0;
    int commas = 0;
    bool nonEmpty = false;
    for (size_t i = open; i < detail.size(); ++i) {
        const char c = detail[i];
        if (c == '(') {
            ++depth;
            if (depth != 1) continue;
        } else if (c == ')') {
            --depth;
            if (depth == 0) break;
        } else if (depth == 1) {
            if (c == ',') ++commas;
            else if (c != ' ' && c != '\t') nonEmpty = true;
        }
    }
    if (depth != 0) return 0; // unbalanced: not parseable
    return nonEmpty ? commas + 1 : 0;
}

} // namespace refl
