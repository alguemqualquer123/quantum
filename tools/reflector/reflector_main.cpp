// quantum-reflector: standalone CLI that regenerates engine_api_suggestions.h
// from LuaBridge binding files. Pure C++17 std; no engine code, no deps.
//
// Subcommands:
//   reflect --bindings <dir> [--engine-core <dir>] --out <header> [--json <file>]
//   catalog --bindings <dir> [--engine-core <dir>] --out <json>
//   docs    --bindings <dir> [--engine-core <dir>] --out <markdown>
//   types   --bindings <dir> [--engine-core <dir>] --out <lua> [--usage <lua>]
//   check   --bindings <dir> [--engine-core <dir>] --against <header>
#include "reflector.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void printUsage(std::ostream& os) {
    os <<
R"(quantum-reflector - regenerate engine API suggestions header from Lua bindings.

Usage:
  quantum-reflector reflect --bindings <dir> [--engine-core <dir>] --out <header> [--json <file>]
  quantum-reflector catalog --bindings <dir> [--engine-core <dir>] --out <json>
  quantum-reflector docs    --bindings <dir> [--engine-core <dir>] --out <markdown>
  quantum-reflector types   --bindings <dir> [--engine-core <dir>] --out <lua|.h> [--usage <lua>]
  quantum-reflector check   --bindings <dir> [--engine-core <dir>] --against <header>
  quantum-reflector --help | -h

