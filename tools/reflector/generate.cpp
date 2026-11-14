// quantum-reflector: manifest assembly + header/JSON rendering + CI check.
#include "reflector.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace refl {
namespace {

// Generated files land inside a project and get committed, so they must not carry
// the build machine's path. Neither the argument nor the working directory is stable
// (CMake passes absolute, a person passes relative), so cut at the checkout's own
// engine/ directory: every path this tool prints starts there.
std::string displayPath(const std::string& p) {
    const std::string g = fs::path(p).generic_string();
    const size_t at = g.find("/engine/");
    return at == std::string::npos ? g : g.substr(at + 1);
}

struct DedupKey {
    std::string name, kind, parent, detail;
    bool operator<(const DedupKey& o) const {
        return std::tie(name, kind, parent, detail) <
               std::tie(o.name, o.kind, o.parent, o.detail);
    }
};

DedupKey keyOf(const Symbol& s) {
    // Python: (name, kind, parent, detail if kind == Constructor else '')
    return {s.name, kindName(s.kind), s.parent,
            s.kind == SymKind::Constructor ? s.detail : std::string()};
}

std::vector<Symbol> dedup(const std::vector<Symbol>& in) {
    std::set<DedupKey> seen;
    std::vector<Symbol> out;
    for (const auto& s : in) {
        if (seen.insert(keyOf(s)).second) out.push_back(s);
    }
    return out;
}

std::string formatDetail(const std::string& parent, const std::string& name,
                         const std::string& sep, const Overload& ov) {
    std::string detail = parent + sep + name + "(" + ov.params + ")";
    if (!detail.empty() && !ov.returns.empty()) detail += " -> " + ov.returns;
    return detail;
}

const Overload& shortestOverload(const std::vector<Overload>& v) {
    static const Overload kEmpty{};
    if (v.empty()) return kEmpty;
    return *std::min_element(v.begin(), v.end(),
                             [](const Overload& a, const Overload& b) {
                                 return a.params.size() < b.params.size();
                             });
}

std::vector<Symbol> filterKind(const std::vector<Symbol>& s, SymKind k) {
    std::vector<Symbol> out;
    for (const auto& sym : s) if (sym.kind == k) out.push_back(sym);
    return out;
}

std::string jsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (size_t i = 0; i < in.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(in[i]);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

void pushSymbolJson(std::ostringstream& o, const Symbol& s) {
    o << "      {\"name\": \"" << jsonEscape(s.name) << "\", \"kind\": \""
      << kindName(s.kind) << "\", \"detail\": \"" << jsonEscape(s.detail)
      << "\", \"parent\": \"" << jsonEscape(s.parent)
      << "\", \"arg_count\": " << argCountFromDetail(s.detail) << "}";
}

std::string trimText(const std::string& s) {
    const size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return std::string();
    const size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

std::string capitalizeAscii(const std::string& s) {
    if (s.empty()) return s;
    std::string out = s;
    if (out[0] >= 'a' && out[0] <= 'z')
        out[0] = static_cast<char>(out[0] - 'a' + 'A');
    return out;
}

std::string anchorFor(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '_' || c == '-')
            out.push_back(static_cast<char>(std::tolower(u)));
        else
            out.push_back('-');
    }
    return out;
}

std::string cleanTypeName(const std::string& raw) {
    std::string t = trimText(raw);
    for (const char* prefix : {"const ", "struct ", "class ", "std::"}) {
        const size_t n = std::char_traits<char>::length(prefix);
        while (t.rfind(prefix, 0) == 0) t.erase(0, n);
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (const char* suffix : {" const", "&", "*"}) {
            const size_t n = std::char_traits<char>::length(suffix);
            while (t.size() >= n && t.compare(t.size() - n, n, suffix) == 0) {
                t.erase(t.size() - n);
                changed = true;
            }
        }
    }
    while (t.find("std::") != std::string::npos)
        t.erase(t.find("std::"), 5);
    return trimText(t);
}

std::string detailReturnType(const std::string& detail) {
    const size_t arrow = detail.rfind(" -> ");
    if (arrow == std::string::npos) return std::string();
    return cleanTypeName(detail.substr(arrow + 4));
}

std::vector<std::string> splitTopLevelParams(const std::string& raw) {
    std::vector<std::string> out;
    int depth = 0;
    std::string current;
    for (char c : raw) {
        if (c == '<' || c == '(' || c == '[') ++depth;
        else if (c == '>' || c == ')' || c == ']') depth = std::max(0, depth - 1);
        if (c == ',' && depth == 0) {
            out.push_back(trimText(current));
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    const std::string last = trimText(current);
    if (!last.empty()) out.push_back(last);
    return out;
}

std::string detailFirstParamType(const std::string& detail) {
    const size_t open = detail.find('(');
    if (open == std::string::npos) return std::string();
    const size_t close = detail.rfind(')');
    if (close == std::string::npos || close <= open) return std::string();
    const auto params =
        splitTopLevelParams(detail.substr(open + 1, close - open - 1));
    if (params.empty()) return std::string();
    std::string param = params.front();
    const size_t eq = param.find('=');
    if (eq != std::string::npos) param = trimText(param.substr(0, eq));
    const size_t space = param.find_last_of(" \t");
    if (space != std::string::npos) {
        const std::string tail = trimText(param.substr(space + 1));
        bool identifier = !tail.empty();
        for (char c : tail) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
                identifier = false;
                break;
            }
        }
        if (identifier) param = param.substr(0, space);
    }
    return cleanTypeName(param);
}

void writeLinkList(std::ostringstream& o, const std::vector<std::string>& names,
                   size_t perLine) {
    for (size_t i = 0; i < names.size(); ++i) {
        o << "[" << names[i] << "](#" << anchorFor(names[i]) << ")";
        if (i + 1 < names.size()) {
            o << ",";
            o << ((i + 1) % perLine == 0 ? "\n" : " ");
        }
    }
    o << "\n";
}

std::string lowerAscii(std::string text) {
    for (char& c : text)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}

// Lua-facing view of the manifest, shared by the Markdown reference and the
// annotated types file: registration order per class, resolved bases, and the
// accessor lookup that types a bound property.
struct ApiIndex {
    std::vector<std::string> classOrder;
    std::vector<std::string> enumOrder;
    std::map<std::string, std::string> classBase;
    std::map<std::string, std::vector<const Symbol*>> ctors;
    std::map<std::string, std::vector<const Symbol*>> statics;
    std::map<std::string, std::vector<const Symbol*>> methods;
    std::map<std::string, std::vector<const Symbol*>> properties;
    std::map<std::string, std::vector<const Symbol*>> events;
    std::map<std::string, std::vector<const Symbol*>> constants;
    std::map<std::string, std::vector<const Symbol*>> enumMembers;
    const Manifest* manifest = nullptr;

