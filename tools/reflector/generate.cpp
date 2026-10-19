// doriax-reflector: manifest assembly + header/JSON rendering + CI check.
#include "reflector.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace refl {
namespace {

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

    line("// Auto-generated by doriax-reflector - DO NOT EDIT");
    line("// Parsed from engine/core/script/binding/*.cpp");
    line("#pragma once");
    line("");
    line("#include <string>");
    line("#include <vector>");
    line("#include <unordered_set>");
    line("");
    line("namespace doriax::editor {");
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
    line("} // namespace doriax::editor");

    return lines.str();
}

std::string renderJson(const Manifest& manifest) {
    std::ostringstream o;
    o << "{\n";
    o << "  \"generated_by\": \"doriax-reflector\",\n";
    o << "  \"bindings_dir\": \"" << jsonEscape(manifest.bindingsDir) << "\",\n";
    o << "  \"engine_core_dir\": \"" << jsonEscape(manifest.engineCoreDir) << "\",\n";

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
