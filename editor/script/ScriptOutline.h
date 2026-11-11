// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <vector>

namespace quantum::editor::script {

    // One symbol of a file, for the outline panel, Ctrl+P symbol search and the
    // go-to-definition jump. Text-derived, so it answers while the file is still dirty
    // in the editor and never needs a compile.
    enum class OutlineKind : unsigned char {
        Class = 0,
        Struct,
        Function,
        Method,
        Property,
        Field,
        Table,
        Local,
        Event,
        Module,
        Section
    };

    struct OutlineEntry {
        std::string name;
        std::string detail;   // parameters, the declared type, or the module path
        int line = 0;         // 0-based
        int column = 0;       // 0-based, where the name starts
        OutlineKind kind = OutlineKind::Function;

        std::string kindName() const;
    };

    // Dispatches on the extension of `path`. Entries come back in file order.
    std::vector<OutlineEntry> buildOutline(const std::string& path, const std::string& text);

    // The same list for every script of the project, keyed by path, for the quick-open
    // "all symbols" mode. Lines stay relative to each file.
    struct OutlineFile {
        std::string path;
        std::vector<OutlineEntry> entries;
    };
    std::vector<OutlineFile> buildProjectOutline(const std::vector<std::pair<std::string, std::string>>& files);

    // Case-insensitive substring test used by the outline filter and quick-open.
    bool fuzzyContains(const std::string& haystack, const std::string& needle);

}