    explicit ApiIndex(const Manifest& m) : manifest(&m) {
        auto indexMethod = [&](const Symbol& s) {
            methodsByName[s.parent + '\x1f' + s.name].push_back(&s);
            methodsByNameLower[s.parent + '\x1f' + lowerAscii(s.name)].push_back(&s);
        };

        std::set<std::string> seenClass;
        std::set<std::string> seenEnum;
        for (const auto& s : m.symbols) {
            switch (s.kind) {
                case SymKind::Class: {
                    if (seenClass.insert(s.name).second)
                        classOrder.push_back(s.name);
                    const size_t sep = s.detail.find(" : ");
                    if (sep != std::string::npos)
                        classBase[s.name] = trimText(s.detail.substr(sep + 3));
                    break;
                }
                case SymKind::Constructor: ctors[s.parent].push_back(&s); break;
                case SymKind::StaticMethod: statics[s.parent].push_back(&s); break;
                case SymKind::Method:
                    methods[s.parent].push_back(&s);
                    indexMethod(s);
                    break;
                case SymKind::Property: properties[s.parent].push_back(&s); break;
                case SymKind::Event: events[s.parent].push_back(&s); break;
                case SymKind::Constant: constants[s.parent].push_back(&s); break;
                case SymKind::Enum:
                    if (seenEnum.insert(s.name).second)
                        enumOrder.push_back(s.name);
                    break;
                case SymKind::EnumMember: enumMembers[s.parent].push_back(&s); break;
                // C++ headers fill in accessor types for properties bound by
                // reference (addProperty("speed", &Actor::getSpeed, ...)).
                case SymKind::CppMethod:
                    indexMethod(s);
                    break;
            }
        }

        // Built-in parents (Math, Engine, ...) have no Class symbol of their own.
        for (const auto& s : m.symbols) {
            if (s.kind == SymKind::CppMethod || s.kind == SymKind::Enum ||
                s.kind == SymKind::EnumMember || s.parent.empty())
                continue;
            if (seenClass.insert(s.parent).second) classOrder.push_back(s.parent);
        }
    }

    const Symbol* findMethod(const std::string& parent,
                             const std::string& name) const {
        const auto it = methodsByName.find(parent + '\x1f' + name);
        if (it == methodsByName.end() || it->second.empty()) return nullptr;
        return it->second.front();
    }

    // Case-insensitive: Lua-facing C++ getters are not always camelCased like
    // the property (svgScale -> getSVGScale).
    const Symbol* findMethodCI(const std::string& parent,
                               const std::string& name) const {
        const auto it = methodsByNameLower.find(parent + '\x1f' + lowerAscii(name));
        if (it == methodsByNameLower.end() || it->second.empty()) return nullptr;
        return it->second.front();
    }

    std::string classField(const std::string& parent,
                           const std::string& name) const {
        const auto cls = manifest->classFields.find(parent);
        if (cls == manifest->classFields.end()) return {};
        const auto field = cls->second.find(name);
        return field == cls->second.end() ? std::string() : field->second;
    }

    std::string propertyType(const Symbol& property) const {
        // The binding names the exact accessor it binds
        // (addProperty("shape", &Particles::getPositionInitializerShape, ...)).
        if (!property.getter.empty()) {
            const Symbol* getter = findMethod(property.parent, property.getter);
            if (!getter) getter = findMethodCI(property.parent, property.getter);
            if (getter) {
                const std::string type = detailReturnType(getter->detail);
                if (!type.empty() && type != "void") return type;
            }
        }
        const std::string suffix = capitalizeAscii(property.name);
        auto returnTypeOf = [&](const Symbol* getter) {
            if (!getter) return std::string();
            const std::string type = detailReturnType(getter->detail);
            return type.empty() || type == "void" ? std::string() : type;
        };
        if (const std::string t = returnTypeOf(findMethod(property.parent, "get" + suffix));
            !t.empty())
            return t;
        if (const std::string t = returnTypeOf(findMethod(property.parent, "is" + suffix));
            !t.empty())
            return t;
        if (const Symbol* setter = findMethod(property.parent, "set" + suffix)) {
            const std::string type = detailFirstParamType(setter->detail);
            if (!type.empty()) return type;
        }
        // Property bound directly to a public data member:
        // addProperty("speed", &ActionComponent::speed, &ActionComponent::speed).
        const std::string field = classField(property.parent, property.name);
        if (!field.empty()) return field;
        if (const std::string t = returnTypeOf(findMethodCI(property.parent, "get" + suffix));
            !t.empty())
            return t;
        if (const std::string t = returnTypeOf(findMethodCI(property.parent, "is" + suffix));
            !t.empty())
            return t;
        if (const Symbol* setter = findMethodCI(property.parent, "set" + suffix)) {
            const std::string type = detailFirstParamType(setter->detail);
            if (!type.empty()) return type;
        }
        return "unknown";
    }

    bool hasSetter(const Symbol& property) const {
        if (!classField(property.parent, property.name).empty()) return true;
        const std::string suffix = capitalizeAscii(property.name);
        return findMethod(property.parent, "set" + suffix) != nullptr ||
               findMethodCI(property.parent, "set" + suffix) != nullptr;
    }

private:
    std::map<std::string, std::vector<const Symbol*>> methodsByName;
    std::map<std::string, std::vector<const Symbol*>> methodsByNameLower;
};

// ── Lua / EmmyLua emission ─────────────────────────────────────────────────

bool isPlainLuaIdentifier(const std::string& s) {
    static const char* const kKeywords[] = {
        "and",    "break",  "do",     "else",   "elseif", "end",    "false",
        "for",    "function", "goto",  "if",     "in",     "local",  "nil",
        "not",    "or",     "repeat", "return", "self",   "then",   "true",
        "until",  "while"};
    if (s.empty()) return false;
    if (!std::isalpha(static_cast<unsigned char>(s[0])) && s[0] != '_') return false;
    for (char c : s) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
    }
    for (const char* k : kKeywords)
        if (s == k) return false;
    return true;
}

struct LuaParam {
    std::string name;
    std::string type;    // cleaned C++ type, as cleanTypeName leaves it
    bool optional = false;
};

// "float duration = 1.0f" -> {duration, float, optional}. Where the binding
// kept only the type, the name becomes argN so the declaration stays valid Lua.
LuaParam splitTypedParam(const std::string& raw, size_t index) {
    LuaParam out;
    std::string p = trimText(raw);
    if (const size_t eq = p.find('='); eq != std::string::npos) {
        out.optional = true;
        p = trimText(p.substr(0, eq));
    }
    std::string name;
    std::string type = p;
    if (const size_t space = p.find_last_of(" \t"); space != std::string::npos) {
        const std::string tail = trimText(p.substr(space + 1));
        bool identifier = !tail.empty();
        for (char c : tail) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
                identifier = false;
                break;
            }
        }
        if (identifier) {
            name = tail;
            type = trimText(p.substr(0, space));
        }
    }
    out.type = cleanTypeName(type);
    if (name.empty()) {
        // C++ constructor signatures carry types only; naming the argument
        // after its class makes Body3D(scene, entity) read like the code.
        if (isPlainLuaIdentifier(out.type) && out.type[0] >= 'A' && out.type[0] <= 'Z')
            out.name = std::string(1, static_cast<char>(out.type[0] - 'A' + 'a')) +
                       out.type.substr(1);
        else
            out.name = "arg" + std::to_string(index + 1);
    } else if (isPlainLuaIdentifier(name)) {
        out.name = name;
    } else {
        out.name = isPlainLuaIdentifier(name + "_") ? name + "_"
                                                    : "arg" + std::to_string(index + 1);
    }
    return out;
}

