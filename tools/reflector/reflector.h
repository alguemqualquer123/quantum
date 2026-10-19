// doriax-reflector: shared declarations.
// Pure C++17 std-only; no engine code, no third-party deps.
#ifndef DORIAX_REFLECTOR_H
#define DORIAX_REFLECTOR_H

#include <map>
#include <string>
#include <vector>

namespace refl {

// ── Symbol model (mirrors APISymbol in generate_api_suggestions.py) ────────

enum class SymKind {
    Enum,
    EnumMember,
    Class,
    Constructor,
    Method,
    StaticMethod,
    Property,
    Constant,
    Event,
    CppMethod,
};

struct Symbol {
    std::string name;
    SymKind kind;
    std::string detail;
    std::string parent;
};

const char* kindName(SymKind kind);                 // internal kind string
const char* suggestionKind(SymKind kind);           // SuggestionKind mapping
std::string escapeCppString(const std::string& in); // \" and \\ escaping

// ── File helpers ───────────────────────────────────────────────────────────

// Reads whole file (text). Returns false and sets err on failure.
bool readFile(const std::string& path, std::string& out, std::string& err);

// Sorted (ascending, by full path) list of *.cpp files directly in dir.
std::vector<std::string> listBindingCppFiles(const std::string& dir);

// Recursive list of *.h files under dir (implementation-defined order; the
// generator is order-insensitive for headers).
std::vector<std::string> listHeadersRecursive(const std::string& dir);

// ── Parsing ────────────────────────────────────────────────────────────────

// Strip C++ comments with the same semantics as the Python regexes:
// first remove `//` to end-of-line, then remove `/* ... */` spans.
std::string stripComments(const std::string& content);

// Parse one binding .cpp file's already-stripped content into symbols.
std::vector<Symbol> parseBindingContent(const std::string& content);

struct Overload {
    std::string params;   // simplified "Type name, ..." string
    std::string returns;  // simplified return type or ""
};

struct ClassInfo {
    std::vector<std::string> methodOrder;           // first-seen order
    std::map<std::string, std::vector<Overload>> overloads;
};

// Walk *.h files under engineCoreDir, extract public method declarations
// per class (equivalent of parse_cpp_headers()).
std::map<std::string, ClassInfo> parseCppHeaders(const std::string& engineCoreDir);

// Simplification helpers (shared with tests / JSON arg counting).
std::string simplifyParam(const std::string& param);
std::string simplifyParams(const std::string& paramsStr);
std::string simplifyReturnType(const std::string& ret);

// ── Manifest assembly (mirrors main() of the Python script) ───────────────

struct Manifest {
    std::vector<Symbol> symbols;  // deduplicated, in output order
    std::string bindingsDir;
    std::string engineCoreDir;
    std::vector<std::pair<std::string, size_t>> perFileCounts; // basename, symbols
};

// Parses bindings + headers, enriches details with C++ signatures and adds
// CppMethod symbols. onProgress(file, symbolCount) is called per binding file.
bool buildManifest(const std::string& bindingsDir,
                   const std::string& engineCoreDir,
                   Manifest& out,
                   std::string& err,
                   void (*onProgress)(const std::string&, size_t));

// ── Rendering / checking ──────────────────────────────────────────────────

std::string renderHeader(const Manifest& manifest);
std::string renderJson(const Manifest& manifest);

// Compares rendered header to file content; prints diff-style summary to
// `os` (injectable for tests). Returns true if identical.
bool checkAgainst(const std::string& rendered,
                  const std::string& fileContent,
                  std::string& report);

// Count top-level comma-separated args inside the "(...)" of a detail string.
int argCountFromDetail(const std::string& detail);

} // namespace refl

#endif // DORIAX_REFLECTOR_H
