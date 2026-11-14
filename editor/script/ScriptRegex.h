// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace quantum::editor::script {

    // A small regex engine for the editor's own text work (find in files, the script
    // editor find bar, the CLI). It exists instead of std::regex because Jolt propagates
    // _HAS_EXCEPTIONS=0 into the editor (CMakeLists.txt), where an invalid pattern typed
    // by a user would abort the process instead of reporting an error. This one compiles
    // into a program and reports a message, and every scan is bounded by a step budget,
    // so a pathological pattern cannot hang the editor either.
    //
    // Supported: literals, . ^ $ \b, character classes with ranges and negation,
    // \d \w \s and their uppercase opposites, \t \n \r \f \v \0, escaped punctuation,
    // groups (capturing, (?: non-capturing), alternation, and the quantifiers
    // * + ? {n} {n,} {n,m} with a trailing ? for the lazy form. No backreference,
    // lookahead or unicode property, and matching is on bytes with ASCII case folding.

    struct RegexMatch {
        bool found = false;
        int start = 0;
        int end = 0;
        // Group span by capture number, group 0 being the whole match; -1 for a group
        // the match did not go through. Read groupCount() before indexing.
        std::vector<std::pair<int, int>> groups;

        std::string text(const std::string& subject) const {
            if (!found || start < 0 || end < start) return std::string();
            return subject.substr((size_t)start, (size_t)(end - start));
        }
    };

    class ScriptRegex {
    public:
        static constexpr size_t maxPatternLength = 512;
        static constexpr int maxGroups = 16;

        ScriptRegex() = default;

        // Empty error means the pattern compiled. An empty pattern matches the empty
        // string everywhere, which is what an empty find field means.
        bool compile(const std::string& pattern, bool caseSensitive, std::string& error);

        bool isValid() const { return compiled; }
        bool empty() const { return program.empty(); }
        const std::string& pattern() const { return source; }
        bool isCaseSensitive() const { return caseSensitive; }
        int groupCount() const { return groupCountValue; }

        // Leftmost match at or after `from`. Lazy quantifiers are resolved left to right,
        // so the first result is the one a user expects in a find bar.
        bool search(const std::string& text, size_t from, RegexMatch& out) const;
        bool search(const std::string& text, RegexMatch& out) const { return search(text, 0, out); }

        // True when the whole string fits the pattern.
        bool fullMatch(const std::string& text) const;

        // Every match, in order, non-overlapping. Stops at the step budget.
        void findAll(const std::string& text, std::vector<RegexMatch>& out, size_t limit) const;

    private:
        enum class Op : unsigned char {
            Char, Any, Class, GroupOpen, GroupClose, Jump, Split, Match, Begin, End, WordBoundary
        };

        struct ClassRange {
            unsigned char lo;
            unsigned char hi;
        };

        struct Instr {
            Op op = Op::Match;
            unsigned char ch = 0;             // Char
            bool negated = false;             // Class content, and the lazy flag on Split
            std::vector<ClassRange> ranges;   // Class
            int arg = 0;                      // group number, or Jump/Split branch target
        };

        // Recursive-descent parser building the instruction program from an syntax tree.
        struct Parser;

        static bool classMatches(const Instr& instr, unsigned char c, bool caseSensitive);
        bool run(size_t start, const std::string& text, RegexMatch& out) const;

        std::string source;
        std::vector<Instr> program;
        bool caseSensitive = true;
        int groupCountValue = 0;
        bool compiled = false;
    };

}