std::vector<LuaParam> splitTypedParams(const std::string& detail) {
    const size_t open = detail.find('(');
    const size_t close = detail.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close <= open)
        return {};
    auto raw = splitTopLevelParams(detail.substr(open + 1, close - open - 1));
    if (raw.size() == 1 && trimText(raw.front()) == "void") raw.clear();
    std::vector<LuaParam> out;
    for (size_t i = 0; i < raw.size(); ++i) out.push_back(splitTypedParam(raw[i], i));
    return out;
}

// A cleaned C++ type expressed as an EmmyLua type. Anything the mapping cannot
// place (odd spelling, nesting it does not understand) becomes `any`, which is
// always a legal annotation.
std::string luaTypeFor(const std::string& raw) {
    const std::string t = trimText(raw);
    if (t.empty() || t == "void" || t == "unknown" || t == "any") return "any";
    // "...", "?" and other parser leftovers carry no type at all.
    if (std::find_if(t.begin(), t.end(), [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) != 0;
        }) == t.end())
        return "any";
    if (t == "bool" || t == "boolean") return "boolean";
    if (t == "string" || t == "String") return "string";
    static const char* const kNumbers[] = {
        "char",  "signed char",      "unsigned char",    "short",
        "unsigned short", "int",     "unsigned int",     "long",
        "unsigned long",  "long long", "unsigned long long", "int8_t",
        "int16_t", "int32_t",        "int64_t",          "uint8_t",
        "uint16_t", "uint32_t",      "uint64_t",         "size_t",
        "float", "double",           "number",           "integer"};
    for (const char* n : kNumbers)
        if (t == n) return "number";

    if (const size_t lt = t.find('<'); lt != std::string::npos && t.back() == '>') {
        const std::string head = lowerAscii(trimText(t.substr(0, lt)));
        const auto args = splitTopLevelParams(t.substr(lt + 1, t.size() - lt - 2));
        if (head == "function") return "function";
        if (head == "vector" || head == "array" || head == "hybridarray" ||
            head == "list" || head == "span")
            return args.empty() ? "any" : luaTypeFor(args.front()) + "[]";
        if (head == "map" || head == "unordered_map")
            return args.size() == 2
                       ? "table<" + luaTypeFor(args[0]) + ", " + luaTypeFor(args[1]) + ">"
                       : std::string("table");
        return "any";
    }
    if (const size_t bracket = t.find('['); bracket != std::string::npos && t.back() == ']')
        return luaTypeFor(t.substr(0, bracket)) + "[]";
    for (char c : t) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '.')
            return "any";
    }
    return t;
}

// Empty string means the signature says nothing (void or unknown) and no
// ---@return line is worth emitting.
std::string luaReturnFor(const std::string& detail) {
    const std::string r = detailReturnType(detail);
    if (r.empty() || r == "void" || r == "unknown") return std::string();
    return luaTypeFor(r);
}

std::string luaOverloadFun(const std::vector<LuaParam>& params,
                           const std::string& returns) {
    std::ostringstream f;
    f << "fun(";
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) f << ", ";
        f << params[i].name << (params[i].optional ? "?" : "") << ": "
          << luaTypeFor(params[i].type);
    }
    f << ")";
    if (!returns.empty()) f << ": " << returns;
    return f.str();
}

std::string luaParamNames(const std::vector<LuaParam>& params) {
    std::string out;
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) out += ", ";
        out += params[i].name;
    }
    return out;
}

} // namespace

bool buildManifest(const std::string& bindingsDir,
                   const std::string& engineCoreDirIn,
                   Manifest& out,
                   std::string& err,
                   void (*onProgress)(const std::string&, size_t)) {
    auto files = listBindingCppFiles(bindingsDir);
    if (files.empty()) {
        err = "WARNING: No binding files found in " + bindingsDir;
        return false;
    }

    std::string engineCoreDir = engineCoreDirIn;
    if (engineCoreDir.empty()) {
        // Python default: abspath(join(binding_dir, '..', '..'))
        std::error_code ec;
        fs::path p = fs::absolute(bindingsDir, ec).parent_path().parent_path();
        engineCoreDir = ec ? bindingsDir : p.generic_string();
    }

    std::vector<Symbol> all;
    out.perFileCounts.clear();
    for (const auto& f : files) {
        std::string content, readErr;
        if (!readFile(f, content, readErr)) {
            err = readErr;
            return false;
        }
        auto syms = parseBindingContent(stripComments(content));
        all.insert(all.end(), syms.begin(), syms.end());
        std::string base = fs::path(f).filename().generic_string();
        out.perFileCounts.emplace_back(base, syms.size());
        if (onProgress) onProgress(base, syms.size());
    }

    auto cppMethods = parseCppHeaders(engineCoreDir);

    for (const auto& cls : cppMethods) {
        if (!cls.second.fields.empty())
            out.classFields[cls.first] = cls.second.fields;
    }

    // (class, method) -> shortest overload, mirrors cpp_signatures lookup.
    std::map<std::pair<std::string, std::string>, Overload> cppSignatures;
    for (const auto& cls : cppMethods) {
        for (const auto& method : cls.second.methodOrder) {
            cppSignatures[{cls.first, method}] =
                shortestOverload(cls.second.overloads.at(method));
        }
    }

    // Enrich Lua method details with params/return from C++ headers.
    for (auto& s : all) {
        if ((s.kind == SymKind::Method || s.kind == SymKind::StaticMethod) &&
            !s.parent.empty()) {
            auto it = cppSignatures.find({s.parent, s.name});
            if (it != cppSignatures.end()) {
                std::string sep = s.kind == SymKind::StaticMethod ? "." : ":";
                s.detail = formatDetail(s.parent, s.name, sep, it->second);
            }
        }
    }

    // C++ methods a bound class does NOT expose to Lua -> CppMethod symbols.
    std::vector<std::string> luaClassOrder;
    std::set<std::string> luaClassSeen;
    std::set<std::pair<std::string, std::string>> luaBound;
    for (const auto& s : all) {
        if (s.kind == SymKind::Class && luaClassSeen.insert(s.name).second)
            luaClassOrder.push_back(s.name);
        if (s.kind == SymKind::Method || s.kind == SymKind::StaticMethod)
            luaBound.emplace(s.name, s.parent);
    }
    for (const auto& cls : luaClassOrder) {
        auto it = cppMethods.find(cls);
        if (it == cppMethods.end()) continue;
        for (const auto& method : it->second.methodOrder) {
            if (luaBound.count({method, cls})) continue;
            all.push_back({method, SymKind::CppMethod,
                           formatDetail(cls, method, "::",
                                        shortestOverload(it->second.overloads.at(method))),
                           cls});
        }
    }

    out.symbols = dedup(all);
    out.bindingsDir = bindingsDir;
    out.engineCoreDir = engineCoreDir;
    return true;
}

