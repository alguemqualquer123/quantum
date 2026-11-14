// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "ScriptSearch.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace quantum::editor::script {

    namespace {

        // Folders nobody wants in a project search: build output, generated caches and
        // the third-party trees. Compared against the lowercase relative path.
        const char* const skippedFolders[] = {
            "build", "debug", "release", "minserverel", "relwithdebinfo", "out",
            "cmakefiles", "x64", "x86", "win32", "node_modules", "binaries", "cache"
        };

        constexpr int maxPreviewBytes = 240;
        constexpr size_t binarySniffBytes = 1024;

        std::string lowerOf(const std::string& text) {
            std::string out = text;
            std::transform(out.begin(), out.end(), out.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            return out;
        }

        std::string fileNameOf(const std::string& path) {
            const size_t slash = path.find_last_of('/');
            return slash == std::string::npos ? path : path.substr(slash + 1);
        }

        std::string extensionOf(const std::string& path) {
            const std::string name = fileNameOf(path);
            const size_t dot = name.find_last_of('.');
            if (dot == std::string::npos || dot == 0) return std::string();
            return lowerOf(name.substr(dot));
        }

        // The relative path the panels show and the CLI prints: '/' separated so it reads
        // the same on every machine and matches the editor's own tab keys.
        std::string relativePath(const fs::path& root, const fs::path& file) {
            std::error_code ec;
            fs::path rel = fs::relative(file, root, ec);
            if (ec || rel.empty()) rel = file.filename();
            std::string out = rel.generic_u8string();
            std::replace(out.begin(), out.end(), '\\', '/');
            return out;
        }

        bool hasFragment(const std::string& path, const std::vector<std::string>& fragments) {
            for (const std::string& fragment : fragments) {
                if (fragment.empty()) continue;
                if (path.find(lowerOf(fragment)) != std::string::npos) return true;
            }
            return false;
        }

        bool isSkippedDirectory(const std::string& name) {
            if (name.empty() || name[0] == '.') return true;   // .git, .quantum, .thumbnails
            const std::string lower = lowerOf(name);
            for (const char* skipped : skippedFolders) {
                if (lower == skipped) return true;
            }
            return false;
        }

        bool extensionAllowed(const std::string& relative, const SearchOptions& options) {
            const std::string ext = extensionOf(relative);
            const std::string name = lowerOf(fileNameOf(relative));
            // A project's CMakeLists.txt is a script the editor edits, but its extension
            // is ".txt", which otherwise means "some text file in assets".
            if (name == "cmakelists.txt" || name == "projectbuild.cmake") return true;
            const std::vector<std::string>& set =
                options.extensions.empty() ? defaultScriptExtensions() : options.extensions;
            return std::find(set.begin(), set.end(), ext) != set.end();
        }

        // A document split into content lines and their endings. The endings are kept
        // beside the text so joining writes the same bytes back - a CRLF file never turns
        // LF on a replace - and the matcher only sees content, so `$` means the end of the
        // line rather than the end of the line plus a '\r'.
        struct Document {
            std::vector<std::string> lines;
            std::vector<std::string> endings;
        };

        Document splitDocument(const std::string& text) {
            Document doc;
            size_t start = 0;
            while (true) {
                const size_t next = text.find('\n', start);
                if (next == std::string::npos) {
                    doc.lines.push_back(text.substr(start));
                    doc.endings.push_back(std::string());
                    break;
                }
                std::string content = text.substr(start, next - start);
                std::string ending = "\n";
                if (!content.empty() && content.back() == '\r') {
                    content.pop_back();
                    ending = "\r\n";
                }
                doc.lines.push_back(std::move(content));
                doc.endings.push_back(std::move(ending));
                start = next + 1;
            }
            return doc;
        }

        std::string joinDocument(const Document& doc) {
            std::string out;
            for (size_t i = 0; i < doc.lines.size(); ++i) {
                out += doc.lines[i];
                if (i < doc.endings.size()) out += doc.endings[i];
            }
            return out;
        }

        bool inRange(const SearchOptions& options, int line) {
            if (options.firstLine >= 0 && line < options.firstLine) return false;
            if (options.lastLine >= 0 && line > options.lastLine) return false;
            return true;
        }

        std::string trimRight(const std::string& text) {
            size_t end = text.size();
            while (end > 0 && (text[end - 1] == '\r' || text[end - 1] == '\n' ||
                               text[end - 1] == ' ' || text[end - 1] == '\t')) --end;
            return text.substr(0, end);
        }

        std::string trimBoth(const std::string& text) {
            size_t begin = 0;
            while (begin < text.size() && std::isspace((unsigned char)text[begin])) ++begin;
            return trimRight(text.substr(begin));
        }

        std::string escapeLiteral(const std::string& text) {
            std::string out;
            out.reserve(text.size() + 8);
            for (char c : text) {
                switch (c) {
                case '\\': case '^': case '$': case '.': case '|': case '?':
                case '*': case '+': case '(': case ')': case '[': case ']':
                case '{': case '}':
                    out.push_back('\\');
                    break;
                default: break;
                }
                out.push_back(c);
            }
            return out;
        }

        bool looksBinary(const std::string& text) {
            const size_t n = std::min(binarySniffBytes, text.size());
            for (size_t i = 0; i < n; ++i) {
                if (text[i] == '\0') return true;
            }
            return false;
        }

        bool readFileText(const fs::path& file, std::string& out, std::string& error) {
            std::ifstream stream(file, std::ios::binary);
            if (!stream) {
                error = "cannot open the file";
                return false;
            }
            std::ostringstream buffer;
            buffer << stream.rdbuf();
            out = buffer.str();
            return true;
        }

        // A preview is what the result row shows next to the file name, so it has to read
        // like the code: trimmed, and with the match inside the window when the line is long.
        std::string makePreview(const std::string& line, int column, bool& cut) {
            size_t leading = 0;
            while (leading < line.size() && std::isspace((unsigned char)line[leading])) ++leading;
            const std::string trimmed = trimBoth(line);
            if ((int)trimmed.size() <= maxPreviewBytes) {
                cut = false;
                return trimmed;
            }
            const int at = std::max(0, column - (int)leading);
            const int from = std::max(0, std::min(at - maxPreviewBytes / 3,
                                                  (int)trimmed.size() - maxPreviewBytes));
            cut = true;
            return trimmed.substr((size_t)from, std::min((size_t)maxPreviewBytes,
                                                         trimmed.size() - (size_t)from));
        }

        std::string groupText(const RegexMatch& match, int group, const std::string& line) {
            if (group < 0 || group >= (int)match.groups.size()) return std::string();
            const std::pair<int, int> span = match.groups[(size_t)group];
            if (span.first < 0 || span.second < span.first) return std::string();
            if (span.second > (int)line.size()) return std::string();
            return line.substr((size_t)span.first, (size_t)(span.second - span.first));
        }

        // Collects the allowed files without following a skipped folder at all, which is
        // what keeps a project search on a real tree instead of the build output.
        void walk(const fs::path& root, const fs::path& dir, const SearchOptions& options,
                  std::vector<std::string>& out) {
            std::error_code ec;
            fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
            const fs::directory_iterator end;
            for (; !ec && it != end; it.increment(ec)) {
                const fs::path& entry = it->path();
                std::string rel = relativePath(root, entry);
                if (it->is_directory(ec) && !ec) {
                    if (isSkippedDirectory(fileNameOf(rel))) continue;
                    if (hasFragment(rel, options.exclude)) continue;
                    walk(root, entry, options, out);
                    continue;
                }
                if (!it->is_regular_file(ec) || ec) continue;
                if (!extensionAllowed(rel, options)) continue;
                if (hasFragment(rel, options.exclude)) continue;
                out.push_back(rel);
            }
        }

    }

    const std::vector<std::string>& defaultScriptExtensions() {
        static const std::vector<std::string> extensions = {
            ".lua",
            ".h", ".hpp", ".hh", ".hxx", ".inl",
            ".cpp", ".cc", ".cxx", ".c",
            ".glsl", ".vert", ".frag", ".comp", ".geom", ".tesc", ".tese",
            ".cmake", ".txt"
        };
        return extensions;
    }

    bool buildSearchRegex(const SearchOptions& options, ScriptRegex& regex, std::string& error) {
        error.clear();
        if (options.query.empty()) {
            error = "empty search query";
            return false;
        }
        std::string pattern = options.useRegex ? options.query : escapeLiteral(options.query);
        if (options.wholeWord) pattern = "\\b(?:" + pattern + ")";
        if (!regex.compile(pattern, options.caseSensitive, error)) {
            if (error.empty()) error = "invalid pattern";
            return false;
        }
        return true;
    }

    std::vector<std::string> collectSearchFiles(const fs::path& root, const SearchOptions& options) {
        std::vector<std::string> out;
        std::error_code ec;
        if (!fs::is_directory(root, ec)) return out;
        if (options.folders.empty()) {
            walk(root, root, options, out);
        } else {
            for (const std::string& folder : options.folders) {
                if (folder.empty()) continue;
                fs::path dir = root / fs::path(folder);
                if (fs::is_directory(dir, ec) && !ec) walk(root, dir, options, out);
            }
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    std::string expandReplacement(const std::string& tmpl, const RegexMatch& match,
        const std::string& line, bool useRegex) {
        std::string out;
        out.reserve(tmpl.size());
        for (size_t i = 0; i < tmpl.size(); ++i) {
            const char c = tmpl[i];
            if (c == '\\' && i + 1 < tmpl.size()) {
                const char next = tmpl[++i];
                switch (next) {
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case '0': out.push_back('\0'); break;
                case '\\': out.push_back('\\'); break;
                case '$': out.push_back('$'); break;
                default: out.push_back(c); out.push_back(next); break;
                }
                continue;
            }
            if (c == '$' && useRegex && i + 1 < tmpl.size()) {
                if (tmpl[i + 1] == '$') { out.push_back('$'); ++i; continue; }
                if (std::isdigit((unsigned char)tmpl[i + 1])) {
                    size_t end = i + 1;
                    int number = 0;
                    while (end < tmpl.size() && std::isdigit((unsigned char)tmpl[end]) && end - i < 3) {
                        number = number * 10 + (tmpl[end] - '0');
                        ++end;
                    }
                    out += groupText(match, number, line);
                    i = end - 1;
                    continue;
                }
            }
            out.push_back(c);
        }
        return out;
    }

    void searchLines(const std::vector<std::string>& lines, const SearchOptions& options,
                     ScriptRegex& regex, SearchFileResult& out) {
        if (!regex.isValid() || options.query.empty()) return;

        std::vector<RegexMatch> found;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (!inRange(options, (int)i)) continue;
            if ((int)out.matches.size() >= options.maxMatchesPerFile) {
                out.truncated = true;
                break;
            }
            if (lines[i].empty()) continue;
            found.clear();
            regex.findAll(lines[i], found, (size_t)options.maxMatchesPerFile);
            for (const RegexMatch& match : found) {
                if ((int)out.matches.size() >= options.maxMatchesPerFile) break;
                SearchMatch one;
                one.line = (int)i;
                one.column = match.start;
                one.endColumn = match.end;
                one.preview = makePreview(lines[i], match.start, one.previewCut);
                out.matches.push_back(std::move(one));
            }
        }
    }

    int replaceLines(std::vector<std::string>& lines, const SearchOptions& options,
        ScriptRegex& regex, const std::string& replacement, int maxReplacements, bool& truncated) {
        truncated = false;
        if (!regex.isValid() || options.query.empty()) return 0;

        int count = 0;
        std::vector<RegexMatch> found;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (!inRange(options, (int)i)) continue;
            if (lines[i].empty()) continue;
            found.clear();
            regex.findAll(lines[i], found, (size_t)options.maxMatchesPerFile);
            if (found.empty()) continue;
            std::string rebuilt;
            size_t cursor = 0;
            for (const RegexMatch& match : found) {
                if (maxReplacements > 0 && count >= maxReplacements) {
                    truncated = true;
                    break;
                }
                if (match.start < (int)cursor || match.end < match.start) continue;
                rebuilt += lines[i].substr(cursor, (size_t)(match.start - (int)cursor));
                rebuilt += expandReplacement(replacement, match, lines[i], options.useRegex);
                cursor = (size_t)match.end;
                ++count;
            }
            rebuilt += lines[i].substr(cursor);
            lines[i] = std::move(rebuilt);
            if (truncated) break;
        }
        return count;
    }

    SearchFileResult searchText(const std::string& path, const std::string& text,
        const SearchOptions& options, ScriptRegex& regex) {
        SearchFileResult result;
        result.path = path;
        searchLines(splitDocument(text).lines, options, regex, result);
        return result;
    }

    SearchReport searchProject(const fs::path& root, const SearchOptions& options) {
        SearchReport report;
        ScriptRegex regex;
        std::string error;
        if (!buildSearchRegex(options, regex, error)) {
            report.error = error;
            return report;
        }
        const std::vector<std::string> files = collectSearchFiles(root, options);
        for (const std::string& rel : files) {
            if (options.cancel && options.cancel()) { report.cancelled = true; break; }
            if (report.totalMatches >= options.maxTotalMatches) { report.truncated = true; break; }
            std::string text, readError;
            SearchFileResult file;
            file.path = rel;
            if (!readFileText(root / fs::path(rel), text, readError)) {
                file.error = readError;
                report.files.push_back(std::move(file));
                continue;
            }
            if (text.size() > options.maxFileBytes || looksBinary(text)) continue;
            ++report.scannedFiles;
            file = searchText(rel, text, options, regex);
            if (file.matches.empty()) continue;
            report.totalMatches += (int)file.matches.size();
            if (report.totalMatches >= options.maxTotalMatches) report.truncated = true;
            report.files.push_back(std::move(file));
        }
        return report;
    }

    ReplaceReport replaceProject(const fs::path& root, const SearchOptions& options,
        const std::string& replacement, bool dryRun) {
        ReplaceReport report;
        report.dryRun = dryRun;
        ScriptRegex regex;
        std::string error;
        if (!buildSearchRegex(options, regex, error)) {
            report.error = error;
            return report;
        }
        const std::vector<std::string> files = collectSearchFiles(root, options);
        for (const std::string& rel : files) {
            if (options.cancel && options.cancel()) { report.cancelled = true; break; }
            std::string text, readError;
            if (!readFileText(root / fs::path(rel), text, readError)) continue;
            if (text.size() > options.maxFileBytes || looksBinary(text)) continue;
            Document doc = splitDocument(text);
            bool truncated = false;
            const int budget = options.maxTotalMatches > 0
                ? std::max(0, options.maxTotalMatches - report.replacements) : 0;
            const int changed = replaceLines(doc.lines, options, regex, replacement, budget, truncated);
            if (changed == 0) continue;
            report.replacements += changed;
            report.changedFiles.push_back(rel);
            ++report.filesChanged;
            if (!dryRun) {
                std::ofstream stream(root / fs::path(rel), std::ios::binary | std::ios::trunc);
                if (!stream) {
                    report.error = "cannot write " + rel;
                    break;
                }
                stream << joinDocument(doc);
                stream.close();
            }
            if (report.replacements >= options.maxTotalMatches) { report.truncated = true; break; }
        }
        return report;
    }

}
