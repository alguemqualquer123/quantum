// doriax-reflector: standalone CLI that regenerates engine_api_suggestions.h
// from LuaBridge binding files. Pure C++17 std; no engine code, no deps.
//
// Subcommands:
//   reflect --bindings <dir> [--engine-core <dir>] --out <header> [--json <file>]
//   check   --bindings <dir> [--engine-core <dir>] --against <header>
#include "reflector.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void printUsage(std::ostream& os) {
    os <<
R"(doriax-reflector - regenerate engine API suggestions header from Lua bindings.

Usage:
  doriax-reflector reflect --bindings <dir> [--engine-core <dir>] --out <header> [--json <file>]
  doriax-reflector check   --bindings <dir> [--engine-core <dir>] --against <header>
  doriax-reflector --help | -h

Commands:
  reflect   Parse binding/*.cpp (+ C++ headers for signatures) and emit the
            generated header (--out) and/or a JSON API manifest (--json).
  check     Regenerate the header in memory and exit non-zero if it would
            differ from --against <header> (prints a diff-style summary).

Options:
  --bindings <dir>     Directory with LuaBridge binding *.cpp files (required).
  --engine-core <dir>  Directory scanned recursively for *.h public-method
                       signatures. Default: <bindings>/../.. (like the old
                       generate_api_suggestions.py).
  --out <header>       Output path for the generated header (reflect only).
  --json <file>        Output path for the machine-readable API manifest.
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
    if (opt.command != "reflect" && opt.command != "check") {
        err = "unknown command: " + opt.command;
        return false;
    }

    while (i < args.size()) {
        const std::string& a = args[i];
        if (a == "--bindings") { if (!needValue(a, opt.bindings)) return false; }
        else if (a == "--engine-core") { if (!needValue(a, opt.engineCore)) return false; }
        else if (a == "--out") { if (!needValue(a, opt.out)) return false; }
        else if (a == "--json") { if (!needValue(a, opt.json)) return false; }
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

int main(int argc, char** argv) {
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

    if (opt.command == "reflect") {
        if (!opt.out.empty()) {
            std::string header = refl::renderHeader(manifest);
            if (!writeFileText(opt.out, header)) return 3;
            std::printf("[doriax-reflector] Generated %s\n", opt.out.c_str());
            printSummary(manifest);
        }
        if (!opt.json.empty()) {
            std::string json = refl::renderJson(manifest);
            if (!writeFileText(opt.json, json)) return 3;
            std::printf("[doriax-reflector] Generated %s\n", opt.json.c_str());
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
        std::printf("[doriax-reflector] %s is up to date\n", opt.against.c_str());
        return 0;
    }
    std::cout << "[doriax-reflector] Regeneration would change " << opt.against
              << ":\n" << report;
    return 1;
}