std::string renderHeader(const Manifest& manifest) {
    const auto& unique = manifest.symbols;
    auto enums = filterKind(unique, SymKind::Enum);
    auto enumMembers = filterKind(unique, SymKind::EnumMember);
    auto classes = filterKind(unique, SymKind::Class);
    auto constructors = filterKind(unique, SymKind::Constructor);
    auto methods = filterKind(unique, SymKind::Method);
    auto cppOnly = filterKind(unique, SymKind::CppMethod);
    auto staticMethods = filterKind(unique, SymKind::StaticMethod);
    auto properties = filterKind(unique, SymKind::Property);
    auto constants = filterKind(unique, SymKind::Constant);
    auto events = filterKind(unique, SymKind::Event);

    std::ostringstream lines;
    auto line = [&](const std::string& s) { lines << s << '\n'; };

    auto stringSet = [&](const std::vector<std::string>& names) {
        line("    return {");
        for (const auto& n : names) line("        \"" + escapeCppString(n) + "\",");
        line("    };");
    };

    line("// Auto-generated by quantum-reflector - DO NOT EDIT");
    line("// Parsed from engine/core/script/binding/*.cpp");
    line("#pragma once");
    line("");
    line("#include <string>");
    line("#include <vector>");
    line("#include <unordered_set>");
    line("");
    line("namespace quantum::editor {");
    line("");

    line("inline std::unordered_set<std::string> getEngineTypeNames() {");
    {
        std::vector<std::string> names;
        std::set<std::string> seen;
        for (const auto& s : classes)
            if (seen.insert(s.name).second) names.push_back(s.name);
        std::sort(names.begin(), names.end());
        stringSet(names);
    }
    line("}");
    line("");

    line("inline std::unordered_set<std::string> getEngineEnumNames() {");
    {
        std::vector<std::string> names;
        std::set<std::string> seen;
        for (const auto& s : enums)
            if (seen.insert(s.name).second) names.push_back(s.name);
        std::sort(names.begin(), names.end());
        stringSet(names);
    }
    line("}");
    line("");

    line("inline std::unordered_set<std::string> getEngineBuiltinNames() {");
    {
        std::set<std::string> builtins;
        for (const auto& s : staticMethods) builtins.insert(s.parent);
        for (const auto& s : enums) builtins.insert(s.name);
        std::vector<std::string> names(builtins.begin(), builtins.end());
        stringSet(names);
    }
    line("}");
    line("");

    line("struct EngineAPISymbol {");
    line("    const char* name;");
    line("    const char* kind;   // maps to SuggestionKind");
    line("    const char* detail;");
    line("    const char* parent; // owning class/enum");
    line("};");
    line("");
    line("inline const std::vector<EngineAPISymbol>& getEngineAPISymbols() {");
    line("    static const std::vector<EngineAPISymbol> symbols = {");
    // Output category order mirrors generate_header()'s all_symbols.
    for (const auto* group : {&enums, &enumMembers, &classes, &constructors,
                              &methods, &staticMethods, &properties, &constants,
                              &events, &cppOnly}) {
        for (const auto& s : *group) {
            line("        {\"" + escapeCppString(s.name) + "\", \"" +
                 suggestionKind(s.kind) + "\", \"" + escapeCppString(s.detail) +
                 "\", \"" + escapeCppString(s.parent) + "\"},");
        }
    }
    line("    };");
    line("    return symbols;");
    line("}");
    line("");
    line("} // namespace quantum::editor");

    return lines.str();
}

std::string renderJson(const Manifest& manifest) {
    std::ostringstream o;
    o << "{\n";
    o << "  \"generated_by\": \"quantum-reflector\",\n";
    o << "  \"bindings_dir\": \"" << jsonEscape(displayPath(manifest.bindingsDir)) << "\",\n";
    o << "  \"engine_core_dir\": \"" << jsonEscape(displayPath(manifest.engineCoreDir)) << "\",\n";

    std::vector<Symbol> classes, constructors, methods, staticMethods, properties,
        constants, events, enums, enumMembers, cppOnly;
    for (const auto& s : manifest.symbols) {
        switch (s.kind) {
            case SymKind::Class: classes.push_back(s); break;
            case SymKind::Constructor: constructors.push_back(s); break;
            case SymKind::Method: methods.push_back(s); break;
            case SymKind::StaticMethod: staticMethods.push_back(s); break;
            case SymKind::Property: properties.push_back(s); break;
            case SymKind::Constant: constants.push_back(s); break;
            case SymKind::Event: events.push_back(s); break;
            case SymKind::Enum: enums.push_back(s); break;
            case SymKind::EnumMember: enumMembers.push_back(s); break;
            case SymKind::CppMethod: cppOnly.push_back(s); break;
        }
    }

    o << "  \"summary\": {";
    o << "\"classes\": " << classes.size();
    o << ", \"constructors\": " << constructors.size();
    o << ", \"methods\": " << methods.size();
    o << ", \"static_methods\": " << staticMethods.size();
    o << ", \"properties\": " << properties.size();
    o << ", \"constants\": " << constants.size();
    o << ", \"events\": " << events.size();
    o << ", \"enums\": " << enums.size();
    o << ", \"enum_members\": " << enumMembers.size();
    o << ", \"cpp_only_methods\": " << cppOnly.size();
    o << "},\n";

    auto childArray = [&](const char* field, const std::vector<Symbol>& pool,
                          const std::string& parentName) {
        std::vector<const Symbol*> matched;
        for (const auto& s : pool)
            if (s.parent == parentName) matched.push_back(&s);
        o << "    \"" << field << "\": [";
        for (size_t i = 0; i < matched.size(); ++i) {
            o << (i ? ",\n" : "\n");
            pushSymbolJson(o, *matched[i]);
        }
        if (!matched.empty()) o << "\n    ";
        o << "]";
    };

    o << "  \"classes\": [";
    for (size_t i = 0; i < classes.size(); ++i) {
        const auto& cls = classes[i];
        o << (i ? ",\n" : "\n") << "    {\"name\": \"" << jsonEscape(cls.name)
          << "\", \"detail\": \"" << jsonEscape(cls.detail) << "\", \"cpp_type\": \""
          << jsonEscape(cls.name) << "\",\n";
        childArray("constructors", constructors, cls.name); o << ",\n";
        childArray("methods", methods, cls.name); o << ",\n";
        childArray("static_methods", staticMethods, cls.name); o << ",\n";
        childArray("properties", properties, cls.name); o << ",\n";
        childArray("constants", constants, cls.name); o << ",\n";
        childArray("events", events, cls.name); o << ",\n";
        childArray("cpp_only_methods", cppOnly, cls.name); o << "\n    }";
    }
    if (!classes.empty()) o << "\n  ";
    o << "],\n";

    o << "  \"enums\": [";
    for (size_t i = 0; i < enums.size(); ++i) {
        o << (i ? ",\n" : "\n") << "    {\"name\": \"" << jsonEscape(enums[i].name)
          << "\", \"members\": [";
        bool first = true;
        for (const auto& m : enumMembers) {
            if (m.parent != enums[i].name) continue;
            o << (first ? "\n" : ",\n") << "      \"" << jsonEscape(m.name) << "\"";
            first = false;
        }
        if (!first) o << "\n    ";
        o << "]}";
    }
    if (!enums.empty()) o << "\n  ";
    o << "]\n}\n";

    return o.str();
}