Commands:
  reflect   Parse binding/*.cpp (+ C++ headers for signatures) and emit the
            generated header (--out) and/or a JSON API manifest (--json).
  catalog   Emit the structured visual-scripting node catalog (classes,
            methods with typed params, properties, events, enums).
  docs      Emit a Markdown Lua API reference (classes, methods, properties,
            events, constants and enums).
  types     Emit the EmmyLua-annotated Types.lua the editor drops into a
            project: the same surface as `docs`, but as declarations a Lua
            language server can complete. --out ending in .h/.hpp wraps it as
            an embedded string header, anything else writes plain Lua.
  check     Regenerate the header in memory and exit non-zero if it would
            differ from --against <header> (prints a diff-style summary).

Options:
  --bindings <dir>     Directory with LuaBridge binding *.cpp files (required).
  --engine-core <dir>  Directory scanned recursively for *.h public-method
                       signatures. Default: <bindings>/../.. (like the old
                       generate_api_suggestions.py).
  --out <path>         Output path for the generated header, catalog, docs or
                       types file (reflect/catalog/docs/types).
  --json <file>        Output path for the machine-readable API manifest.
  --usage <file.lua>   Hand-written usage chapter appended to the types file.
  --against <header>   Existing header to compare against (check only).

Exit codes: 0 ok / up-to-date, 1 check found changes or no binding files,
            2 usage error, 3 I/O error.
)";
}

struct Options {
    std::string command;
    std::string bindings;
    std::string engineCore;
    std::string out;
    std::string json;
    std::string against;
    std::string usage;
};

bool parseArgs(int argc, char** argv, Options& opt, std::string& err) {
    std::vector<std::string> args(argv + 1, argv + argc);
    size_t i = 0;

    auto needValue = [&](const std::string& flag, std::string& dst) {
        if (i + 1 >= args.size()) {
            err = "missing value for " + flag;
            return false;
        }
        dst = args[++i];
        return true;
    };

    if (!args.empty() && (args[0] == "--help" || args[0] == "-h" || args[0] == "help")) {
        return true; // handled by caller
    }
    if (args.empty()) {
        err = "no command given";
        return false;
    }
    opt.command = args[i++];
    if (opt.command != "reflect" && opt.command != "catalog" &&
        opt.command != "docs" && opt.command != "types" &&
        opt.command != "check") {
        err = "unknown command: " + opt.command;
        return false;
    }

    while (i < args.size()) {
        const std::string& a = args[i];
        if (a == "--bindings") { if (!needValue(a, opt.bindings)) return false; }
        else if (a == "--engine-core") { if (!needValue(a, opt.engineCore)) return false; }
        else if (a == "--out") { if (!needValue(a, opt.out)) return false; }
        else if (a == "--json") { if (!needValue(a, opt.json)) return false; }
        else if (a == "--usage") { if (!needValue(a, opt.usage)) return false; }
        else if (a == "--against") { if (!needValue(a, opt.against)) return false; }
        else if (a == "--help" || a == "-h") { opt.command.clear(); return true; }
        else {
            err = "unknown option: " + a;
            return false;
        }
        ++i;
    }

    if (opt.bindings.empty()) {
        err = "--bindings <dir> is required";
        return false;
    }
    if (opt.command == "reflect" && opt.out.empty() && opt.json.empty()) {
        err = "reflect needs at least one of --out <header> / --json <file>";
        return false;
    }
    if (opt.command == "catalog" && opt.out.empty()) {
        err = "catalog needs --out <json>";
        return false;
    }
    if (opt.command == "docs" && opt.out.empty()) {
        err = "docs needs --out <markdown>";
        return false;
    }
    if (opt.command == "types" && opt.out.empty()) {
        err = "types needs --out <lua or .h>";
        return false;
    }
    if (opt.command == "check" && opt.against.empty()) {
        err = "check needs --against <header>";
        return false;
    }
    return true;
}

void progress(const std::string& file, size_t count) {
    std::printf("  Parsed %s: %zu symbols\n", file.c_str(), count);
}

bool writeFileText(const std::string& path, const std::string& content) {
    std::error_code ec;
    fs::path p(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        std::cerr << "error: cannot write: " << path << "\n";
        return false;
    }
    f << content;
    return f.good();
}

void printSummary(const refl::Manifest& m) {
    size_t classes = 0, methods = 0, cppOnly = 0, statics = 0, props = 0,
           consts = 0, events = 0, enums = 0, enumMembers = 0, ctors = 0;
    for (const auto& s : m.symbols) {
        switch (s.kind) {
            case refl::SymKind::Class: ++classes; break;
            case refl::SymKind::Constructor: ++ctors; break;
            case refl::SymKind::Method: ++methods; break;
            case refl::SymKind::StaticMethod: ++statics; break;
            case refl::SymKind::Property: ++props; break;
            case refl::SymKind::Constant: ++consts; break;
            case refl::SymKind::Event: ++events; break;
            case refl::SymKind::Enum: ++enums; break;
            case refl::SymKind::EnumMember: ++enumMembers; break;
            case refl::SymKind::CppMethod: ++cppOnly; break;
        }
    }
    std::printf(
        "  Classes: %zu, Methods: %zu, CppMethods: %zu, StaticMethods: %zu, "
        "Properties: %zu, Constants: %zu, Events: %zu, Enums: %zu, "
        "EnumMembers: %zu\n",
        classes, methods, cppOnly, statics, props, consts, events, enums,
        enumMembers);
}

} // namespace

static int runMain(int argc, char** argv);

int main(int argc, char** argv) {
    try {
        return runMain(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 4;
    } catch (...) {
        std::cerr << "fatal: unknown exception\n";
        return 4;
    }
}

static int runMain(int argc, char** argv) {
    Options opt;
    std::string err;
    if (!parseArgs(argc, argv, opt, err)) {
        std::cerr << "error: " << err << "\n\n";
        printUsage(std::cerr);
        return 2;
    }
    if (opt.command.empty()) {
        printUsage(std::cout);
        return 0;
    }

    refl::Manifest manifest;
    if (!refl::buildManifest(opt.bindings, opt.engineCore, manifest, err,
                             opt.command == "reflect" ? &progress : nullptr)) {
        std::cerr << "error: " << err << "\n";
        return 1;
    }

    if (opt.command == "catalog") {
        std::string json = refl::renderNodeCatalogJson(manifest);
        if (!writeFileText(opt.out, json)) return 3;
        std::printf("[quantum-reflector] Generated node catalog %s\n", opt.out.c_str());
        printSummary(manifest);
        return 0;
    }

    if (opt.command == "docs") {
        std::string markdown = refl::renderLuaDocsMarkdown(manifest);
        if (!writeFileText(opt.out, markdown)) return 3;
        std::printf("[quantum-reflector] Generated Lua API docs %s\n", opt.out.c_str());
        printSummary(manifest);
        return 0;
    }

    if (opt.command == "types") {
        std::string usage;
        if (!opt.usage.empty()) {
            std::string readErr;
            if (!refl::readFile(opt.usage, usage, readErr)) {
                std::cerr << "error: " << readErr << "\n";
                return 3;
            }
        }
        const std::string ext = fs::path(opt.out).extension().generic_string();
        const bool headerForm = ext == ".h" || ext == ".hpp";
        const std::string text = headerForm
                                     ? refl::renderLuaTypesHeader(manifest, usage)
                                     : refl::renderLuaTypesFile(manifest, usage);
        if (!writeFileText(opt.out, text)) return 3;
        std::printf("[quantum-reflector] Generated %s %s\n",
                    headerForm ? "Lua types header" : "Lua types file",
                    opt.out.c_str());
        printSummary(manifest);
        return 0;
    }

    if (opt.command == "reflect") {
        if (!opt.out.empty()) {
            std::string header = refl::renderHeader(manifest);
            if (!writeFileText(opt.out, header)) return 3;
            std::printf("[quantum-reflector] Generated %s\n", opt.out.c_str());
            printSummary(manifest);
        }
        if (!opt.json.empty()) {
            std::string json = refl::renderJson(manifest);
            if (!writeFileText(opt.json, json)) return 3;
            std::printf("[quantum-reflector] Generated %s\n", opt.json.c_str());
        }
        return 0;
    }

    // check
    std::string existing;
    if (!refl::readFile(opt.against, existing, err)) {
        // readFile normalizes newlines; that is fine for comparison but the
        // file must exist.
        std::cerr << "error: " << err << "\n";
        return 3;
    }
    std::string rendered = refl::renderHeader(manifest);
    std::string report;
    if (refl::checkAgainst(rendered, existing, report)) {
        std::printf("[quantum-reflector] %s is up to date\n", opt.against.c_str());
        return 0;
    }
    std::cout << "[quantum-reflector] Regeneration would change " << opt.against
              << ":\n" << report;
    return 1;
}
