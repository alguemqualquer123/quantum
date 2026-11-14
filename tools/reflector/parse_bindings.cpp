// quantum-reflector: LuaBridge binding file parser (hand-rolled scan + std::regex,
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
// A value bound straight onto the global namespace, with no class or namespace
// around it (`NULL_ENTITY`).
const std::regex& reGlobalVariable() {
    static const std::regex re(
        R"RE(getGlobalNamespace\(\s*L\s*\)\s*\.addVariable\(\s*"([^"]+)"\s*,\s*([^;]*))RE");
    return re;
}
// The template arguments can themselves carry a template (`FunctionSubscribe<void()>`),
// so one level of nested angles is part of the name, not the end of the list.
const std::regex& reBeginClass() {
    static const std::regex re(
        R"RE(\.beginClass\s*<\s*((?:[^<>]|<[^<>]*>)+?)\s*>\s*\(\s*"([^"]+)"\s*\))RE");
    return re;
}
const std::regex& reDeriveClass() {
    static const std::regex re(
        R"RE(\.deriveClass\s*<\s*([^,>]+?)\s*,\s*((?:[^<>]|<[^<>]*>)+?)\s*>\s*\(\s*"([^"]+)"\s*\))RE");
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
const std::regex& reAddPropertyGetter() {
    static const std::regex re(
        R"RE(\.addProperty\(\s*"[^"]+"\s*,\s*&?\w+::(\w+))RE");
    return re;
}
const std::regex& reAddStaticPropertyGetter() {
    static const std::regex re(
        R"RE(\.addStaticProperty\(\s*"[^"]+"\s*,\s*&?\w+::(\w+))RE");
    return re;
}
const std::regex& reStaticPropertyLambdaType() {
    static const std::regex re(
        R"RE(\[\] *\( *\) *-> *([A-Za-z_][\w:]*(?:<[^<>]*>)?))RE");
    return re;
}
const std::regex& reStaticCastType() {
    static const std::regex re(R"RE(static_cast\s*<\s*([\w:]+))RE");
    return re;
}

// An event accessor is the only bound accessor that receives the caller's
// lua_State and hands it to a FunctionSubscribe (`Engine::onUpdate = L` or
// `self->onClick = L`). Constants may read the state too (the enum tables build
// a Lua table), so the assignment is what separates the two.
bool bindsLuaEvent(const std::string& s) {
    return s.find("lua_State") != std::string::npos &&
           (s.find("= L;") != std::string::npos || s.find("=L;") != std::string::npos);
}

// `FunctionSubscribe_V_FF` and friends are the bases every event field derives
// from. They are global in Lua but a script never names one, and the generated
// type files would fill with 30+ unreadable class blocks, so the whole body is
// skipped as soon as the class opens.
bool isSubscriptionBase(const std::string& luaName) {
    return luaName.rfind("FunctionSubscribe_", 0) == 0;
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

    // Namespaces are two different things in a binding, and both have to reach the docs:
    // `addVariable` members are an enum (`NoiseType.PERLIN`), and `addFunction` members are a table
    // of plain Lua functions (`WorldGen.generate(...)`). The second form is how an API that needs
    // the VM itself is bound, because a raw `int f(lua_State*)` cannot be a typed LuaBridge call,
    // and before this the whole table was silently missing from the generated reference.
    for (const auto& m : findMatches(reNamespace(), content)) {
        const std::string& nsName = m.groups[1];
        const std::string& nsBody = m.groups[2];
        if (nsName.find("FunctionSubscribe") != std::string::npos) continue;
        auto values = findMatches(reAddVariable(), nsBody);
        auto functions = findMatches(reAddFunction(), nsBody);
        if (values.empty() && functions.empty()) continue;
        if (!values.empty()) {
            symbols.push_back({nsName, SymKind::Enum, "enum " + nsName, ""});
            for (const auto& v : values) {
                symbols.push_back({v.groups[1], SymKind::EnumMember,
                                   nsName + "." + v.groups[1], nsName});
            }
        }
        if (!functions.empty() && nsBody.find(".beginClass") == std::string::npos) {
            symbols.push_back({nsName, SymKind::Class, nsName, ""});
            for (const auto& f : functions) {
                const std::string& fname = f.groups[1];
                if (fname.rfind("__", 0) == 0) continue;
                symbols.push_back({fname, SymKind::StaticMethod,
                                   nsName + "." + fname + "()", nsName});
            }
        }
    }

    // Values bound straight onto the global namespace. A script names these with
    // no class in front (`NULL_ENTITY`), so a type file that leaves them out
    // makes every correct use look like a typo to the editor.
    for (const auto& m : findMatches(reGlobalVariable(), content)) {
        std::string valueType;
        Match cast;
        if (searchFirst(reStaticCastType(), m.groups[2], cast)) valueType = cast.groups[1];
        symbols.push_back({m.groups[1], SymKind::Constant, m.groups[1], "", "", valueType});
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
            if (isSubscriptionBase(m.groups[2])) { resetClass(); continue; }
            currentClass = trim(m.groups[1]);
            currentLuaName = m.groups[2];
            currentBase.clear();
            currentConstructors.clear();
            continue;
        }
        // deriveClass
        m.groups.clear();
        if (searchFirst(reDeriveClass(), stripped, m)) {
            if (isSubscriptionBase(m.groups[3])) { resetClass(); continue; }
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
                const bool isEvent = bindsLuaEvent(stripped);
                Symbol symbol{pname,
                              isEvent ? SymKind::Event : SymKind::Constant,
                              currentLuaName + "." + pname, currentLuaName};
                // Either accessor form lets a renderer show the real type: the
                // member-function pointer names it, the lambda writes it.
                m.groups.clear();
                if (!isEvent && searchFirst(reAddStaticPropertyGetter(), stripped, m))
                    symbol.getter = m.groups[1];
                m.groups.clear();
                if (!isEvent && searchFirst(reStaticPropertyLambdaType(), stripped, m))
                    symbol.valueType = m.groups[1];
                symbols.push_back(std::move(symbol));
            }
            continue;
        }
        // Member property
        m.groups.clear();
        if (searchFirst(reAddProperty(), stripped, m)) {
            const std::string pname = m.groups[1];
            if (pname.rfind("__", 0) != 0) {
                const bool isEvent = bindsLuaEvent(stripped);
                Symbol symbol{pname, isEvent ? SymKind::Event : SymKind::Property,
                              currentLuaName + "." + pname, currentLuaName};
                // The bound accessor names the C++ getter, which is what lets a
                // renderer type a property whose Lua name is not get<Name>.
                m.groups.clear();
                if (!isEvent && searchFirst(reAddPropertyGetter(), stripped, m))
                    symbol.getter = m.groups[1];
                symbols.push_back(std::move(symbol));
            }
            continue;
        }
    }

    return symbols;
}

} // namespace refl