std::string renderNodeCatalogJson(const Manifest& manifest) {
    auto trim = [](const std::string& s) {
        size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return std::string();
        size_t e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    };

    std::set<std::string> enumNames;
    for (const auto& s : manifest.symbols)
        if (s.kind == SymKind::Enum) enumNames.insert(s.name);

    auto simplifyType = [&](const std::string& raw) {
        std::string t = trim(raw);
        for (const char* prefix : {"const ", "struct ", "class ", "std::"}) {
            size_t n = std::char_traits<char>::length(prefix);
            while (t.rfind(prefix, 0) == 0) t.erase(0, n);
        }
        for (const char* suffix : {" const", "&", "*"}) {
            size_t n = std::char_traits<char>::length(suffix);
            while (t.size() >= n && t.compare(t.size() - n, n, suffix) == 0)
                t.erase(t.size() - n);
        }
        t = trim(t);
        while (t.find("std::") != std::string::npos)
            t.erase(t.find("std::"), 5);
        return t;
    };

    auto valueType = [&](const std::string& rawType) {
        const std::string t = simplifyType(rawType);
        if (t.empty() || t == "void") return std::string("void");
        if (t == "bool") return std::string("bool");
        if (t == "float" || t == "double") return std::string("float");
        if (t == "string" || t == "String") return std::string("string");
        if (t == "Entity") return std::string("entity");
        if (t == "Vector2") return std::string("vector2");
        if (t == "Vector3") return std::string("vector3");
        if (t == "Vector4") return std::string("vector4");
        if (t == "Color3") return std::string("color3");
        if (t == "Color4") return std::string("color4");
        if (t == "int" || t == "int8_t" || t == "int16_t" || t == "int32_t" ||
            t == "int64_t" || t == "uint8_t" || t == "uint16_t" ||
            t == "uint32_t" || t == "uint64_t" || t == "size_t" ||
            t == "unsigned" || t == "unsigned int" || t == "long" ||
            enumNames.count(t))
            return std::string("int");
        return std::string("object");
    };

    auto splitParams = [&](const std::string& raw) {
        std::vector<std::string> params;
        int depth = 0;
        std::string current;
        for (char c : raw) {
            if (c == '<' || c == '(') ++depth;
            else if (c == '>' || c == ')') depth = std::max(0, depth - 1);
            if (c == ',' && depth == 0) {
                params.push_back(trim(current));
                current.clear();
            } else {
                current.push_back(c);
            }
        }
        if (!trim(current).empty()) params.push_back(trim(current));
        if (params.size() == 1 && simplifyType(params[0]) == "void") params.clear();
        return params;
    };

    auto parseParam = [&](const std::string& raw) {
        std::string p = raw;
        size_t eq = p.find('=');
        if (eq != std::string::npos) p = trim(p.substr(0, eq));
        size_t space = p.find_last_of(" \t");
        std::string name;
        std::string type = p;
        if (space != std::string::npos) {
            name = trim(p.substr(space + 1));
            type = trim(p.substr(0, space));
            bool identifier = !name.empty();
            for (char c : name) {
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
                    identifier = false;
                    break;
                }
            }
            if (!identifier) {
                name.clear();
                type = p;
            }
        }
        return std::make_pair(name, simplifyType(type));
    };

    struct MethodInfo {
        const Symbol* symbol;
        std::vector<std::pair<std::string, std::string>> params;
        std::string returns;
        bool isStatic;
        bool cppOnly;
    };

    std::map<std::string, std::string> classBase;
    std::vector<std::string> classOrder;
    for (const auto& s : manifest.symbols) {
        if (s.kind != SymKind::Class) continue;
        if (classBase.insert({s.name, std::string()}).second)
            classOrder.push_back(s.name);
        size_t sep = s.detail.find(" : ");
        if (sep != std::string::npos)
            classBase[s.name] = trim(s.detail.substr(sep + 3));
    }

    std::map<std::string, std::vector<MethodInfo>> methods;
    std::map<std::string, std::vector<const Symbol*>> properties;
    std::map<std::string, std::vector<const Symbol*>> events;
    std::map<std::string, std::vector<const Symbol*>> constants;

    for (const auto& s : manifest.symbols) {
        if (s.kind == SymKind::Method || s.kind == SymKind::StaticMethod ||
            s.kind == SymKind::CppMethod) {
            std::string detail = s.detail;
            std::string returns;
            size_t arrow = detail.rfind(" -> ");
            if (arrow != std::string::npos) {
                returns = trim(detail.substr(arrow + 4));
                detail.erase(arrow);
            }
            size_t open = detail.find('(');
            size_t close = detail.rfind(')');
            std::string paramsRaw;
            if (open != std::string::npos && close != std::string::npos && close > open)
                paramsRaw = detail.substr(open + 1, close - open - 1);

            MethodInfo info{&s, {}, returns, s.kind == SymKind::StaticMethod,
                            s.kind == SymKind::CppMethod};
            for (const std::string& param : splitParams(paramsRaw))
                info.params.push_back(parseParam(param));
            methods[s.parent].push_back(std::move(info));
            if (!classBase.count(s.parent)) {
                classBase[s.parent] = std::string();
                classOrder.push_back(s.parent);
            }
        } else if (s.kind == SymKind::Property) {
            properties[s.parent].push_back(&s);
        } else if (s.kind == SymKind::Event) {
            events[s.parent].push_back(&s);
        } else if (s.kind == SymKind::Constant) {
            constants[s.parent].push_back(&s);
        }
    }

    auto capitalize = [](const std::string& s) {
        if (s.empty()) return s;
        std::string out = s;
        if (out[0] >= 'a' && out[0] <= 'z') out[0] = static_cast<char>(out[0] - 'a' + 'A');
        return out;
    };

    size_t methodCount = 0, propertyCount = 0, eventCount = 0;
    std::ostringstream o;
    o << "{\n";
    o << "  \"version\": 1,\n";
    o << "  \"generated_by\": \"quantum-reflector\",\n";
    o << "  \"classes\": [";

    bool firstClass = true;
    for (const std::string& className : classOrder) {
        const auto& classMethods = methods[className];
        const auto& classProperties = properties[className];
        const auto& classEvents = events[className];

        auto getterFor = [&](const std::string& propertyName, const char* prefix) {
            const std::string wanted = std::string(prefix) + capitalize(propertyName);
            for (const MethodInfo& m : classMethods) {
                if (m.symbol->name == wanted) return &m;
            }
            return static_cast<const MethodInfo*>(nullptr);
        };

        o << (firstClass ? "\n" : ",\n");
        firstClass = false;
        o << "    {\"name\": \"" << jsonEscape(className) << "\"";
        const std::string& base = classBase[className];
        if (!base.empty()) o << ", \"base\": \"" << jsonEscape(base) << "\"";
        o << ",\n      \"methods\": [";
        for (size_t i = 0; i < classMethods.size(); ++i) {
            const MethodInfo& m = classMethods[i];
            o << (i ? ",\n" : "\n");
            o << "        {\"name\": \"" << jsonEscape(m.symbol->name)
              << "\", \"kind\": \"" << (m.isStatic ? "static_method" : "method")
              << "\", \"signature\": \"" << jsonEscape(m.symbol->detail)
              << "\", \"returns\": \"" << jsonEscape(simplifyType(m.returns))
              << "\", \"return_type\": \"" << valueType(m.returns)
              << "\", \"cpp_only\": " << (m.cppOnly ? "true" : "false")
              << ", \"params\": [";
            for (size_t p = 0; p < m.params.size(); ++p) {
                const auto& [name, type] = m.params[p];
                o << (p ? ", " : "") << "{\"name\": \"" << jsonEscape(name)
                  << "\", \"type\": \"" << jsonEscape(type)
                  << "\", \"value_type\": \"" << valueType(type) << "\"}";
            }
            o << "]}";
            ++methodCount;
        }
        if (!classMethods.empty()) o << "\n      ";
        o << "],\n      \"properties\": [";
        for (size_t i = 0; i < classProperties.size(); ++i) {
            const Symbol& property = *classProperties[i];
            const MethodInfo* getter = getterFor(property.name, "get");
            if (!getter) getter = getterFor(property.name, "is");
            const MethodInfo* setter = getterFor(property.name, "set");
            o << (i ? ",\n" : "\n") << "        {\"name\": \""
              << jsonEscape(property.name) << "\", \"type\": \""
              << jsonEscape(getter ? simplifyType(getter->returns) : std::string())
              << "\", \"value_type\": \""
              << (getter ? valueType(getter->returns) : std::string("object"))
              << "\"";
            if (getter) o << ", \"getter\": \"" << jsonEscape(getter->symbol->name) << "\"";
            if (setter) o << ", \"setter\": \"" << jsonEscape(setter->symbol->name) << "\"";
            o << "}";
            ++propertyCount;
        }
        if (!classProperties.empty()) o << "\n      ";
        o << "],\n      \"events\": [";
        for (size_t i = 0; i < classEvents.size(); ++i) {
            o << (i ? ", " : "") << "\"" << jsonEscape(classEvents[i]->name) << "\"";
            ++eventCount;
        }
        o << "],\n      \"constants\": [";
        const auto& classConstants = constants[className];
        for (size_t i = 0; i < classConstants.size(); ++i)
            o << (i ? ", " : "") << "\"" << jsonEscape(classConstants[i]->name) << "\"";
        o << "]}";
    }
    if (!classOrder.empty()) o << "\n  ";
    o << "],\n";

    o << "  \"enums\": [";
    bool firstEnum = true;
    for (const auto& s : manifest.symbols) {
        if (s.kind != SymKind::Enum) continue;
        o << (firstEnum ? "\n" : ",\n") << "    {\"name\": \"" << jsonEscape(s.name)
          << "\", \"members\": [";
        bool firstMember = true;
        for (const auto& m : manifest.symbols) {
            if (m.kind != SymKind::EnumMember || m.parent != s.name) continue;
            o << (firstMember ? "\n" : ",\n") << "      \""
              << jsonEscape(m.name) << "\"";
            firstMember = false;
        }
        if (!firstMember) o << "\n    ";
        o << "]}";
        firstEnum = false;
    }
    if (!firstEnum) o << "\n  ";
    o << "],\n";

    o << "  \"summary\": {\"classes\": " << classOrder.size()
      << ", \"methods\": " << methodCount
      << ", \"properties\": " << propertyCount
      << ", \"events\": " << eventCount
      << ", \"nodes\": " << (methodCount + propertyCount + eventCount)
      << "}\n";
    o << "}\n";

    return o.str();
}

