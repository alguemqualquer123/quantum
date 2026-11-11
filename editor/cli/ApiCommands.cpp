// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "ApiCommands.h"

#include "EngineApiIndex.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace quantum::editor::cli {

namespace {

    using quantum::editor::EngineApiIndex;
    using quantum::editor::EngineAPISymbol;

    void usage(const char* executableName) {
        std::cout
            << "usage: " << (executableName ? executableName : "quantum-editor-cmd")
            << " api <stats | list | search <text> | show <symbol> | owners | verify> [options]\n"
            << "\n"
            << "  stats           how many symbols each kind holds, and how many owners\n"
            << "  list            the symbols themselves (200 rows unless --limit says otherwise)\n"
            << "  search <text>   substring over name, owner and signature, case-insensitive\n"
            << "  show <symbol>   one symbol, its signature, and the members it owns\n"
            << "  owners          every owner with its member count, biggest first\n"
            << "  verify          shape check on the generated metadata; exits 1 on trouble\n"
            << "\n"
            << "options:\n"
            << "  --kind <K>      keep one kind (Class, Enum, EnumMember, Function, Method,\n"
            << "                  CppMethod, Property, Constant)\n"
            << "  --owner <O>     keep one owner, e.g. --owner Engine\n"
            << "  --limit <N>     rows to print, 0 prints everything (default 200)\n"
            << "  --json          one JSON object per row\n"
            << "  --list-kinds    print the kind names and exit\n"
            << "\n"
            << "Read-only: the data is compiled into the executable from the generated\n"
            << "engine_api_suggestions.h, so nothing here touches a project or a settings file.\n";
    }

    // Column padding by hand: these rows go into a log, and a log that wraps in the middle
    // of a signature is useless for comparing two engine versions.
    std::string pad(const std::string& text, size_t width) {
        return text.size() >= width ? text + " " : text + std::string(width - text.size(), ' ');
    }

