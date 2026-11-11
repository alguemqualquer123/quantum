// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "ScriptOutline.h"
#include "ScriptRegex.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>

namespace quantum::editor::script {

    namespace {

        struct Rule {
            const char* pattern;
            OutlineKind kind;
            int nameGroup;
            int detailGroup;   // -1 for none
            // The section markers ("-- category: x") are comments themselves, so their rule
            // must survive the pass that drops commented-out code.
            bool matchesComments = false;
            // A plain `local` inside a function body is a step of the algorithm, not a
            // symbol of the file; only the column-0 ones belong in the outline.
            bool matchesIndented = true;
        };

        std::string lowerOf(const std::string& text) {
            std::string out = text;
            std::transform(out.begin(), out.end(), out.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            return out;
        }

        bool endsWith(const std::string& path, const char* suffix) {
            const size_t n = strlen(suffix);
            return path.size() >= n && path.compare(path.size() - n, n, suffix) == 0;
        }

        std::vector<std::string> splitLines(const std::string& text) {
            std::vector<std::string> lines;
            std::string current;
            for (char c : text) {
                if (c == '\n') { lines.push_back(current); current.clear(); }
                else if (c != '\r') current.push_back(c);
            }
            lines.push_back(current);
            return lines;
        }

        bool isCommentLine(const std::string& line, const char* marker) {
            size_t i = 0;
            while (i < line.size() && std::isspace((unsigned char)line[i])) ++i;
            return line.compare(i, strlen(marker), marker) == 0;
        }

        // The first character that is not whitespace, which is also the column the entry
        // starts at when the rule's match begins there.
        int indentOf(const std::string& line) {
            int i = 0;
            while (i < (int)line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
            return i;
        }

        std::string trimSpace(const std::string& text) {
            size_t begin = 0;
            while (begin < text.size() && std::isspace((unsigned char)text[begin])) ++begin;
            size_t end = text.size();
            while (end > begin && std::isspace((unsigned char)text[end - 1])) --end;
            return text.substr(begin, end - begin);
        }

        void runRules(const std::string& text, const std::vector<Rule>& rules,
                      const char* commentMarker, std::vector<OutlineEntry>& out) {
            const std::vector<std::string> lines = splitLines(text);
            std::vector<ScriptRegex> compiled;
            std::vector<OutlineKind> kinds;
            std::vector<int> nameGroups, detailGroups;
            std::vector<bool> indentedOk;
            for (const Rule& rule : rules) {
                ScriptRegex re;
                std::string error;
                if (!re.compile(rule.pattern, true, error)) continue;
                compiled.push_back(std::move(re));
                kinds.push_back(rule.kind);
                nameGroups.push_back(rule.nameGroup);
                detailGroups.push_back(rule.detailGroup);
                indentedOk.push_back(rule.matchesIndented);
            }

            bool insideProperties = false;
            ScriptRegex rowName, rowType;
            std::string rowError;
            // One row per line, and the row's Inspector name is the `name` key of its table.
            rowName.compile(R"rx(\bname\s*=\s*"([^"]+)")rx", true, rowError);
            rowType.compile(R"rx(\btype\s*=\s*"([^"]+)")rx", true, rowError);
            for (size_t i = 0; i < lines.size(); ++i) {
                const std::string& line = lines[i];
                if (line.empty()) continue;
                const bool isComment = commentMarker && isCommentLine(line, commentMarker);

                // The component kit declares its Inspector rows inside a `properties`
                // table; those rows are entries too, and no rule can see them.
                const bool wasInsideProperties = insideProperties;
                if (!insideProperties && line.find("properties") != std::string::npos &&
                    line.find('=') != std::string::npos && line.find('{') != std::string::npos) {
                    insideProperties = true;
                } else if (insideProperties && isCommentLine(line, "}")) {
                    insideProperties = false;
                }

                for (size_t r = 0; r < compiled.size(); ++r) {
                    // A commented-out line of code is not a symbol; a marker comment is.
                    if (isComment && !rules[r].matchesComments) continue;
                    RegexMatch m;
                    if (!compiled[r].search(line, m)) continue;
                    if (m.groups.size() <= (size_t)nameGroups[r]) continue;
                    const int nameStart = m.groups[nameGroups[r]].first;
                    const int nameEnd = m.groups[nameGroups[r]].second;
                    if (nameStart < 0 || nameEnd <= nameStart) continue;

                    OutlineEntry entry;
                    entry.name = trimSpace(line.substr(nameStart, nameEnd - nameStart));
                    entry.line = (int)i;
                    // The jump lands on the first character, where the user sees the symbol;
                    // the byte column of the name would be mid-indent.
                    const int indent = indentOf(line);
                    entry.column = indent;
                    entry.kind = kinds[r];
                    if (detailGroups[r] > 0 && detailGroups[r] < (int)m.groups.size()) {
                        const int dStart = m.groups[detailGroups[r]].first;
                        const int dEnd = m.groups[detailGroups[r]].second;
                        if (dStart >= 0 && dEnd > dStart)
                            entry.detail = trimSpace(line.substr(dStart, dEnd - dStart));
                    }
                    if (entry.kind == OutlineKind::Function &&
                        (entry.name.find('.') != std::string::npos || entry.name.find(':') != std::string::npos)) {
                        entry.kind = OutlineKind::Method;
                    }
                    // A local inside a function body is a step of the algorithm, not a symbol
                    // of the file; the outline would list half the lines if it were kept.
                    if (indent > 0 && !indentedOk[r]) continue;
                    out.push_back(std::move(entry));
                    break;  // one entry per line, the first matching rule wins
                }

                if (wasInsideProperties && out.size() < 4000 && rowName.isValid()) {
                    RegexMatch m;
                    if (rowName.search(line, m) && m.groups.size() > 1 && m.groups[1].first >= 0) {
                        OutlineEntry entry;
                        entry.name = trimSpace(line.substr(m.groups[1].first, m.groups[1].second - m.groups[1].first));
                        entry.line = (int)i;
                        entry.column = indentOf(line);
                        entry.kind = OutlineKind::Property;
                        entry.detail = "Inspector row";
                        RegexMatch typeMatch;
                        if (rowType.isValid() && rowType.search(line, typeMatch) &&
                            typeMatch.groups.size() > 1 && typeMatch.groups[1].first >= 0) {
                            entry.detail = trimSpace(line.substr(typeMatch.groups[1].first,
                                            typeMatch.groups[1].second - typeMatch.groups[1].first));
                        }
                        out.push_back(std::move(entry));
                    }
                }
            }
        }

    }

    std::string OutlineEntry::kindName() const {
        switch (kind) {
            case OutlineKind::Class: return "class";
            case OutlineKind::Struct: return "struct";
            case OutlineKind::Function: return "function";
            case OutlineKind::Method: return "method";
            case OutlineKind::Property: return "property";
            case OutlineKind::Field: return "field";
            case OutlineKind::Table: return "table";
            case OutlineKind::Local: return "local";
            case OutlineKind::Event: return "event";
            case OutlineKind::Module: return "module";
            case OutlineKind::Section: return "section";
        }
        return "symbol";
    }

    std::vector<OutlineEntry> buildOutline(const std::string& path, const std::string& text) {
        std::vector<OutlineEntry> out;
        const std::string lower = lowerOf(path);

        if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".lua") == 0) {
            static const std::vector<Rule> rules = {
                { R"(^\s*(?:local\s+)?function\s+([A-Za-z_][\w.:]*)\s*\(([^)]*)\))", OutlineKind::Function, 1, 2 },
                { R"(^\s*(local\s+)?([A-Za-z_][\w.:]*)\s*=\s*function\s*\(([^)]*)\))", OutlineKind::Function, 2, 3 },
                { R"(^\s*([A-Za-z_][\w.]*)\s*=\s*\{)", OutlineKind::Table, 1, -1 },
                { R"(^\s*local\s+([A-Za-z_]\w*)\s*=\s*\{)", OutlineKind::Table, 1, -1 },
                { R"(^\s*local\s+([A-Za-z_]\w*))", OutlineKind::Local, 1, -1, false, false },
                { R"(Register(?:Engine)?Event\s*\(\s*["']([A-Za-z_]\w*)["'])", OutlineKind::Event, 1, -1 },
                { R"(^\s*--\s*(category|section|todo|note):(.{0,60}))", OutlineKind::Section, 2, 1, true },
            };
            runRules(text, rules, "--", out);
            return out;
        }

        const bool cmakeFile = endsWith(lower, ".cmake") || lower == "cmakelists.txt";
        if (cmakeFile) {
            static const std::vector<Rule> rules = {
                { R"(^\s*(function|macro)\s*\(\s*([A-Za-z_][\w-]*))", OutlineKind::Function, 2, 1 },
                { R"(^\s*(add_executable|add_library|add_subdirectory|target_link_libraries|option|set|project|include)\s*\(\s*([A-Za-z_][\w.-]*))", OutlineKind::Section, 2, 1 },
            };
            runRules(text, rules, "#", out);
            return out;
        }

        const bool shader = endsWith(lower, ".glsl") || endsWith(lower, ".vert") || endsWith(lower, ".frag");
        const bool cpp = !shader && (endsWith(lower, ".h") || endsWith(lower, ".hpp") || endsWith(lower, ".hh") ||
                                     endsWith(lower, ".cpp") || endsWith(lower, ".cc") || endsWith(lower, ".cxx"));
        if (!cpp && !shader) return out;

        if (shader) {
            static const std::vector<Rule> rules = {
                { R"(^\s*([A-Za-z_]\w*)\s*\([^)]*\)\s*\{)", OutlineKind::Function, 1, -1 },
                { R"(^\s*(uniform|in|out)\s+[A-Za-z_]\w*\s+([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*;)", OutlineKind::Field, 2, 1 },
                { R"(^\s*#define\s+([A-Za-z_]\w*))", OutlineKind::Local, 1, -1 },
            };
            runRules(text, rules, "//", out);
            return out;
        }

        static const std::vector<Rule> rules = {
            { R"(^\s*(class|struct)\s+([A-Za-z_]\w*))", OutlineKind::Class, 2, 1 },
            { R"(^\s*DPROPERTY\s*\(\s*([A-Za-z_:][\w:<>, ]*?)\s*,\s*([A-Za-z_]\w*))", OutlineKind::Property, 2, 1 },
            { R"(^\s*(?:template\s*<[^>]*>\s*)?[A-Za-z_][\w:<>,&\*\s]*\s+([A-Za-z_]\w*)::([~A-Za-z_]\w*)\s*\(([^;]*?)\)\s*(?:const)?\s*\{)", OutlineKind::Method, 2, 3 },
            { R"(^\s*(?:static\s+|inline\s+|virtual\s+)*[A-Za-z_][\w:<>,&\* ]*\s+([A-Za-z_]\w*)\s*\(([^;]*?)\)\s*(?:const)?\s*\{)", OutlineKind::Function, 1, 2 },
            { R"(^\s*#include\s*[<"]([^>"]+)[>"])", OutlineKind::Module, 1, -1 },
            { R"(^\s*REGISTER_[A-Z_]+\s*\(\s*([A-Za-z_]\w*))", OutlineKind::Event, 1, -1 },
        };
        runRules(text, rules, nullptr, out);

        // `class X` and `struct X` share a rule; the keyword decides the kind.
        for (OutlineEntry& entry : out) {
            if (entry.kind == OutlineKind::Class && entry.detail == "struct") entry.kind = OutlineKind::Struct;
        }
        // Drop the noisy stdafx-style includes beyond a sane outline size.
        if (out.size() > 4000) out.resize(4000);
        return out;
    }

    std::vector<OutlineFile> buildProjectOutline(const std::vector<std::pair<std::string, std::string>>& files) {
        std::vector<OutlineFile> out;
        out.reserve(files.size());
        for (const auto& file : files) {
            OutlineFile entry;
            entry.path = file.first;
            entry.entries = buildOutline(file.first, file.second);
            if (!entry.entries.empty()) out.push_back(std::move(entry));
        }
        return out;
    }

    bool fuzzyContains(const std::string& haystack, const std::string& needle) {
        if (needle.empty()) return true;
        const std::string text = lowerOf(haystack);
        const std::string want = lowerOf(needle);
        // Subsequence match, so "fcam" finds FollowCamera, but a plain substring always
        // wins because the caller sorts by how early the hit starts.
        size_t cursor = 0;
        for (char c : want) {
            size_t at = text.find(c, cursor);
            if (at == std::string::npos) return false;
            cursor = at + 1;
        }
        return true;
    }

}