std::string renderLuaDocsMarkdown(const Manifest& manifest) {
    const ApiIndex idx(manifest);
    const auto& classOrder = idx.classOrder;
    const auto& enumOrder = idx.enumOrder;
    const auto& classBase = idx.classBase;
    const auto& ctors = idx.ctors;
    const auto& statics = idx.statics;
    const auto& methods = idx.methods;
    const auto& properties = idx.properties;
    const auto& events = idx.events;
    const auto& constants = idx.constants;
    const auto& enumMembers = idx.enumMembers;
    auto propertyType = [&](const Symbol& property) { return idx.propertyType(property); };
    auto hasSetter = [&](const Symbol& property) { return idx.hasSetter(property); };

    std::ostringstream o;
    o << "<!-- Auto-generated by quantum-reflector docs - DO NOT EDIT -->\n";
    o << "# Quantum Lua API\n\n";
    o << "Complete Lua API exposed by the engine bindings in "
         "`engine/core/script/binding`.\n\n";

    o << "## Contents\n\n";
    if (!classOrder.empty()) {
        o << "**Classes**\n\n";
        writeLinkList(o, classOrder, 6);
        o << "\n";
    }
    if (!enumOrder.empty()) {
        o << "**Enums**\n\n";
        writeLinkList(o, enumOrder, 6);
        o << "\n";
    }

    auto writeDetails = [&](const std::vector<const Symbol*>& symbols) {
        for (const Symbol* symbol : symbols) o << "- `" << symbol->detail << "`\n";
        o << "\n";
    };

    for (const std::string& className : classOrder) {
        o << "## " << className << "\n\n";
        const auto baseIt = classBase.find(className);
        if (baseIt != classBase.end() && !baseIt->second.empty())
            o << "Base: `" << baseIt->second << "`\n\n";

        const auto classCtors = ctors.find(className);
        if (classCtors != ctors.end() && !classCtors->second.empty()) {
            o << "### Constructors\n\n";
            writeDetails(classCtors->second);
        }

        const auto classStatics = statics.find(className);
        if (classStatics != statics.end() && !classStatics->second.empty()) {
            o << "### Static methods\n\n";
            writeDetails(classStatics->second);
        }

        const auto classMethods = methods.find(className);
        if (classMethods != methods.end() && !classMethods->second.empty()) {
            o << "### Methods\n\n";
            writeDetails(classMethods->second);
        }

        const auto classProperties = properties.find(className);
        if (classProperties != properties.end() && !classProperties->second.empty()) {
            o << "### Properties\n\n";
            for (const Symbol* property : classProperties->second) {
                o << "- `" << property->name << "` - `" << propertyType(*property)
                  << "`";
                if (!hasSetter(*property)) o << " (read-only)";
                o << "\n";
            }
            o << "\n";
        }

        const auto classEvents = events.find(className);
        if (classEvents != events.end() && !classEvents->second.empty()) {
            o << "### Events\n\n";
            for (const Symbol* event : classEvents->second)
                o << "- `" << event->name << "`\n";
            o << "\n";
        }

        const auto classConstants = constants.find(className);
        if (classConstants != constants.end() && !classConstants->second.empty()) {
            o << "### Constants\n\n";
            for (const Symbol* constant : classConstants->second)
                o << "- `" << constant->name << "`\n";
            o << "\n";
        }
    }

    if (!enumOrder.empty()) {
        o << "## Enums\n\n";
        for (const std::string& enumName : enumOrder) {
            o << "### " << enumName << "\n\n";
            const auto members = enumMembers.find(enumName);
            if (members != enumMembers.end())
                for (const Symbol* member : members->second)
                    o << "- `" << member->name << "`\n";
            o << "\n";
        }
    }

    return o.str();
}

