// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "ScriptRegex.h"

namespace quantum::editor::script {

    namespace fs = std::filesystem;

    // Find in files / replace in files for the script editor page and its CLI twin. Text
    // only: no project, no scene, so the same call answers the panel and a headless run.
    struct SearchOptions {
        std::string query;
        bool useRegex = false;
        bool caseSensitive = false;
        bool wholeWord = false;

        // Project-relative roots to look inside; empty means the project folder itself.
        std::vector<std::string> folders;
        // Extensions to read, with the dot (".lua"). Empty means the default script set.
        std::vector<std::string> extensions;
        // Path fragments that skip a file, e.g. "/build/" or ".thumbnails".
        std::vector<std::string> exclude;

        int maxMatchesPerFile = 400;
        int maxTotalMatches = 5000;
        size_t maxFileBytes = 2u * 1024u * 1024u;

        // An in-document search can be limited to these lines, inclusive; -1 leaves the
        // range open. This is what the find bar's "in selection" scope sets.
        int firstLine = -1;
        int lastLine = -1;

        // Polled between files so closing the panel stops a big scan.
        std::function<bool()> cancel;
    };

    struct SearchMatch {
        int line = 0;     // 0-based
        int column = 0;   // 0-based byte offset in the line, the editor's own unit
        int endColumn = 0;
        std::string preview;   // the line, trimmed
        bool previewCut = false;
    };

    struct SearchFileResult {
        std::string path;   // project-relative, '/' separated
        std::vector<SearchMatch> matches;
        bool truncated = false;
        std::string error;  // a file that could not be read
    };

    struct SearchReport {
        std::vector<SearchFileResult> files;
        int totalMatches = 0;
        int scannedFiles = 0;
        bool truncated = false;
        bool cancelled = false;
        std::string error;  // an invalid pattern stops the whole search

        int fileCount() const { return static_cast<int>(files.size()); }
    };

    struct ReplaceReport {
        int filesChanged = 0;
        int replacements = 0;
        std::vector<std::string> changedFiles;
        bool dryRun = true;
        bool truncated = false;
        bool cancelled = false;
        std::string error;
    };

    // The pattern a search runs on: the query as written, or escaped when it is literal,
    // with word boundaries when asked. Reports the compile error instead of throwing.
    bool buildSearchRegex(const SearchOptions& options, ScriptRegex& regex, std::string& error);

    // Script files under `root` that the options allow, project-relative, sorted.
    std::vector<std::string> collectSearchFiles(const fs::path& root, const SearchOptions& options);

    // The line loop both entry points share. `lines` holds the text of each line without
    // its ending, which is what the editor widget already has, so a keystroke in the find
    // bar never copies the document. Matches are appended to `out`.
    void searchLines(const std::vector<std::string>& lines, const SearchOptions& options,
        ScriptRegex& regex, SearchFileResult& out);

    // Replaces every match in `lines` and returns how many. `maxReplacements` of 0 means
    // no limit; when it stops the scan early `truncated` says so, because the caller has
    // to report a partial apply instead of a clean run.
    int replaceLines(std::vector<std::string>& lines, const SearchOptions& options,
        ScriptRegex& regex, const std::string& replacement, int maxReplacements, bool& truncated);

    // The text `replacement` produces for one match. With useRegex, `$0`..`$99` splice a
    // capture group and `$$` writes a dollar; either way `\n \t \r \0 \\ \$` are escapes.
    // The find bar's "Replace" needs it on a single match, the project command on a whole
    // file, and both must mean the same thing.
    std::string expandReplacement(const std::string& replacement, const RegexMatch& match,
        const std::string& line, bool useRegex);

    // One file of text. `error` is only for an invalid pattern; a file error goes to the
    // result. Used by the in-document find bar, which already has the text in memory.
    SearchFileResult searchText(const std::string& path, const std::string& text,
        const SearchOptions& options, ScriptRegex& regex);

    SearchReport searchProject(const fs::path& root, const SearchOptions& options);

    // When useRegex, `$0`..`$99` in `replacement` splice a capture group and `$$` writes a
    // dollar; a literal search only takes `\n`, `\t` and `\\` as escapes.
    ReplaceReport replaceProject(const fs::path& root, const SearchOptions& options,
        const std::string& replacement, bool dryRun);

    // The extensions a search reads when the caller named none.
    const std::vector<std::string>& defaultScriptExtensions();

}
