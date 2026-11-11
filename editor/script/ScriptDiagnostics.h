// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace quantum::editor::script {

    namespace fs = std::filesystem;

    // What the script editor's Problems panel and gutter marks show. Everything here is
    // plain text work: no project, no scene, no window, so the same function answers the
    // CLI and the page.
    enum class IssueSeverity : unsigned char {
        Hint = 0,
        Info = 1,
        Warning = 2,
        Error = 3
    };

    struct ScriptIssue {
        std::string file;      // project-relative path, empty when the caller supplies it
        int line = 0;          // 0-based
        int column = 0;        // 0-based
        int endLine = 0;
        int endColumn = 0;
        IssueSeverity severity = IssueSeverity::Error;
        std::string message;
        std::string origin;    // the checker that produced it: "lua", "balance", "lint"
    };

    // The real Lua parser: compiles the chunk and never runs it, so a syntax error is
    // exactly what the engine would report. luaL_loadstring would execute the chunk, and
    // running a project script from the editor is not acceptable.
    std::vector<ScriptIssue> checkLuaSyntax(const std::string& chunkName, const std::string& text);

    // Unbalanced {} [] (), a string or comment that never closes. Enough to explain most
    // broken C++/GLSL files without a compiler; the build output stays the authority.
    std::vector<ScriptIssue> checkBracketBalance(const std::string& text, bool allowPreprocessor);

    // Cheap text lint: a local that no other line mentions, a print() left in a shipped
    // script, a tab inside a space-indented file.
    std::vector<ScriptIssue> checkLuaLint(const std::string& text);

    // Dispatches on the extension of `path` (.lua, .h/.hpp/.cpp/.cc/.cxx, .glsl/.vert/.frag).
    std::vector<ScriptIssue> checkScriptText(const std::string& path, const std::string& text);

    // Reads the file and runs the same checks. An unreadable file yields no issues: the
    // caller shows the load error itself.
    std::vector<ScriptIssue> checkScriptFile(const std::string& path, const fs::path& absolute);

    const char* severityName(IssueSeverity severity);

}
