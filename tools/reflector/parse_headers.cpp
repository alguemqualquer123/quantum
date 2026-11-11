// quantum-reflector: C++ header parser (public method and field extraction),
// mirroring parse_cpp_headers() and the _simplify_* helpers of
// generate_api_suggestions.py.
#include "reflector.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace refl {
namespace {

const std::regex& reSigPattern() {
    static const std::regex re(
        R"RE((?:virtual\s+|inline\s+|static\s+)*([A-Za-z0-9_:<>&*\s]+)\s+([A-Za-z0-9_]+)\s*\(([^)]*)\)\s*(?:const)?\s*(?:override|final)?\s*[;{])RE");
    return re;
}

const std::regex& reClassStart() {
    static const std::regex re(
        R"RE((class|struct)\s+(?:[A-Z0-9_]+\s+)?([A-Za-z0-9_]+)\s*(?::[^{]+)?\{)RE");
    return re;
}

const std::regex& reAccessLabel() {
    static const std::regex re(R"RE(\b(public|protected|private)\s*:)RE");
    return re;
}

const std::set<std::pair<std::string, std::string>>& excludedHeaderMethods() {
    static const std::set<std::pair<std::string, std::string>> k = {
        {"EntityHandle", "addComponent"},
        {"EntityHandle", "removeComponent"},
        {"EntityHandle", "getComponent"},
        {"EntityRegistry", "addComponent"},
        {"EntityRegistry", "removeComponent"},
        {"EntityRegistry", "getComponent"},
        {"EntityRegistry", "findComponent"},
        {"EntityRegistry", "findComponentFromIndex"},
        {"EntityRegistry", "getComponentFromIndex"},
        {"EntityRegistry", "getComponentId"},
        {"EntityRegistry", "getComponentArray"},
    };
    return k;
}

const std::set<std::string>& returnTypeReject() {
    static const std::set<std::string> k = {
        "return", "if", "else", "for", "while", "switch", "case", "do",
        "break", "continue", "goto", "using", "typedef", "friend", "template"};
    return k;
}

void replaceAll(std::string& s, const std::regex& re, const std::string& fmt) {
    s = std::regex_replace(s, re, fmt);
}

std::string collapseWhitespace(const std::string& s) {
    static const std::regex ws(R"RE(\s+)RE");
    std::string out = std::regex_replace(s, ws, " ");
    size_t b = out.find_first_not_of(" ");
    if (b == std::string::npos) return "";
    size_t e = out.find_last_not_of(" ");
    return out.substr(b, e - b + 1);
}

// Python _extract_class_body: brace-matched body starting at '{' pos.
std::string extractClassBody(const std::string& text, size_t bracePos) {
    int depth = 0;
    for (size_t i = bracePos; i < text.size(); ++i) {
        if (text[i] == '{') {
            ++depth;
        } else if (text[i] == '}') {
            --depth;
            if (depth == 0) return text.substr(bracePos + 1, i - bracePos - 1);
        }
    }
    return "";
}

bool isIdentChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

// Collects class-scope `Type name;` declarations from a public section. Only
// depth-0 statements are considered, so inline method bodies are ignored.
void collectPublicFields(const std::string& publicBody,
                         std::map<std::string, std::string>& fields) {
    static const std::regex specifiers(
        R"RE(\b(?:static|inline|constexpr|consteval|mutable|thread_local|register|volatile|extern|const|typename)\b)RE");

    auto flush = [&](std::string pending) {
        pending = collapseWhitespace(pending);
        if (pending.empty() || pending.back() != ';') return;
        pending.pop_back();
        pending = collapseWhitespace(pending);
        const size_t eq = pending.find('=');
        if (eq != std::string::npos)
            pending = collapseWhitespace(pending.substr(0, eq));
        while (!pending.empty() && pending.back() == ']') {
            const size_t open = pending.rfind('[');
            if (open == std::string::npos) break;
            pending = collapseWhitespace(pending.substr(0, open));
        }
        if (pending.find('(') != std::string::npos) return;
        pending = collapseWhitespace(std::regex_replace(pending, specifiers, " "));
        if (pending.empty()) return;

        // Split declarators at top-level commas so `float x, y;` yields both
        // fields; commas inside template arguments are kept.
        std::vector<std::string> declarators;
        std::string current;
        int angleDepth = 0;
        for (char c : pending) {
            if (c == '<') {
                ++angleDepth;
            } else if (c == '>') {
                angleDepth = angleDepth > 0 ? angleDepth - 1 : 0;
            } else if (c == ',' && angleDepth == 0) {
                declarators.push_back(collapseWhitespace(current));
                current.clear();
                continue;
            }
            current.push_back(c);
        }
        declarators.push_back(collapseWhitespace(current));

        std::string type;
        std::vector<std::string> names;
        for (size_t i = 0; i < declarators.size(); ++i) {
            std::string decl = declarators[i];
            while (!decl.empty() && decl.back() == ']') {
                const size_t open = decl.rfind('[');
                if (open == std::string::npos) break;
                decl = collapseWhitespace(decl.substr(0, open));
            }
            size_t nameStart = decl.size();
            while (nameStart > 0 && isIdentChar(decl[nameStart - 1])) --nameStart;
            if (nameStart == decl.size()) return;
            const std::string name = decl.substr(nameStart);
            if (name[0] >= '0' && name[0] <= '9') return;
            const std::string prefix = collapseWhitespace(decl.substr(0, nameStart));
            if (i == 0) {
                type = simplifyReturnType(prefix);
                if (type.empty()) return;
                if (type == "struct" || type == "class" || type == "enum" ||
                    type == "union" || type == "namespace")
                    return;
            } else if (prefix.find_first_not_of("*& ") != std::string::npos) {
                return;
            }
            names.push_back(name);
        }
        for (const std::string& name : names) fields.emplace(name, type);
    };

    int depth = 0;
    bool skipBraced = false;
    std::string pending;
    for (char c : publicBody) {
        if (depth > 0) {
            if (c == '{') {
                ++depth;
            } else if (c == '}') {
                --depth;
                if (depth == 0) skipBraced = false;
            }
            continue;
        }
        if (c == '{') {
            const std::string head = collapseWhitespace(pending);
            const bool nestedType =
                head.rfind("struct ", 0) == 0 || head.rfind("class ", 0) == 0 ||
                head.rfind("enum ", 0) == 0 || head.rfind("union ", 0) == 0 ||
                head.rfind("namespace ", 0) == 0;
            skipBraced = nestedType;
            if (!nestedType && pending.find('(') != std::string::npos)
                pending.clear(); // inline method body
            depth = 1;
            continue;
        }
        if (c == ';') {
            flush(pending + ";");
            pending.clear();
            continue;
        }
        if (c == ':') {
            const std::string head = collapseWhitespace(pending);
            if (head == "public" || head == "protected" || head == "private") {
                pending.clear();
                continue;
            }
        }
        if (!skipBraced) pending.push_back(c);
    }
}

// Matches method declarations in one chunk and records them in `info`.
// Returns false (with `error` set) when the regex engine gives up.
bool scanMethodSigs(const std::string& chunk, const std::string& className,
                    ClassInfo& info, std::string* error = nullptr) {
    try {
        for (auto it = std::sregex_iterator(chunk.begin(), chunk.end(), reSigPattern());
             it != std::sregex_iterator(); ++it) {
            const std::string retType = collapseWhitespace((*it)[1].str());
            const std::string methodName = (*it)[2].str();
            const std::string paramsRaw = (*it)[3].str();
            if (methodName == className || methodName.rfind("~", 0) == 0) continue;
            if (excludedHeaderMethods().count({className, methodName})) continue;
            Overload ov{simplifyParams(paramsRaw), simplifyReturnType(retType)};
            auto inserted = info.overloads
                .emplace(methodName, std::vector<Overload>{ov}).second;
            if (inserted) info.methodOrder.push_back(methodName);
            else info.overloads[methodName].push_back(ov);
        }
        return true;
    } catch (const std::regex_error& e) {
        if (error) *error = e.what();
        return false;
    }
}

} // namespace

