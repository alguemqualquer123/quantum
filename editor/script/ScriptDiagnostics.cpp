// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "ScriptDiagnostics.h"

#include "lua.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace quantum::editor::script {

    namespace {

        std::vector<std::string> splitLines(const std::string& text) {
            std::vector<std::string> lines;
            std::string current;
            for (char c : text) {
                if (c == '\n') {
                    if (!current.empty() && current.back() == '\r') current.pop_back();
                    lines.push_back(current);
                    current.clear();
                } else {
                    current.push_back(c);
                }
            }
            lines.push_back(current);
            return lines;
        }

        std::string trimmed(const std::string& text) {
            size_t begin = text.find_first_not_of(" \t\r\n");
            if (begin == std::string::npos) return std::string();
            size_t end = text.find_last_not_of(" \t\r\n");
            return text.substr(begin, end - begin + 1);
        }

        // "Scripts/Foo.lua:12: '=' expected near 'end'" -> the position and the message.
        // Lua cuts a long chunk name to LUA_IDSIZE with a "..." head, so the name cannot be
        // matched against what we passed; the first colon followed by a digit is the anchor.
        void parseLuaError(const std::string& message, ScriptIssue& issue) {
            size_t at = 0;
            while (at + 1 < message.size() &&
                   !(message[at] == ':' && std::isdigit((unsigned char)message[at + 1]))) ++at;
            if (at + 1 >= message.size()) {
                issue.message = trimmed(message);
                return;
            }
            size_t digits = at + 1;
            while (digits < message.size() && std::isdigit((unsigned char)message[digits])) ++digits;
            issue.line = std::max(0, atoi(message.substr(at + 1, digits - at - 1).c_str()) - 1);
            at = digits;
            if (at + 1 < message.size() && message[at] == ':' &&
                std::isdigit((unsigned char)message[at + 1])) {
                digits = at + 1;
                while (digits < message.size() && std::isdigit((unsigned char)message[digits])) ++digits;
                issue.column = std::max(0, atoi(message.substr(at + 1, digits - at - 1).c_str()) - 1);
                at = digits;
            }
            if (at < message.size() && message[at] == ':') ++at;
            issue.message = trimmed(message.substr(at));
        }

        struct Scanner {
            const std::string& text;
            size_t pos = 0;
            int line = 0;
            int column = 0;

            explicit Scanner(const std::string& t) : text(t) {}

            bool atEnd() const { return pos >= text.size(); }
            char peek(size_t ahead = 0) const {
                return pos + ahead < text.size() ? text[pos + ahead] : '\0';
            }
            void step() {
                char c = text[pos];
                ++pos;
                if (c == '\n') { ++line; column = 0; }
                else { ++column; }
            }
            void stepN(size_t count) { for (size_t i = 0; i < count && !atEnd(); ++i) step(); }
        };

        char closerOf(char opener) {
            switch (opener) {
                case '(': return ')';
                case '[': return ']';
                case '{': return '}';
                default: return '\0';
            }
        }

        bool isCloser(char c) { return c == ')' || c == ']' || c == '}'; }

    }

    std::vector<ScriptIssue> checkLuaSyntax(const std::string& chunkName, const std::string& text) {
        std::vector<ScriptIssue> issues;
        if (trimmed(text).empty()) return issues;

        // A state of its own, used only to compile. Running the chunk would execute
        // project code from the editor, and the engine state belongs to the main thread.
        lua_State* L = luaL_newstate();
        if (!L) return issues;

        // '@' marks the name as a file, so Lua reports "name:line: message". Long names are
        // cut to LUA_IDSIZE with a "..." head, so the prefix strip has to accept a suffix too.
        const std::string prefixed = "@" + chunkName;
        int status = luaL_loadbuffer(L, text.c_str(), text.size(), prefixed.c_str());
        if (status != LUA_OK) {
            const char* raw = lua_tostring(L, -1);
            ScriptIssue issue;
            issue.file = chunkName;
            issue.severity = IssueSeverity::Error;
            issue.origin = "lua";
            if (raw) parseLuaError(raw, issue);
            if (issue.message.empty()) issue.message = "the file does not compile as Lua";
            issues.push_back(issue);
        }
        lua_close(L);
        return issues;
    }

    std::vector<ScriptIssue> checkBracketBalance(const std::string& text, bool allowPreprocessor) {
        std::vector<ScriptIssue> issues;
        struct Open {
            char ch;
            int line;
            int column;
        };
        std::vector<Open> stack;
        Scanner scan(text);
        const int maxReported = 16;

        while (!scan.atEnd()) {
            char c = scan.peek();
            if (c == '/' && scan.peek(1) == '/') {
                while (!scan.atEnd() && scan.peek() != '\n') scan.step();
                continue;
            }
            if (c == '/' && scan.peek(1) == '*') {
                int startLine = scan.line;
                int startColumn = scan.column;
                scan.stepN(2);
                bool closed = false;
                while (!scan.atEnd()) {
                    if (scan.peek() == '*' && scan.peek(1) == '/') { scan.stepN(2); closed = true; break; }
                    scan.step();
                }
                if (!closed && (int)issues.size() < maxReported) {
                    ScriptIssue issue;
                    issue.line = startLine;
                    issue.column = startColumn;
                    issue.severity = IssueSeverity::Error;
                    issue.origin = "balance";
                    issue.message = "this comment never closes";
                    issues.push_back(issue);
                }
                continue;
            }
            if (c == '"' || c == '\'') {
                int startLine = scan.line;
                int startColumn = scan.column;
                char quote = c;
                scan.step();
                bool closed = false;
                while (!scan.atEnd() && scan.peek() != '\n') {
                    if (scan.peek() == '\\') { scan.stepN(2); continue; }
                    if (scan.peek() == quote) { scan.step(); closed = true; break; }
                    scan.step();
                }
                if (!closed && (int)issues.size() < maxReported) {
                    ScriptIssue issue;
                    issue.line = startLine;
                    issue.column = startColumn;
                    issue.severity = IssueSeverity::Error;
                    issue.origin = "balance";
                    issue.message = quote == '\'' ? "this character literal never closes"
                                                  : "this string never closes";
                    issues.push_back(issue);
                }
                continue;
            }
            if (allowPreprocessor && c == '#' && scan.column == 0) {
                // A directive line can end in a backslash continuation; skip the line and
                // let the balance check see only real code.
                while (!scan.atEnd() && scan.peek() != '\n') {
                    if (scan.peek() == '\\' && scan.peek(1) == '\n') scan.stepN(2);
                    else scan.step();
                }
                continue;
            }
            if (c == '(' || c == '[' || c == '{') {
                stack.push_back(Open{ c, scan.line, scan.column });
                scan.step();
                continue;
            }
            if (isCloser(c)) {
                if (stack.empty()) {
                    if ((int)issues.size() < maxReported) {
                        ScriptIssue issue;
                        issue.line = scan.line;
                        issue.column = scan.column;
                        issue.severity = IssueSeverity::Error;
                        issue.origin = "balance";
                        issue.message = std::string("a closing '") + c + "' has no opener";
                        issues.push_back(issue);
                    }
                    scan.step();
                    continue;
                }
                Open open = stack.back();
                if (closerOf(open.ch) == c) {
                    stack.pop_back();
                } else if ((int)issues.size() < maxReported) {
                    ScriptIssue issue;
                    issue.line = scan.line;
                    issue.column = scan.column;
                    issue.severity = IssueSeverity::Error;
                    issue.origin = "balance";
                    issue.message = std::string("'") + c + "' closes the '" + open.ch +
                                    "' opened on line " + std::to_string(open.line + 1);
                    issues.push_back(issue);
                    stack.pop_back();
                }
                scan.step();
                continue;
            }
            scan.step();
        }

        for (const Open& open : stack) {
            if ((int)issues.size() >= maxReported) break;
            ScriptIssue issue;
            issue.line = open.line;
            issue.column = open.column;
            issue.severity = IssueSeverity::Error;
            issue.origin = "balance";
            issue.message = std::string("this '") + open.ch + "' is never closed";
            issues.push_back(issue);
        }
        return issues;
    }

    std::vector<ScriptIssue> checkLuaLint(const std::string& text) {
        std::vector<ScriptIssue> issues;
        const std::vector<std::string> lines = splitLines(text);

        // "local name" used nowhere else in the file, counted on the whole text so a
        // reference inside a nested function still counts.
        for (size_t i = 0; i < lines.size(); ++i) {
            const std::string& line = lines[i];
            size_t searchFrom = 0;
            for (;;) {
                size_t at = line.find("local ", searchFrom);
                if (at == std::string::npos) break;
                if (at != 0 && (std::isalnum((unsigned char)line[at - 1]) || line[at - 1] == '_' )) {
                    searchFrom = at + 6;
                    continue;
                }
                size_t nameStart = at + 6;
                while (nameStart < line.size() && std::isspace((unsigned char)line[nameStart])) ++nameStart;
                size_t nameEnd = nameStart;
                while (nameEnd < line.size() &&
                       (std::isalnum((unsigned char)line[nameEnd]) || line[nameEnd] == '_')) ++nameEnd;
                std::string name = line.substr(nameStart, nameEnd - nameStart);
                searchFrom = nameEnd;
                if (name.empty() || name == "function" || name == "table") continue;

                size_t uses = 0;
                size_t cursor = 0;
                for (;;) {
                    cursor = text.find(name, cursor);
                    if (cursor == std::string::npos) break;
                    bool leftOk = cursor == 0 ||
                        !(std::isalnum((unsigned char)text[cursor - 1]) || text[cursor - 1] == '_');
                    size_t after = cursor + name.size();
                    bool rightOk = after >= text.size() ||
                        !(std::isalnum((unsigned char)text[after]) || text[after] == '_');
                    if (leftOk && rightOk) ++uses;
                    cursor = after;
                }
                if (uses <= 1 && issues.size() < 64) {
                    ScriptIssue issue;
                    issue.line = (int)i;
                    issue.column = (int)nameStart;
                    issue.endColumn = (int)nameEnd;
                    issue.severity = IssueSeverity::Hint;
                    issue.origin = "lint";
                    issue.message = "local '" + name + "' is not used anywhere else";
                    issues.push_back(issue);
                }
            }
        }

        for (size_t i = 0; i < lines.size(); ++i) {
            const std::string& line = lines[i];
            if (line.find("--") != std::string::npos) continue;
            if (line.find("print(") != std::string::npos && issues.size() < 64) {
                ScriptIssue issue;
                issue.line = (int)i;
                issue.column = (int)line.find("print(");
                issue.severity = IssueSeverity::Info;
                issue.origin = "lint";
                issue.message = "print() is a debug write; the editor console also gets Out::info";
                issues.push_back(issue);
            }
        }
        return issues;
    }

    std::vector<ScriptIssue> checkScriptText(const std::string& path, const std::string& text) {
        std::vector<ScriptIssue> issues;
        std::string lower = path;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });

        if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".lua") == 0) {
            issues = checkLuaSyntax(path, text);
            // A file that does not compile says nothing useful about its locals.
            if (issues.empty()) {
                std::vector<ScriptIssue> lint = checkLuaLint(text);
                issues.insert(issues.end(), lint.begin(), lint.end());
            }
            return issues;
        }
        const bool cpp = lower.size() > 4 &&
            (lower.compare(lower.size() - 4, 4, ".cpp") == 0 || lower.compare(lower.size() - 4, 4, ".hpp") == 0);
        const bool known = cpp ||
            (lower.size() > 2 && (lower.compare(lower.size() - 2, 2, ".h") == 0 ||
                                  lower.compare(lower.size() - 3, 3, ".cc") == 0 ||
                                  lower.compare(lower.size() - 4, 4, ".cxx") == 0 ||
                                  lower.compare(lower.size() - 4, 4, ".hh") == 0 ||
                                  lower.compare(lower.size() - 5, 5, ".glsl") == 0 ||
                                  lower.compare(lower.size() - 5, 5, ".vert") == 0 ||
                                  lower.compare(lower.size() - 5, 5, ".frag") == 0));
        if (known) {
            // A Lua syntax error already explains a broken brace; only add the balance
            // scan when the file looks otherwise fine, to keep the panel short.
            std::vector<ScriptIssue> balance = checkBracketBalance(text, cpp);
            issues.insert(issues.end(), balance.begin(), balance.end());
        }
        return issues;
    }

    std::vector<ScriptIssue> checkScriptFile(const std::string& path, const fs::path& absolute) {
        std::ifstream in(absolute, std::ios::binary);
        if (!in) return std::vector<ScriptIssue>();
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return checkScriptText(path, buffer.str());
    }

    const char* severityName(IssueSeverity severity) {
        switch (severity) {
            case IssueSeverity::Error: return "error";
            case IssueSeverity::Warning: return "warning";
            case IssueSeverity::Info: return "info";
            case IssueSeverity::Hint: return "hint";
        }
        return "info";
    }

}
