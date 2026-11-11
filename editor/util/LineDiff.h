// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <vector>

namespace quantum::editor {

    // Lines that differ between two texts, with exclusive ends. An empty original range is
    // an insertion, an empty modified range a deletion.
    struct LineChange {
        int originalStart;
        int originalEnd;
        int modifiedStart;
        int modifiedEnd;

        bool operator==(const LineChange& other) const {
            return originalStart == other.originalStart && originalEnd == other.originalEnd &&
                   modifiedStart == other.modifiedStart && modifiedEnd == other.modifiedEnd;
        }
        bool operator!=(const LineChange& other) const { return !(*this == other); }
    };

    class LineDiff {
    public:
        static std::vector<LineChange> compute(const std::vector<std::string>& original, const std::vector<std::string>& modified);
    };

}
