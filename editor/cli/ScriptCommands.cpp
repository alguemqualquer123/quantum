// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "ScriptCommands.h"

#include "script/ScriptDiagnostics.h"
#include "script/ScriptOutline.h"
#include "script/ScriptSearch.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace quantum::editor::cli {

    namespace {

        using namespace quantum::editor::script;

        struct ScriptsOptions {
            std::string verb;
            std::string project;
            std::string query;
            std::string replacement;
            std::vector<std::string> folders;
            std::vector<std::string> extensions;
            std::vector<std::string> exclude;
            bool useRegex = false;
            bool caseSensitive = false;
            bool wholeWord = false;
            bool json = false;
            bool write = false;      // replace only; without it the run is a dry run
            bool severities = false; // check: also report hints and info
            int maxMatches = 5000;
            bool help = false;
        };

        std::string commandBaseName(const char* executableName) {
            std::string name = executableName ? executableName : "quantum-editor-cmd";
            const size_t slash = name.find_last_of("/\\");
            if (slash != std::string::npos) name = name.substr(slash + 1);
            const size_t dot = name.find_last_of('.');
            if (dot != std::string::npos && name.compare(dot, name.size() - dot, ".exe") == 0) {
                name = name.substr(0, dot);
            }
            return name;
        }

        std::string trimmed(const std::string& value) {
            size_t begin = value.find_first_not_of(" \t\r\n");
            if (begin == std::string::npos) return std::string();
            const size_t end = value.find_last_not_of(" \t\r\n");
            return value.substr(begin, end - begin + 1);
        }

        bool requireValue(int argc, char** argv, int& index, std::string& value, std::string& error) {
            if (index + 1 >= argc) {
                error = std::string(argv[index]) + " needs a value";
                return false;
            }
            value = argv[++index];
            return true;
        }

        void splitList(const std::string& text, std::vector<std::string>& into) {
            std::istringstream stream(text);
            std::string item;
            while (std::getline(stream, item, ',')) {
                item = trimmed(item);
                if (!item.empty()) into.push_back(item);
            }
        }

        std::string normaliseExtension(std::string value) {
            std::transform(value.begin(), value.end(), value.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (value.empty()) return value;
            if (value[0] != '.') value.insert(value.begin(), '.');
            return value;
        }

        void printUsage(const std::string& commandName) {
            std::cout <<
                "Usage: " << commandName << " scripts <verb> [options]\n"
                "\n"
                "  check   --project <dir>   compile every script and report what breaks\n"
                "            [--folder <rel>] [--ext .lua,.cpp] [--exclude <frag>] [--all] [--json]\n"
                "  outline --project <dir>   the symbols of every script, in file order\n"
                "            [--folder <rel>] [--ext <list>] [--json]\n"
                "  search  <query> --project <dir>   find in files\n"
                "            [--folder <rel>] [--ext <list>] [--exclude <frag>] [--regex] [--case]\n"
                "            [--word] [--max <n>] [--json]\n"
                "  replace <query> --with <text> --project <dir>   the same search, and write\n"
                "            [--write] to change the files; without it nothing is touched\n"
                "\n"
                "Search is per line, so ^ and $ anchor a line. In a --regex replacement $0 is\n"
                "the whole match, $1..$9 a group and $$ a dollar; \\n and \\t work in both modes.\n"
                "Folders default to everything the project holds, and build output, hidden\n"
                "folders and Debug/Release are always skipped.\n"
                "\n"
                "Exit codes: 0 clean, 1 findings (check) or no matches (search), 2 bad usage,\n"
                "3 a file could not be read or written.\n";
        }

        std::string jsonEscape(const std::string& text) {
            std::string out;
            out.reserve(text.size() + 8);
            for (unsigned char c : text) {
                switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20) {
                        char buffer[8];
                        std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                        out += buffer;
                    } else {
                        out.push_back((char)c);
                    }
                }
            }
            return out;
        }

        bool parseScriptsArgs(int argc, char** argv, ScriptsOptions& options, std::string& error) {
            // argv[0] is the `scripts` token, like every other subcommand of this executable.
            options.verb = argc >= 2 && argv[1] ? argv[1] : "";
            if (options.verb.empty() || options.verb == "--help" || options.verb == "-h" ||
                options.verb == "help") {
                options.help = true;
                return true;
            }
            if (options.verb != "check" && options.verb != "outline" &&
                options.verb != "search" && options.verb != "replace") {
                error = "expected check, outline, search or replace, got `" + options.verb + "`";
                return false;
            }

            for (int i = 2; i < argc; ++i) {
                const std::string arg = argv[i];
                if (arg == "--help" || arg == "-h") {
                    options.help = true;
                } else if (arg == "--project") {
                    if (!requireValue(argc, argv, i, options.project, error)) return false;
                } else if (arg == "--folder") {
                    std::string folder;
                    if (!requireValue(argc, argv, i, folder, error)) return false;
                    options.folders.push_back(folder);
                } else if (arg == "--ext") {
                    std::string list;
                    if (!requireValue(argc, argv, i, list, error)) return false;
                    std::vector<std::string> parsed;
                    splitList(list, parsed);
                    for (std::string& value : parsed) options.extensions.push_back(normaliseExtension(value));
                } else if (arg == "--exclude") {
                    std::string fragment;
                    if (!requireValue(argc, argv, i, fragment, error)) return false;
                    options.exclude.push_back(fragment);
                } else if (arg == "--query") {
                    if (!requireValue(argc, argv, i, options.query, error)) return false;
                } else if (arg == "--with") {
                    if (!requireValue(argc, argv, i, options.replacement, error)) return false;
                } else if (arg == "--max") {
                    std::string value;
                    if (!requireValue(argc, argv, i, value, error)) return false;
                    options.maxMatches = std::max(1, atoi(value.c_str()));
                } else if (arg == "--regex") {
                    options.useRegex = true;
                } else if (arg == "--case") {
                    options.caseSensitive = true;
                } else if (arg == "--word") {
                    options.wholeWord = true;
                } else if (arg == "--json") {
                    options.json = true;
                } else if (arg == "--write") {
                    options.write = true;
                } else if (arg == "--all") {
                    options.severities = true;
                } else if (!arg.empty() && arg[0] == '-') {
                    error = "unknown option: " + arg;
                    return false;
                } else if (options.query.empty()) {
                    options.query = arg;
                } else {
                    error = "unexpected argument: " + arg;
                    return false;
                }
            }

            if (options.help) return true;
            if (options.project.empty()) {
                error = options.verb + " needs --project <dir>";
                return false;
            }
            std::error_code ec;
            if (!fs::is_directory(options.project, ec)) {
                error = "not a folder: " + options.project;
                return false;
            }
            if ((options.verb == "search" || options.verb == "replace") && options.query.empty()) {
                error = options.verb + " needs a query";
                return false;
            }
            if (options.verb == "replace" && options.replacement.empty()) {
                error = "replace needs --with <text> (use --with \"\" to delete the matches)";
                return false;
            }
            return true;
        }

        SearchOptions toSearchOptions(const ScriptsOptions& options) {
            SearchOptions search;
            search.query = options.query;
            search.useRegex = options.useRegex;
            search.caseSensitive = options.caseSensitive;
            search.wholeWord = options.wholeWord;
            search.folders = options.folders;
            search.extensions = options.extensions;
            search.exclude = options.exclude;
            search.maxTotalMatches = options.maxMatches;
            return search;
        }

        std::string readFileOrEmpty(const fs::path& file) {
            std::ifstream stream(file, std::ios::binary);
            if (!stream) return std::string();
            std::ostringstream buffer;
            buffer << stream.rdbuf();
            return buffer.str();
        }

        int runCheck(const ScriptsOptions& options, const std::string& commandName) {
            const SearchOptions search = toSearchOptions(options);
            const std::vector<std::string> files = collectSearchFiles(fs::path(options.project), search);
            if (files.empty()) {
                std::cerr << commandName << " scripts check: no script files under "
                          << options.project << "\n";
                return 3;
            }

            int errors = 0, warnings = 0, notes = 0;
            std::vector<std::pair<std::string, std::vector<ScriptIssue>>> rows;
            for (const std::string& rel : files) {
                const std::string text = readFileOrEmpty(fs::path(options.project) / fs::path(rel));
                if (text.empty()) continue;
                auto issues = checkScriptText(rel, text);
                std::vector<ScriptIssue> kept;
                for (const ScriptIssue& issue : issues) {
                    if (!options.severities && issue.severity < IssueSeverity::Warning) continue;
                    kept.push_back(issue);
                    if (issue.severity >= IssueSeverity::Error) ++errors;
                    else if (issue.severity == IssueSeverity::Warning) ++warnings;
                    else ++notes;
                }
                if (!kept.empty()) rows.push_back({ rel, std::move(kept) });
            }

            if (options.json) {
                std::cout << "{\"files\":" << files.size() << ",\"issues\":[";
                for (size_t f = 0; f < rows.size(); ++f) {
                    if (f) std::cout << ",";
                    std::cout << "{\"path\":\"" << jsonEscape(rows[f].first) << "\",\"issues\":[";
                    for (size_t i = 0; i < rows[f].second.size(); ++i) {
                        const ScriptIssue& issue = rows[f].second[i];
                        if (i) std::cout << ",";
                        std::cout << "{\"line\":" << issue.line + 1 << ",\"column\":" << issue.column + 1
                                  << ",\"severity\":\"" << severityName(issue.severity)
                                  << "\",\"origin\":\"" << jsonEscape(issue.origin)
                                  << "\",\"message\":\"" << jsonEscape(issue.message) << "\"}";
                    }
                    std::cout << "]}";
                }
                std::cout << "]}\n";
            } else {
                for (const auto& row : rows) {
                    for (const ScriptIssue& issue : row.second) {
                        std::cout << row.first << ":" << issue.line + 1 << ":" << issue.column + 1
                                  << ": [" << severityName(issue.severity) << "] "
                                  << issue.message << " (" << issue.origin << ")\n";
                    }
                }
                std::cout << "\n" << commandName << " scripts check: " << files.size() << " files, "
                          << errors << " errors, " << warnings << " warnings";
                if (options.severities) std::cout << ", " << notes << " hints";
                std::cout << "\n";
            }
            return errors == 0 ? 0 : 1;
        }

        int runOutline(const ScriptsOptions& options, const std::string& commandName) {
            const SearchOptions search = toSearchOptions(options);
            const std::vector<std::string> files = collectSearchFiles(fs::path(options.project), search);
            if (files.empty()) {
                std::cerr << commandName << " scripts outline: no script files under "
                          << options.project << "\n";
                return 3;
            }

            struct Row {
                std::string path;
                std::vector<OutlineEntry> entries;
            };
            std::vector<Row> rows;
            int total = 0;
            for (const std::string& rel : files) {
                const std::string text = readFileOrEmpty(fs::path(options.project) / fs::path(rel));
                if (text.empty()) continue;
                auto entries = buildOutline(rel, text);
                if (entries.empty()) continue;
                total += (int)entries.size();
                rows.push_back({ rel, std::move(entries) });
            }

            if (options.json) {
                std::cout << "{\"files\":[";
                for (size_t f = 0; f < rows.size(); ++f) {
                    if (f) std::cout << ",";
                    std::cout << "{\"path\":\"" << jsonEscape(rows[f].path) << "\",\"symbols\":[";
                    for (size_t i = 0; i < rows[f].entries.size(); ++i) {
                        const OutlineEntry& entry = rows[f].entries[i];
                        if (i) std::cout << ",";
                        std::cout << "{\"name\":\"" << jsonEscape(entry.name)
                                  << "\",\"kind\":\"" << entry.kindName()
                                  << "\",\"line\":" << entry.line + 1
                                  << ",\"column\":" << entry.column + 1;
                        if (!entry.detail.empty()) std::cout << ",\"detail\":\"" << jsonEscape(entry.detail) << "\"";
                        std::cout << "}";
                    }
                    std::cout << "]}";
                }
                std::cout << "]}\n";
            } else {
                for (const Row& row : rows) {
                    std::cout << row.path << "\n";
                    for (const OutlineEntry& entry : row.entries) {
                        std::cout << "  " << entry.line + 1 << "  " << entry.kindName() << "  " << entry.name;
                        if (!entry.detail.empty()) std::cout << "  (" << entry.detail << ")";
                        std::cout << "\n";
                    }
                }
                std::cout << "\n" << commandName << " scripts outline: " << total << " symbols in "
                          << rows.size() << " files\n";
            }
            return 0;
        }

        int runSearch(const ScriptsOptions& options, const std::string& commandName) {
            const SearchOptions search = toSearchOptions(options);
            const SearchReport report = searchProject(fs::path(options.project), search);
            if (!report.error.empty()) {
                std::cerr << commandName << " scripts search: " << report.error << "\n";
                return 2;
            }

            if (options.json) {
                std::cout << "{\"query\":\"" << jsonEscape(options.query) << "\",\"matches\":"
                          << report.totalMatches << ",\"files\":[";
                for (size_t f = 0; f < report.files.size(); ++f) {
                    const SearchFileResult& file = report.files[f];
                    if (f) std::cout << ",";
                    std::cout << "{\"path\":\"" << jsonEscape(file.path) << "\",\"matches\":[";
                    for (size_t i = 0; i < file.matches.size(); ++i) {
                        const SearchMatch& match = file.matches[i];
                        if (i) std::cout << ",";
                        std::cout << "{\"line\":" << match.line + 1 << ",\"column\":" << match.column + 1
                                  << ",\"endColumn\":" << match.endColumn + 1 << ",\"preview\":\""
                                  << jsonEscape(match.preview) << "\"}";
                    }
                    std::cout << "]}";
                }
                std::cout << "]}\n";
            } else {
                for (const SearchFileResult& file : report.files) {
                    std::cout << file.path << "\n";
                    for (const SearchMatch& match : file.matches) {
                        std::cout << "  " << match.line + 1 << ":" << match.column + 1 << "  "
                                  << match.preview << (match.previewCut ? " ..." : "") << "\n";
                    }
                }
                std::cout << "\n" << report.totalMatches << " matches in " << report.fileCount()
                          << " files (" << report.scannedFiles << " scanned";
                if (report.truncated) std::cout << ", limited to " << options.maxMatches;
                if (report.cancelled) std::cout << ", cancelled";
                std::cout << ")\n";
            }
            return report.totalMatches > 0 ? 0 : 1;
        }

        int runReplace(const ScriptsOptions& options, const std::string& commandName) {
            const SearchOptions search = toSearchOptions(options);
            const ReplaceReport report = replaceProject(fs::path(options.project), search,
                                                        options.replacement, !options.write);
            if (!report.error.empty()) {
                std::cerr << commandName << " scripts replace: " << report.error << "\n";
                return report.dryRun ? 2 : 3;
            }
            if (options.json) {
                std::cout << "{\"dryRun\":" << (report.dryRun ? "true" : "false")
                          << ",\"replacements\":" << report.replacements << ",\"files\":[";
                for (size_t i = 0; i < report.changedFiles.size(); ++i) {
                    if (i) std::cout << ",";
                    std::cout << "\"" << jsonEscape(report.changedFiles[i]) << "\"";
                }
                std::cout << "]}\n";
            } else {
                for (const std::string& file : report.changedFiles) {
                    std::cout << (report.dryRun ? "would change  " : "changed       ") << file << "\n";
                }
                std::cout << "\n" << report.replacements << " replacements in " << report.filesChanged
                          << " files" << (report.dryRun ? " (dry run, nothing written; add --write)" : "")
                          << "\n";
            }
            return report.replacements > 0 || report.dryRun ? 0 : 1;
        }

    }

    int runScriptsCommand(int argc, char** argv, const char* executableName) {
        const std::string commandName = commandBaseName(executableName);
        ScriptsOptions options;
        std::string error;
        if (!parseScriptsArgs(argc, argv, options, error)) {
            std::cerr << commandName << " scripts: " << error << "\n\n";
            printUsage(commandName);
            return 2;
        }
        if (options.help) {
            printUsage(commandName);
            return 0;
        }

        if (options.verb == "check") return runCheck(options, commandName);
        if (options.verb == "outline") return runOutline(options, commandName);
        if (options.verb == "search") return runSearch(options, commandName);
        return runReplace(options, commandName);
    }

}