std::string renderLuaTypesFile(const Manifest& manifest,
                               const std::string& usageChapter) {
    const ApiIndex idx(manifest);
    const std::set<std::string> allClasses(idx.classOrder.begin(),
                                           idx.classOrder.end());

    std::ostringstream o;
    o << "-- =============================================================================\n"
         "-- PART 1 - THE API AS DECLARATIONS\n"
         "-- =============================================================================\n"
         "-- Auto-generated by quantum-reflector types - DO NOT EDIT\n"
         "-- Every class, method, property, event, constant and enum that the bindings under "
      << displayPath(manifest.bindingsDir)
      << " expose.\n"
         "--\n"
         "-- The editor writes this file into a project as .quantum/engine-api/Types.lua when\n"
         "-- the project opens. A Lua language server reads it as definitions, so completions\n"
         "-- and hover work without the file ever being executed; a person can read it as the\n"
         "-- reference, then follow Part 2 at the end for worked examples.\n"
         "--\n"
         "-- It holds names and types but no behaviour: never require() it and never load it\n"
         "-- from a scene.\n"
         "--\n"
         "-- Fields marked (event) are engine hooks: assign a function to subscribe.\n"
         "-- Fields marked (read-only) have no bound setter.\n"
         "-- Inherited members are listed once, under the class the binding registers; the\n"
         "-- ---@class Child: Parent lines carry the rest.\n"
         "-- Enum members and constants carry positional placeholder numbers, because the\n"
         "-- reflector reads the names a binding registers, not the engine's values.\n"
         "-- Always compare them by name, never by number.\n"
         "---@diagnostic disable\n---@meta\n\n";

    auto writeField = [&](const std::string& name, const std::string& type,
                          const std::string& note) {
        o << "---@field " << name << " " << type;
        if (!note.empty()) o << " " << note;
        o << "\n";
    };

    // A bound name that is not a plain Lua identifier (the easing properties are
    // literally called `function`) still exists in Lua, only bracketed.
    auto skipField = [&](const char* what, const Symbol& s, const std::string& why) {
        o << "-- " << what << " " << s.detail << ": " << why << "\n";
    };

    auto bracketNote = [](const std::string& name) {
        return "read it as obj[\"" + name + "\"]";
    };

    // One Lua member per name: the C++ overloads the binding collapses onto a
    // single Lua function are listed as comments instead of redefining it.
    auto writeMethods = [&](const std::string& cls,
                            const std::vector<const Symbol*>& symbols, bool isStatic,
                            std::set<std::string>& declared) {
        std::set<std::string> seen;
        for (const Symbol* symbol : symbols) {
            if (!seen.insert(symbol->name).second) {
                o << "-- also: " << symbol->detail << "\n";
                continue;
            }
            if (!isPlainLuaIdentifier(symbol->name) || declared.count(symbol->name)) {
                o << "-- " << symbol->detail << "\n";
                continue;
            }
            declared.insert(symbol->name);
            const std::vector<LuaParam> params = splitTypedParams(symbol->detail);
            const std::string returns = luaReturnFor(symbol->detail);
            for (const LuaParam& p : params) {
                o << "---@param " << p.name << (p.optional ? "?" : "") << " "
                  << luaTypeFor(p.type) << "\n";
            }
            if (!returns.empty()) o << "---@return " << returns << "\n";
            o << "function " << cls << (isStatic ? "." : ":") << symbol->name << "("
              << luaParamNames(params) << ") end\n";
        }
    };

    for (const std::string& className : idx.classOrder) {
        if (!isPlainLuaIdentifier(className)) {
            o << "-- (skipped) class " << className << "\n\n";
            continue;
        }

        std::set<std::string> methodNames;
        for (const auto& bucket : {idx.statics, idx.methods}) {
            const auto it = bucket.find(className);
            if (it == bucket.end()) continue;
            for (const Symbol* s : it->second) methodNames.insert(s->name);
        }

        o << "---@class " << className;
        const auto baseIt = idx.classBase.find(className);
        if (baseIt != idx.classBase.end() && baseIt->second != className &&
            isPlainLuaIdentifier(baseIt->second) && allClasses.count(baseIt->second))
            o << ": " << baseIt->second;
        o << "\n";

        if (const auto it = idx.ctors.find(className); it != idx.ctors.end()) {
            for (const Symbol* ctor : it->second) {
                o << "---@overload "
                  << luaOverloadFun(splitTypedParams(ctor->detail), className) << "\n";
            }
        }

        if (const auto it = idx.properties.find(className); it != idx.properties.end()) {
            for (const Symbol* p : it->second) {
                if (!isPlainLuaIdentifier(p->name)) {
                    skipField("property", *p, bracketNote(p->name));
                    continue;
                }
                if (methodNames.count(p->name)) {
                    skipField("property", *p,
                              "a method of this class already owns the name");
                    continue;
                }
                writeField(p->name, luaTypeFor(idx.propertyType(*p)),
                           idx.hasSetter(*p) ? std::string() : "(read-only)");
            }
        }

        if (const auto it = idx.events.find(className); it != idx.events.end()) {
            for (const Symbol* e : it->second) {
                if (!isPlainLuaIdentifier(e->name)) {
                    skipField("event", *e, bracketNote(e->name));
                    continue;
                }
                if (methodNames.count(e->name)) {
                    skipField("event", *e,
                              "a method of this class already owns the name");
                    continue;
                }
                writeField(e->name, "function", "(event)");
            }
        }

        if (const auto it = idx.constants.find(className); it != idx.constants.end()) {
            for (const Symbol* c : it->second) {
                if (!isPlainLuaIdentifier(c->name)) {
                    skipField("constant", *c, bracketNote(c->name));
                    continue;
                }
                if (methodNames.count(c->name)) {
                    skipField("constant", *c,
                              "a method of this class already owns the name");
                    continue;
                }
                std::string type = "any";
                if (!c->getter.empty()) {
                    const Symbol* getter = idx.findMethod(c->parent, c->getter);
                    if (!getter) getter = idx.findMethodCI(c->parent, c->getter);
                    if (!getter) {
                        // Engine.scene reads through a static: getScene is
                        // itself Lua-bound, so it is not in the method index.
                        const auto s = idx.statics.find(c->parent);
                        if (s != idx.statics.end()) {
                            for (const Symbol* cand : s->second) {
                                if (cand->name == c->getter) {
                                    getter = cand;
                                    break;
                                }
                            }
                        }
                    }
                    if (getter) {
                        const std::string t = detailReturnType(getter->detail);
                        if (!t.empty() && t != "void") type = luaTypeFor(t);
                    }
                }
                if (type == "any" && !c->valueType.empty())
                    type = luaTypeFor(cleanTypeName(c->valueType));
                writeField(c->name, type, "(constant)");
            }
        }

        o << className << " = {}\n\n";

        std::set<std::string> declared;
        if (const auto it = idx.statics.find(className); it != idx.statics.end())
            writeMethods(className, it->second, true, declared);
        if (const auto it = idx.methods.find(className); it != idx.methods.end())
            writeMethods(className, it->second, false, declared);
        o << "\n";
    }

    if (!idx.enumOrder.empty()) {
        for (const std::string& enumName : idx.enumOrder) {
            if (!isPlainLuaIdentifier(enumName)) {
                o << "-- (skipped) enum " << enumName << "\n\n";
                continue;
            }
            const auto members = idx.enumMembers.find(enumName);
            // A namespace registered under a name the bindings also use as a
            // class cannot be redeclared as a table; extend the existing one.
            if (allClasses.count(enumName)) {
                if (members != idx.enumMembers.end()) {
                    std::set<std::string> written;
                    for (const Symbol* m : members->second) {
                        if (!isPlainLuaIdentifier(m->name) || !written.insert(m->name).second)
                            continue;
                        o << enumName << "." << m->name << " = "
                          << written.size() - 1 << "\n";
                    }
                }
                o << "\n";
                continue;
            }
            o << "---@enum " << enumName << "\n" << enumName << " = {\n";
            std::set<std::string> written;
            if (members != idx.enumMembers.end()) {
                for (const Symbol* m : members->second) {
                    if (!isPlainLuaIdentifier(m->name)) continue;
                    if (!written.insert(m->name).second) continue;
                    o << "    " << m->name << " = " << written.size() - 1 << ",\n";
                }
            }
            o << "}\n\n";
        }
    }

    // Names the bindings put on the Lua global namespace itself: a script uses
    // them with no class in front, so leaving them out makes a correct use look
    // like a typo to the editor. `= ...` is the main-chunk vararg expression,
    // which says "this exists" without inventing a value.
    if (const auto globals = idx.constants.find(""); globals != idx.constants.end()) {
        o << "-- Global values (no class in front of them)\n";
        for (const Symbol* c : globals->second) {
            if (!isPlainLuaIdentifier(c->name)) continue;
            o << "---@type "
              << (c->valueType.empty() ? std::string("any")
                                       : luaTypeFor(cleanTypeName(c->valueType)))
              << "\n" << c->name << " = ...\n";
        }
        o << "\n";
    }

    if (!usageChapter.empty()) {
        std::string chapter = usageChapter;
        if (chapter.back() != '\n') chapter.push_back('\n');
        o << chapter;
    }

    return o.str();
}

