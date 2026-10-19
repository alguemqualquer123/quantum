// doriax-reflector: LuaBridge binding file parser (hand-rolled scan + std::regex,
// mirroring generate_api_suggestions.py one-to-one).
#include "reflector.h"

#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace refl {
namespace {

struct Match {
    std::vector<std::string> groups;
};

std::vector<Match> findMatches(const std::regex& re, const std::string& text) {
    std::vector<Match> out;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), re);
         it != std::sregex_iterator(); ++it) {
        Match m;
        for (size_t g = 0; g < it->size(); ++g) m.groups.push_back((*it)[g].str());
        out.push_back(std::move(m));
    }
    return out;
}

bool searchFirst(const std::regex& re, const std::string& text, Match& out) {
    std::smatch sm;
    if (!std::regex_search(text, sm, re)) return false;
    for (size_t g = 0; g < sm.size(); ++g) out.groups.push_back(sm[g].str());
    return true;
}

const std::regex& reNamespace() {
    static const std::regex re(
        R"RE(\.beginNamespace\(\s*"([^"]+)"\s*\)([\s\S]*?)\.endNamespace\(\))RE");
    return re;
}
const std::regex& reAddVariable() {
    static const std::regex re(R"RE(\.addVariable\(\s*"([^"]+)")RE");
    return re;
}
const std::regex& reBeginClass() {
    static const std::regex re(
        R"RE(\.beginClass\s*<\s*([^>]+?)\s*>\s*\(\s*"([^"]+)"\s*\))RE");
    return re;
}
const std::regex& reDeriveClass() {
    static const std::regex re(
        R"RE(\.deriveClass\s*<\s*([^,>]+?)\s*,\s*([^>]+?)\s*>\s*\(\s*"([^"]+)"\s*\))RE");
    return re;
}
const std::regex& reEndClass() {
    static const std::regex re(R"RE(\.endClass\(\))RE");
    return re;
}
const std::regex& reConstructorExtract() {
    static const std::regex re(R"RE(\.addConstructor\s*<(.+?)>\s*\()RE");
    return re;
}
const std::regex& reConstructorSignature() {
    static const std::regex re(R"RE(void\s*(?:\(\s*\*\s*\))?\s*\(([^()]*)\))RE");
    return re;
}
const std::regex& reAddStaticFunction() {
    static const std::regex re(R"RE(\.addStaticFunction\(\s*"([^"]+)")RE");
    return re;
}
const std::regex& reAddFunction() {
    static const std::regex re(R"RE(\.addFunction\(\s*"([^"]+)")RE");
    return re;
}
const std::regex& reAddStaticProperty() {
    static const std::regex re(R"RE(\.addStaticProperty\(\s*"([^"]+)")RE");
    return re;
}
const std::regex& reAddProperty() {
    static const std::regex re(R"RE(\.addProperty\(\s*"([^"]+)")RE");
    return re;
}

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n\f\v");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n\f\v");
    return s.substr(b, e - b + 1);
}

// Mirrors _parse_constructor_details() from the Python script.
std::vector<std::string> parseConstructorDetails(const std::string& text,
                                                 const std::string& className) {
    if (className.empty()) return {};
    Match m;
    if (!searchFirst(reConstructorExtract(), text, m)) {
        return {className + "(...)"};
    }
    const std::string& raw = m.groups[1];
    std::vector<std::string> details;
    std::set<std::string> seen;
    for (const auto& sig : findMatches(reConstructorSignature(), raw)) {
        std::string simplified = simplifyParams(sig.groups[1]);
        std::string detail = className + "(" + simplified + ")";
        if (seen.insert(detail).second) details.push_back(detail);
    }
    if (details.empty()) details.push_back(className + "()");
    return details;
}

} // namespace

std::vector<Symbol> parseBindingContent(const std::string& content) {
    std::vector<Symbol> symbols;

    // Enums (namespace-based), parsed over the whole content.
    for (const auto& m : findMatches(reNamespace(), content)) {
        const std::string& nsName = m.groups[1];
        const std::string& nsBody = m.groups[2];
        if (nsName.find("FunctionSubscribe") != std::string::npos) continue;
        auto values = findMatches(reAddVariable(), nsBody);
        if (values.empty()) continue;
        symbols.push_back({nsName, SymKind::Enum, "enum " + nsName, ""});
        for (const auto& v : values) {
            symbols.push_back({v.groups[1], SymKind::EnumMember,
                               nsName + "." + v.groups[1], nsName});
        }
    }

    // Classes: line-by-line state machine.
    std::vector<std::string> lines;
    {
        std::istringstream in(content);
        std::string line;
        while (std::getline(in, line)) lines.push_back(line);
    }

    std::string currentClass, currentLuaName, currentBase;
    std::vector<std::string> currentConstructors;
    std::string pendingConstructor;
    bool hasPending = false;

    auto resetClass = [&]() {
        currentClass.clear();
        currentLuaName.clear();
        currentBase.clear();
        currentConstructors.clear();
        pendingConstructor.clear();
        hasPending = false;
    };

    for (const auto& rawLine : lines) {
        std::string stripped = trim(rawLine);

        if (hasPending) {
            pendingConstructor += ' ' + stripped;
            if (stripped.find('>') != std::string::npos) {
                for (auto& d : parseConstructorDetails(pendingConstructor, currentLuaName))
                    currentConstructors.push_back(d);
                hasPending = false;
                pendingConstructor.clear();
            }
            continue;
        }

        Match m;
        // beginClass
        if (searchFirst(reBeginClass(), stripped, m)) {
            currentClass = trim(m.groups[1]);
            currentLuaName = m.groups[2];
            currentBase.clear();
            currentConstructors.clear();
            continue;
        }
        // deriveClass
        m.groups.clear();
        if (searchFirst(reDeriveClass(), stripped, m)) {
            currentClass = trim(m.groups[1]);
            currentBase = trim(m.groups[2]);
            currentLuaName = m.groups[3];
            currentConstructors.clear();
            continue;
        }
        // endClass
        m.groups.clear();
        if (searchFirst(reEndClass(), stripped, m)) {
            if (!currentLuaName.empty()) {
                std::string baseInfo = currentBase.empty() ? "" : " : " + currentBase;
                symbols.push_back({currentLuaName, SymKind::Class,
                                   "class " + currentLuaName + baseInfo, ""});
                for (const auto& ctor : currentConstructors) {
                    symbols.push_back({currentLuaName, SymKind::Constructor, ctor,
                                       currentLuaName});
                }
            }
            resetClass();
            continue;
        }

        if (currentLuaName.empty()) continue;

        // Constructor
        if (stripped.find(".addConstructor") != std::string::npos) {
            if (stripped.find('>') != std::string::npos) {
                for (auto& d : parseConstructorDetails(stripped, currentLuaName))
                    currentConstructors.push_back(d);
            } else {
                pendingConstructor = stripped;
                hasPending = true;
            }
            continue;
        }

        // Static function
        m.groups.clear();
        if (searchFirst(reAddStaticFunction(), stripped, m)) {
            const std::string fname = m.groups[1];
            if (fname.rfind("__", 0) != 0) {
                symbols.push_back({fname, SymKind::StaticMethod,
                                   currentLuaName + "." + fname + "()",
                                   currentLuaName});
            }
            continue;
        }
        // Member function
        m.groups.clear();
        if (searchFirst(reAddFunction(), stripped, m)) {
            const std::string fname = m.groups[1];
            if (fname.rfind("__", 0) != 0) {
                symbols.push_back({fname, SymKind::Method,
                                   currentLuaName + ":" + fname + "()",
                                   currentLuaName});
            }
            continue;
        }
        // Static property
        m.groups.clear();
        if (searchFirst(reAddStaticProperty(), stripped, m)) {
            const std::string pname = m.groups[1];
            if (pname.rfind("__", 0) != 0) {
                bool isEvent = stripped.find("[]") != std::string::npos ||
                               stripped.find("lua_State") != std::string::npos;
                symbols.push_back({pname,
                                   isEvent ? SymKind::Event : SymKind::Constant,
                                   currentLuaName + "." + pname, currentLuaName});
            }
            continue;
        }
        // Member property
        m.groups.clear();
        if (searchFirst(reAddProperty(), stripped, m)) {
            const std::string pname = m.groups[1];
            if (pname.rfind("__", 0) != 0) {
                symbols.push_back({pname, SymKind::Property,
                                   currentLuaName + "." + pname, currentLuaName});
            }
            continue;
        }
    }

    return symbols;
}

} // namespace refl