    std::string jsonEscape(const std::string& text) {
        std::string out;
        out.reserve(text.size() + 8);
        for (const char c : text) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\t': out += "\\t"; break;
                default: out += c; break;
            }
        }
        return out;
    }

    struct Options {
        std::string kind;
        std::string owner;
        long limit = 200;
        bool json = false;
    };

    void printRow(const EngineAPISymbol& symbol, const Options& options) {
        if (options.json) {
            std::cout << "{\"name\": \"" << jsonEscape(symbol.name) << "\", \"kind\": \""
                      << jsonEscape(symbol.kind) << "\", \"owner\": \"" << jsonEscape(symbol.parent)
                      << "\", \"signature\": \"" << jsonEscape(symbol.detail) << "\"}\n";
            return;
        }
        std::cout << pad(symbol.kind, 12) << pad(symbol.parent[0] ? symbol.parent : "-", 24)
                  << pad(symbol.name, 30) << symbol.detail << "\n";
    }

    int printRows(const std::vector<int>& rows, const Options& options, const std::string& header) {
        long shown = 0;
        const std::vector<EngineAPISymbol>& symbols = EngineApiIndex::symbols();
        for (const int index : rows) {
            if (options.limit <= 0 || shown < options.limit) {
                printRow(symbols[index], options);
                ++shown;
            }
        }
        if (!options.json) {
            std::cout << "\n" << header << " " << rows.size() << " symbol(s)";
            if (shown < static_cast<long>(rows.size())) {
                std::cout << ", showing " << shown << " (--limit 0 prints all)";
            }
            std::cout << "\n";
        }
        return rows.empty() ? 1 : 0;
    }

    int reportStats(const Options& options) {
        const std::vector<std::string>& kinds = EngineApiIndex::kinds();
        const std::vector<int> counts = EngineApiIndex::kindCounts();
        const size_t owners = EngineApiIndex::owners().size();
        if (options.json) {
            std::cout << "{\"total\": " << EngineApiIndex::symbols().size() << ", \"owners\": "
                      << owners << ", \"types\": " << getEngineTypeNames().size()
                      << ", \"enums\": " << getEngineEnumNames().size()
                      << ", \"builtins\": " << getEngineBuiltinNames().size() << ", \"kinds\": {";
            for (size_t i = 0; i < kinds.size(); ++i) {
                std::cout << (i ? ", " : "") << "\"" << jsonEscape(kinds[i]) << "\": " << counts[i];
            }
            std::cout << "}}\n";
            return EngineApiIndex::symbols().empty() ? 1 : 0;
        }
        std::cout << "engine api: " << EngineApiIndex::symbols().size() << " symbols\n";
        for (size_t i = 0; i < kinds.size(); ++i) {
            std::cout << "  " << pad(kinds[i], 14) << counts[i] << "\n";
        }
        std::cout << "  " << pad("owners", 14) << owners << "\n"
                  << "  " << pad("type names", 14) << getEngineTypeNames().size() << "\n"
                  << "  " << pad("enum names", 14) << getEngineEnumNames().size() << "\n"
                  << "  " << pad("builtins", 14) << getEngineBuiltinNames().size() << "\n";
        return EngineApiIndex::symbols().empty() ? 1 : 0;
    }

    int reportOwners(const Options& options) {
        const std::vector<std::pair<std::string, int>> rows = EngineApiIndex::owners();
        long shown = 0;
        for (const auto& row : rows) {
            if (!options.owner.empty() && row.first != options.owner) continue;
            if (options.limit > 0 && shown >= options.limit) continue;
            if (options.json) {
                std::cout << "{\"owner\": \"" << jsonEscape(row.first) << "\", \"members\": "
                          << row.second << "}\n";
            } else {
                std::cout << pad(std::to_string(row.second), 8) << row.first << "\n";
            }
            ++shown;
        }
        if (!options.json) std::cout << "\n" << rows.size() << " owner(s), showing " << shown << "\n";
        return shown > 0 ? 0 : 1;
    }

    // show <symbol> also lists what the symbol owns, because the useful question after
    // "does Texture exist" is "what can I call on it".
    int showSymbol(const std::string& wanted) {
        const std::vector<const EngineAPISymbol*> hits = EngineApiIndex::named(wanted);
        if (hits.empty()) {
            std::cout << "api: no symbol named '" << wanted << "'\n";
            const std::vector<int> near = EngineApiIndex::filter(wanted, std::string(), std::string());
            for (size_t i = 0; i < near.size() && i < 10; ++i) {
                std::cout << "  did you mean: " << EngineApiIndex::symbols()[near[i]].name << "\n";
            }
            return 1;
        }
        for (const EngineAPISymbol* symbol : hits) {
            std::cout << pad(symbol->kind, 12) << pad(symbol->parent[0] ? symbol->parent : "-", 24)
                      << symbol->detail << "\n";
        }
        const std::vector<const EngineAPISymbol*> members = EngineApiIndex::membersOf(wanted);
        for (const EngineAPISymbol* member : members) {
            std::cout << "  " << pad(member->kind, 12) << pad(member->name, 30)
                      << member->detail << "\n";
        }
        std::cout << (members.empty() ? "\n(no members: not a class or enum)\n"
                                      : "\n" + std::to_string(members.size()) +
                                            " member(s) of " + wanted + "\n");
        return 0;
    }

    int reportVerify() {
        const std::vector<std::string> problems = EngineApiIndex::verify();
        for (const std::string& problem : problems) std::cout << "api: " << problem << "\n";
        std::cout << (problems.empty() ? "OK " : "FAILED ") << EngineApiIndex::symbols().size()
                  << " symbols checked, " << problems.size() << " problem(s)\n";
        return problems.empty() ? 0 : 1;
    }

    bool readValue(int argc, char** argv, int& index, const std::string& flag, std::string& out) {
        if (index + 1 >= argc) {
            std::cout << "api: " << flag << " needs a value\n";
            return false;
        }
        out = argv[++index];
        return true;
    }

} // namespace

    int runApiCommand(int argc, char** argv, const char* executableName) {
        Options options;
        std::string subcommand;
        std::string wanted;

        // The caller shifts argv past the executable name but leaves the "api" token in
        // argv[0], so the subcommand starts at 1 - the same shape the other subcommands use.
        if (argc >= 2 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0 ||
                          std::strcmp(argv[1], "help") == 0)) {
            usage(executableName);
            return 0;
        }

        for (int i = 1; i < argc; ++i) {
            const std::string arg(argv[i] ? argv[i] : "");
            if (arg == "--kind") { if (!readValue(argc, argv, i, arg, options.kind)) return 1; continue; }
            if (arg == "--owner") { if (!readValue(argc, argv, i, arg, options.owner)) return 1; continue; }
            if (arg == "--limit") {
                std::string value;
                if (!readValue(argc, argv, i, arg, value)) return 1;
                try { options.limit = std::stol(value); } catch (...) { options.limit = 200; }
                if (options.limit < 0) options.limit = 0;
                continue;
            }
            if (arg == "--json") { options.json = true; continue; }
            if (arg == "--list-kinds") {
                for (const std::string& kind : EngineApiIndex::kinds()) std::cout << kind << "\n";
                return 0;
            }
            if (!arg.empty() && arg[0] == '-') {
                std::cout << "api: unknown option " << arg << "\n";
                usage(executableName);
                return 1;
            }
            if (subcommand.empty()) subcommand = arg;
            else if (wanted.empty()) wanted = arg;
            else wanted += " " + arg;
        }

        if (subcommand.empty()) { usage(executableName); return 1; }

        const std::vector<std::string>& kinds = EngineApiIndex::kinds();
        if (!options.kind.empty() &&
            std::find(kinds.begin(), kinds.end(), options.kind) == kinds.end()) {
            std::cout << "api: unknown kind '" << options.kind << "'; --list-kinds prints the set\n";
            return 1;
        }

        if (subcommand == "stats") return reportStats(options);
        if (subcommand == "verify") return reportVerify();
        if (subcommand == "owners") return reportOwners(options);
        if (subcommand == "list") {
            return printRows(EngineApiIndex::filter(std::string(), options.kind, options.owner),
                             options, "list:");
        }
        if (subcommand == "search") {
            if (wanted.empty()) {
                std::cout << "api: search needs a text\n";
                return 1;
            }
            return printRows(EngineApiIndex::filter(wanted, options.kind, options.owner),
                             options, "search '" + wanted + "':");
        }
        if (subcommand == "show") {
            if (wanted.empty()) {
                std::cout << "api: show needs a symbol name\n";
                return 1;
            }
            return showSymbol(wanted);
        }

        std::cout << "api: unknown subcommand '" << subcommand << "'\n";
        usage(executableName);
        return 1;
    }

} // namespace quantum::editor::cli
