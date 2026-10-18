// doriax-reflector: C++ header parser (public method extraction), mirroring
// parse_cpp_headers() and the _simplify_* helpers of generate_api_suggestions.py.
#include "reflector.h"

#include <algorithm>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>

namespace refl {
namespace {

const std::regex& reSigPattern() {
    static const std::regex re(
        R"RE((?:virtual\s+|inline\s+|static\s+)*([A-Za-z0-9_:<>&*\s]+)\s+([A-Za-z0-9_]+)\s*\(([^)]*)\)\s*(?:const)?\s*(?:override|final)?\s*[;{])RE");
    return re;
}

const std::regex& reClassStart() {
    static const std::regex re(
        R"RE((?:class|struct)\s+(?:[A-Z0-9_]+\s+)?([A-Za-z0-9_]+)\s*(?::[^{]+)?\{)RE");
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

        std::sregex_iterator classIt(text.begin(), text.end(), reClassStart());
        std::sregex_iterator classEnd;
        for (; classIt != classEnd; ++classIt) {
            const std::smatch& cm = *classIt;
            std::string className = cm[1].str();
            size_t bracePos = static_cast<size_t>(cm.position(0) +
                                                  static_cast<std::streamoff>(cm.length(0)) - 1);
            std::string body = extractClassBody(text, bracePos);
            if (!body.empty()) {
                size_t publicIdx = body.find("public:");
                if (publicIdx != std::string::npos) {
                    size_t endIdx = body.size();
                    size_t protectedIdx = body.find("protected:", publicIdx + 1);
                    size_t privateIdx = body.find("private:", publicIdx + 1);
                    if (protectedIdx != std::string::npos && protectedIdx < endIdx)
                        endIdx = protectedIdx;
                    if (privateIdx != std::string::npos && privateIdx < endIdx)
                        endIdx = privateIdx;
                    std::string publicBody = body.substr(publicIdx, endIdx - publicIdx);

                    ClassInfo& info = classMethods[className]; // creates empty like Python

                    for (auto it = std::sregex_iterator(publicBody.begin(),
                             publicBody.end(), reSigPattern());
                         it != std::sregex_iterator(); ++it) {
                        std::string retType = collapseWhitespace((*it)[1].str());
                        std::string methodName = (*it)[2].str();
                        std::string paramsRaw = (*it)[3].str();
                        if (methodName == className ||
                            methodName.rfind("~", 0) == 0) continue;
                        if (excludedHeaderMethods().count({className, methodName}))
                            continue;
                        Overload ov{simplifyParams(paramsRaw),
                                    simplifyReturnType(retType)};
                        auto inserted = info.overloads
                            .emplace(methodName, std::vector<Overload>{ov}).second;
                        if (inserted) info.methodOrder.push_back(methodName);
                        else info.overloads[methodName].push_back(ov);
                    }
                }
            }
        }
    }
    return classMethods;
}

} // namespace refl