std::string simplifyParam(const std::string& param) {
    std::string p = collapseWhitespace(param); // re.sub strip: initial .strip()
    // Python: p = param.strip(); then substitutions; collapse; strip.
    static const std::regex constPrefix(R"RE(\bconst\s+)RE");
    static const std::regex constSuffix(R"RE(\s+const$)RE");
    static const std::regex stdPrefix(R"RE(\bstd::)RE");
    static const std::regex refPtr(R"RE(\s*[&*]+\s*)RE");
    replaceAll(p, constPrefix, "");
    replaceAll(p, constSuffix, "");
    replaceAll(p, stdPrefix, "");
    replaceAll(p, refPtr, " ");
    return collapseWhitespace(p);
}

std::string simplifyParams(const std::string& paramsStr) {
    std::string trimmed = collapseWhitespace(paramsStr);
    if (trimmed.empty() || trimmed == "void") return "";
    // Python splits on every comma (even inside templates) and drops empties.
    std::vector<std::string> parts;
    size_t pos = 0;
    while (true) {
        size_t comma = trimmed.find(',', pos);
        parts.push_back(trimmed.substr(pos,
            comma == std::string::npos ? std::string::npos : comma - pos));
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    std::string out;
    for (const auto& part : parts) {
        std::string s = simplifyParam(part);
        if (!s.empty()) {
            if (!out.empty()) out += ", ";
            out += s;
        }
    }
    return out;
}

std::string simplifyReturnType(const std::string& ret) {
    if (ret.empty()) return "";
    static const std::regex keywords(R"RE(\b(?:virtual|inline|static|constexpr|noexcept|const)\b)RE");
    static const std::regex stdPrefix(R"RE(\bstd::)RE");
    static const std::regex ptrRef(R"RE([&*]+)RE");
    std::string t = ret;
    replaceAll(t, keywords, " ");
    replaceAll(t, stdPrefix, "");
    replaceAll(t, ptrRef, " ");
    t = collapseWhitespace(t);
    if (t.empty() || t == "void") return "";
    size_t space = t.find(' ');
    std::string first = space == std::string::npos ? t : t.substr(0, space);
    if (returnTypeReject().count(first)) return "";
    static const std::regex identifierLike(R"RE(^[A-Za-z_][A-Za-z0-9_:<>, ]*$)RE");
    if (!std::regex_match(t, identifierLike)) return "";
    return t;
}

std::map<std::string, ClassInfo> parseCppHeaders(const std::string& engineCoreDir) {
    std::map<std::string, ClassInfo> classMethods;
    for (const auto& path : listHeadersRecursive(engineCoreDir)) {
        std::string text, err;
        if (!readFile(path, text, err)) continue;
        text = stripComments(text);

        try {
            std::sregex_iterator classIt(text.begin(), text.end(), reClassStart());
            std::sregex_iterator classEnd;
            std::string lastClass;
            while (classIt != classEnd) {
                try {
                    const std::smatch& cm = *classIt;
                    std::string keyword = cm[1].str();
                    std::string className = cm[2].str();
                    lastClass = className;
                    size_t bracePos = static_cast<size_t>(cm.position(0) +
                                                          static_cast<std::streamoff>(cm.length(0)) - 1);
                    std::string body = extractClassBody(text, bracePos);
                    if (!body.empty()) {
                        bool isStruct = (keyword == "struct");
                        // Classes can interleave private blocks between public sections,
                        // so concatenate every public range instead of stopping at the
                        // first protected:/private: label.
                        bool inPublic = isStruct;
                        bool sawPublic = isStruct;
                        std::string publicBody;
                        size_t segmentStart = 0;
                        for (auto labelIt = std::sregex_iterator(body.begin(), body.end(), reAccessLabel());
                             labelIt != std::sregex_iterator(); ++labelIt) {
                            const std::smatch& lm = *labelIt;
                            size_t labelPos = static_cast<size_t>(lm.position(0));
                            if (inPublic)
                                publicBody += body.substr(segmentStart, labelPos - segmentStart) + '\n';
                            inPublic = (lm[1].str() == "public");
                            if (inPublic) sawPublic = true;
                            segmentStart = labelPos + static_cast<size_t>(lm.length(0));
                        }
                        if (inPublic) publicBody += body.substr(segmentStart);

                        if (sawPublic) {
                            ClassInfo& info = classMethods[className]; // creates empty like Python
                            const size_t fieldsBefore = info.fields.size();
                            try {
                                collectPublicFields(publicBody, info.fields);
                            } catch (const std::regex_error& e) {
                                std::fprintf(stderr, "warning: %s fields of %s in %s (%s)\n",
                                             info.fields.size() == fieldsBefore ? "skipped" : "partial",
                                             className.c_str(), path.c_str(), e.what());
                            }

                            const auto overloadsBefore = info.overloads;
                            const auto orderBefore = info.methodOrder;
                            std::string methodError;
                            if (!scanMethodSigs(publicBody, className, info, &methodError)) {
                                // MSVC's regex engine gives up on the whole body of some
                                // template-heavy classes (error_complexity). Retry line by
                                // line so the single-line signatures still register.
                                info.overloads = overloadsBefore;
                                info.methodOrder = orderBefore;
                                std::istringstream lines(publicBody);
                                std::string line;
                                size_t lineNo = 0;
                                size_t failedLine = 0;
                                std::string failedError;
                                while (std::getline(lines, line)) {
                                    ++lineNo;
                                    std::string lineError;
                                    if (!scanMethodSigs(line, className, info, &lineError) &&
                                        failedError.empty()) {
                                        failedLine = lineNo;
                                        failedError = lineError;
                                    }
                                }
                                if (!failedError.empty())
                                    std::fprintf(stderr,
                                                 "warning: partial methods of %s in %s (line %zu: %s)\n",
                                                 className.c_str(), path.c_str(), failedLine,
                                                 failedError.c_str());
                            }
                        }
                    }
                } catch (const std::regex_error& e) {
                    // MSVC's regex engine gives up on some template-heavy bodies
                    // (error_complexity) instead of matching them. Skip that class
                    // only and keep the classes already parsed.
                    std::fprintf(stderr, "warning: skipped class %s in %s (%s)\n",
                                 lastClass.c_str(), path.c_str(), e.what());
                }
                try {
                    ++classIt;
                } catch (const std::regex_error& e) {
                    // A failing search cannot be advanced past, so stop this file
                    // instead of retrying the same match forever.
                    std::fprintf(stderr, "warning: stopped scanning %s after class %s (%s)\n",
                                 path.c_str(), lastClass.c_str(), e.what());
                    break;
                }
            }
        } catch (const std::regex_error& e) {
            std::fprintf(stderr, "warning: skipped %s (%s)\n", path.c_str(), e.what());
        }
    }
    return classMethods;
}

} // namespace refl