std::string renderLuaTypesHeader(const Manifest& manifest,
                                 const std::string& usage) {
    const std::string lua = renderLuaTypesFile(manifest, usage);

    // One narrow literal per source line, grouped into elements to stay clear
    // of MSVC's per-literal size limit. Bytes >= 0x80 go out as octal escapes
    // so the text survives a compiler that reads this header as ANSI.
    auto literal = [](const std::string& line) {
        std::string out = "\"";
        for (unsigned char c : line) {
            if (c == '\\' || c == '"') {
                out.push_back('\\');
                out.push_back(static_cast<char>(c));
            } else if (c == '\t') {
                out += "\\t";
            } else if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\%03o", c);
                out += buf;
            } else if (c < 0x80) {
                out.push_back(static_cast<char>(c));
            } else {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\%03o", c);
                out += buf;
            }
        }
        out += "\\n\"";
        return out;
    };

    std::vector<std::string> lines;
    {
        std::istringstream in(lua);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
    }

    std::ostringstream o;
    o << "// Auto-generated by quantum-reflector types - DO NOT EDIT\n"
         "// The Lua API types file the editor drops into a project as Types.lua.\n"
         "#pragma once\n\n#include <string>\n#include <vector>\n\n"
         "namespace quantum::editor {\n\n"
         "inline const std::vector<std::string>& getLuaTypesFileChunks() {\n"
         "    static const std::vector<std::string> chunks = {\n";

    size_t bytes = 0;
    bool first = true;
    for (const std::string& line : lines) {
        if (first) {
            o << "        " << literal(line);
            first = false;
        } else {
            o << "\n        " << literal(line);
        }
        bytes += line.size();
        if (bytes >= 16000) {
            o << ",";
            bytes = 0;
        }
    }
    o << "\n    };\n"
         "    return chunks;\n"
         "}\n\n"
         "inline std::string getLuaTypesFileText() {\n"
         "    std::string out;\n"
         "    for (const std::string& chunk : getLuaTypesFileChunks()) out += chunk;\n"
         "    return out;\n"
         "}\n\n"
         "} // namespace quantum::editor\n";

    return o.str();
}

bool checkAgainst(const std::string& rendered,
                  const std::string& fileContent,
                  std::string& report) {
    auto split = [](const std::string& s) {
        std::vector<std::string> lines;
        std::istringstream in(s);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
        return lines;
    };
    auto newLines = split(rendered);
    auto oldLines = split(fileContent);

    // The banner names the generator, so it legitimately differs between the
    // Python script and this tool; everything else must match exactly.
    auto isBanner = [](const std::string& l) {
        return l.rfind("// Auto-generated by ", 0) == 0;
    };

    std::ostringstream r;
    bool changed = false;

    size_t common = std::min(newLines.size(), oldLines.size());
    size_t diffCount = 0;
    for (size_t i = 0; i < common; ++i) {
        if (newLines[i] == oldLines[i]) continue;
        if (isBanner(newLines[i]) && isBanner(oldLines[i])) continue;
        changed = true;
        if (diffCount < 10) {
            r << "- old:" << (i + 1) << ": " << oldLines[i] << "\n";
            r << "+ new:" << (i + 1) << ": " << newLines[i] << "\n";
        }
        ++diffCount;
    }
    if (newLines.size() != oldLines.size()) {
        changed = true;
        r << "! line count: old=" << oldLines.size() << " new=" << newLines.size() << "\n";
        auto printExtra = [&](const std::vector<std::string>& lines, size_t from,
                              const char* sign) {
            size_t shown = 0;
            for (size_t i = from; i < lines.size() && shown < 10; ++i, ++shown)
                r << sign << ' ' << lines[i] << "\n";
        };
        if (newLines.size() > oldLines.size())
            printExtra(newLines, oldLines.size(), "+");
        else
            printExtra(oldLines, newLines.size(), "-");
    }

    if (changed) {
        if (diffCount > 10) r << "! ...and " << (diffCount - 10) << " more changed lines\n";
        report = r.str();
    }
    return !changed;
}

} // namespace refl
