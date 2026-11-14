// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "EngineApiIndex.h"

#include <algorithm>
#include <cctype>
#include <map>

namespace quantum::editor {

namespace {

    // The kinds quantum-reflector emits today. A kind outside this list is a generator
    // change the editor has not been taught about, so verify() reports it instead of
    // letting the tab filter it away.
    const std::vector<std::string> kKnownKinds = {
        "Class", "Enum", "EnumMember", "Function", "Method", "CppMethod", "Property", "Constant",
    };

    std::string lowered(const std::string& value) {
        std::string out;
        out.reserve(value.size());
        for (const char c : value) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    }

    // Owners are counted once per symbol; the map keeps the walk single-pass because the
    // tab rebuilds its filter box on every keystroke over four thousand rows.
    std::map<std::string, int> ownerCounts() {
        std::map<std::string, int> counts;
        for (const EngineAPISymbol& symbol : EngineApiIndex::symbols()) {
            if (symbol.parent[0] != '\0') ++counts[symbol.parent];
        }
        return counts;
    }

}

const std::vector<EngineAPISymbol>& EngineApiIndex::symbols() {
    return getEngineAPISymbols();
}

const std::vector<std::string>& EngineApiIndex::kinds() {
    static const std::vector<std::string> all = [] {
        std::vector<std::string> list = kKnownKinds;
        for (const EngineAPISymbol& symbol : getEngineAPISymbols()) {
            if (std::find(list.begin(), list.end(), std::string(symbol.kind)) == list.end()) {
                list.push_back(symbol.kind);
            }
        }
        return list;
    }();
    return all;
}

std::vector<std::pair<std::string, int>> EngineApiIndex::owners() {
    std::vector<std::pair<std::string, int>> rows;
    for (const auto& entry : ownerCounts()) rows.push_back({entry.first, entry.second});
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });
    return rows;
}

std::vector<int> EngineApiIndex::filter(const std::string& text, const std::string& kind,
                                        const std::string& owner) {
    const std::string needle = lowered(text);
    const std::string wantKind = lowered(kind);
    const std::string wantOwner = lowered(owner);
    std::vector<int> rows;
    const std::vector<EngineAPISymbol>& symbols = getEngineAPISymbols();
    rows.reserve(symbols.size());
    for (int index = 0; index < static_cast<int>(symbols.size()); ++index) {
        const EngineAPISymbol& symbol = symbols[index];
        if (!wantKind.empty() && wantKind != lowered(symbol.kind)) continue;
        if (!wantOwner.empty() && wantOwner != lowered(symbol.parent)) continue;
        if (!needle.empty() && lowered(symbol.name).find(needle) == std::string::npos &&
            lowered(symbol.parent).find(needle) == std::string::npos &&
            lowered(symbol.detail).find(needle) == std::string::npos) {
            continue;
        }
        rows.push_back(index);
    }
    return rows;
}

std::vector<int> EngineApiIndex::kindCounts() {
    std::map<std::string, int> counts;
    for (const EngineAPISymbol& symbol : getEngineAPISymbols()) ++counts[symbol.kind];
    std::vector<int> out;
    out.reserve(kinds().size());
    for (const std::string& kind : kinds()) {
        const auto it = counts.find(kind);
        out.push_back(it == counts.end() ? 0 : it->second);
    }
    return out;
}

std::vector<const EngineAPISymbol*> EngineApiIndex::membersOf(const std::string& owner) {
    std::vector<const EngineAPISymbol*> rows;
    const std::string want = lowered(owner);
    for (const EngineAPISymbol& symbol : getEngineAPISymbols()) {
        if (lowered(symbol.parent) == want) rows.push_back(&symbol);
    }
    return rows;
}

std::vector<const EngineAPISymbol*> EngineApiIndex::named(const std::string& name) {
    std::vector<const EngineAPISymbol*> rows;
    const std::string want = lowered(name);
    for (const EngineAPISymbol& symbol : getEngineAPISymbols()) {
        if (lowered(symbol.name) == want) rows.push_back(&symbol);
    }
    return rows;
}

std::vector<std::string> EngineApiIndex::verify() {
    std::vector<std::string> problems;
    const std::vector<EngineAPISymbol>& symbols = getEngineAPISymbols();
    if (symbols.empty()) {
        problems.push_back("no symbols at all: engine_api_suggestions.h is empty");
        return problems;
    }
    std::map<std::string, int> counts;
    for (const EngineAPISymbol& symbol : symbols) {
        ++counts[symbol.kind];
        if (symbol.name[0] == '\0') {
            problems.push_back(std::string("a ") + symbol.kind + " row has no name");
            continue;
        }
        if (std::find(kKnownKinds.begin(), kKnownKinds.end(), std::string(symbol.kind)) ==
            kKnownKinds.end()) {
            problems.push_back(std::string(symbol.name) + ": unknown kind '" + symbol.kind + "'");
        }
        const bool needsOwner = std::string(symbol.kind) == "EnumMember" ||
                                std::string(symbol.kind) == "Property" ||
                                std::string(symbol.kind) == "Method" ||
                                std::string(symbol.kind) == "CppMethod";
        if (needsOwner && symbol.parent[0] == '\0') {
            problems.push_back(std::string(symbol.name) + ": a " + symbol.kind +
                               " with no owner cannot be completed by the script editor");
        }
    }
    // A kind that dropped to zero means the reflector changed its mind, and a tab filter
    // that silently lists nothing looks like a broken UI instead of a stale header.
    for (const std::string& kind : kKnownKinds) {
        if (counts.find(kind) == counts.end()) {
            problems.push_back("kind " + kind + " has no symbol: the reflector stopped emitting it");
        }
    }
    return problems;
}

const char* EngineApiIndex::kindLabel(const std::string& kind) {
    if (kind == "Class") return "C";
    if (kind == "Enum") return "E";
    if (kind == "EnumMember") return "e";
    if (kind == "Function") return "F";
    if (kind == "Method") return "M";
    if (kind == "CppMethod") return "m";
    if (kind == "Property") return "P";
    if (kind == "Constant") return "K";
    return "?";
}

} // namespace quantum::editor
