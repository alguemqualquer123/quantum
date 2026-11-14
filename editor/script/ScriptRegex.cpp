// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "ScriptRegex.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>

namespace quantum::editor::script {

    namespace {
        constexpr size_t maxPattern = 512;
        constexpr size_t maxProgramInstructions = 4096;
        // One attempt at a start position. Bounds what a pathological pattern can cost
        // instead of letting the editor freeze on it.
        constexpr int stepBudget = 300000;
        // Above this the (pc, position) visited bitmap costs more than the scan is worth.
        constexpr size_t maxVisitedCells = 4u << 20;

        bool isWordByte(unsigned char c) {
            return std::isalnum(c) != 0 || c == '_';
        }

        unsigned char foldByte(unsigned char c, bool caseSensitive) {
            return caseSensitive ? c : (unsigned char)std::tolower(c);
        }

        // Ranges for \d, \w and \s. ASCII only, which is what the byte engine promises.
        bool setRanges(char which, std::vector<std::pair<unsigned char, unsigned char>>& out) {
            out.clear();
            switch (which) {
                case 'd':
                    out.push_back({ '0', '9' });
                    return true;
                case 'w':
                    out.push_back({ '0', '9' });
                    out.push_back({ 'A', 'Z' });
                    out.push_back({ '_', '_' });
                    out.push_back({ 'a', 'z' });
                    return true;
                case 's':
                    out.push_back({ '\t', '\r' });
                    out.push_back({ ' ', ' ' });
                    return true;
                default:
                    return false;
            }
        }

        void appendSet(std::vector<std::pair<unsigned char, unsigned char>>& into,
                       const std::vector<std::pair<unsigned char, unsigned char>>& ranges,
                       bool negate) {
            if (!negate) {
                for (const auto& r : ranges) into.push_back(r);
                return;
            }
            std::vector<std::pair<unsigned char, unsigned char>> sorted = ranges;
            std::sort(sorted.begin(), sorted.end());
            int cursor = 0;
            for (const auto& r : sorted) {
                if ((int)r.first > cursor) into.push_back({ (unsigned char)cursor, (unsigned char)(r.first - 1) });
                if ((int)r.second + 1 > cursor) cursor = (int)r.second + 1;
            }
            if (cursor <= 255) into.push_back({ (unsigned char)cursor, 255 });
        }

        char singleEscape(char e, bool& known) {
            known = true;
            switch (e) {
                case 'n': return '\n';
                case 't': return '\t';
                case 'r': return '\r';
                case 'f': return '\f';
                case 'v': return '\v';
                case 'a': return '\a';
                case '0': return '\0';
                default: break;
            }
            static const char* specials = "()[]{}.*+?^$\\/|-:&~`'\"#_%@!;,<>=";
            for (const char* p = specials; *p; ++p) {
                if (*p == e) return e;
            }
            known = false;
            return 0;
        }
    }

    // Parser: a syntax tree first, the instruction program second. Building the tree
    // before emitting keeps each construct's back-patching local, and lets a bounded
    // repeat emit its body more than once without relocating any target.
    struct ScriptRegex::Parser {
        enum class Kind : unsigned char { Char, Any, Class, Begin, End, Boundary, Concat, Alt, Repeat, Group };

        struct Node {
            Kind kind = Kind::Concat;
            unsigned char ch = 0;
            bool negated = false;      // Class content, and the lazy flag on Repeat
            std::vector<std::pair<unsigned char, unsigned char>> ranges;
            std::vector<Node> children; // Concat, Alt
            std::vector<Node> body;     // Repeat, Group
            int group = 0;
            int minRepeat = 0;
            int maxRepeat = 0;          // -1 for unbounded
        };

        const std::string& src;
        std::vector<Instr>& prog;
        size_t pos = 0;
        int groups = 0;
        int depth = 0;
        std::string error;

        Parser(const std::string& pattern, std::vector<Instr>& out) : src(pattern), prog(out) {}

        bool fail(const std::string& message) {
            if (error.empty()) error = message;
            return false;
        }

        bool atEnd() const { return pos >= src.size(); }
        char peek() const { return atEnd() ? '\0' : src[pos]; }

        // ---- parse ----

        bool parse(Node& root) {
            if (!parseTerm(root)) return false;
            if (!atEnd()) return fail("unexpected ')'");
            return true;
        }

        bool parseTerms(std::vector<Node>& out) {
            while (!atEnd() && peek() != ')' && peek() != '|') {
                Node term;
                if (!parseAtomWithQuantifier(term)) return false;
                out.push_back(std::move(term));
            }
            return true;
        }

        bool parseTerm(Node& out) {
            std::vector<Node> alts;
            Node first;
            if (!parseBranch(first)) return false;
            alts.push_back(std::move(first));
            while (peek() == '|') {
                ++pos;
                Node next;
                if (!parseBranch(next)) return false;
                alts.push_back(std::move(next));
            }
            if (alts.size() == 1) {
                out = std::move(alts.front());
                return true;
            }
            out.kind = Kind::Alt;
            out.children = std::move(alts);
            return true;
        }

        bool parseBranch(Node& out) {
            out.kind = Kind::Concat;
            return parseTerms(out.children);
        }

        bool parseAtomWithQuantifier(Node& out) {
            Node atom;
            if (!parseAtom(atom)) return false;

            while (!atEnd() && (peek() == '*' || peek() == '+' || peek() == '?' || peek() == '{')) {
                char q = peek();
                int minCount = 0;
                int maxCount = 0;
                size_t quantifierStart = pos;
                if (q == '*') { ++pos; maxCount = -1; }
                else if (q == '+') { ++pos; minCount = 1; maxCount = -1; }
                else if (q == '?') { ++pos; maxCount = 1; }
                else if (!parseBrace(minCount, maxCount)) {
                    // A brace that is not a bound is a literal, which is what every regex
                    // flavor does with a pattern like "a{".
                    pos = quantifierStart;
                    break;
                }
                bool lazy = false;
                if (peek() == '?') { ++pos; lazy = true; }
                if (minCount == 0 && maxCount == 0) continue;

                Node repeat;
                repeat.kind = Kind::Repeat;
                repeat.minRepeat = minCount;
                repeat.maxRepeat = maxCount;
                repeat.negated = lazy;
                repeat.body.push_back(std::move(atom));
                atom = std::move(repeat);
            }
            out = std::move(atom);
            return true;
        }

        bool parseBrace(int& minCount, int& maxCount) {
            size_t save = pos;
            ++pos;
            int lo = 0;
            bool any = false;
            while (!atEnd() && std::isdigit((unsigned char)peek())) { lo = lo * 10 + (peek() - '0'); ++pos; any = true; }
            if (!any) { pos = save; return false; }
            int hi = lo;
            if (peek() == ',') {
                ++pos;
                hi = -1;
                bool hiAny = false;
                int value = 0;
                while (!atEnd() && std::isdigit((unsigned char)peek())) { value = value * 10 + (peek() - '0'); ++pos; hiAny = true; }
                if (hiAny) hi = value;
            }
            if (peek() != '}') { pos = save; return false; }
            ++pos;
            if (hi >= 0 && hi < lo) { pos = save; return false; }
            minCount = lo;
            maxCount = hi;
            return true;
        }

        bool parseAtom(Node& out) {
            if (atEnd()) return fail("pattern ended early");
            char c = peek();
            if (c == '(') {
                ++pos;
                bool capturing = true;
                if (peek() == '?') {
                    ++pos;
                    if (peek() != ':') return fail("only (?: groups are supported");
                    ++pos;
                    capturing = false;
                }
                if (++depth > 32) { --depth; return fail("too many nested groups"); }
                Node inner;
                if (!parseTerm(inner)) { --depth; return false; }
                if (peek() != ')') { --depth; return fail("missing ')'"); }
                ++pos;
                --depth;
                if (!capturing) {
                    out = std::move(inner);
                    return true;
                }
                out.kind = Kind::Group;
                // Capture numbers are 1-based here: index 0 of RegexMatch::groups is the
                // whole match, and the spans come from slots 2*group and 2*group+1.
                out.group = ++groups;
                out.body.push_back(std::move(inner));
                return true;
            }
            if (c == '[') {
                ++pos;
                Node node;
                node.kind = Kind::Class;
                if (peek() == '^') { node.negated = true; ++pos; }
                bool anyItem = false;
                while (!atEnd() && peek() != ']') {
                    unsigned char lo = 0;
                    std::vector<std::pair<unsigned char, unsigned char>> extra;
                    bool isSet = false;
                    if (!parseClassChar(lo, extra, isSet)) return false;
                    if (isSet) {
                        for (const auto& r : extra) node.ranges.push_back(r);
                        anyItem = true;
                        continue;
                    }
                    unsigned char hi = lo;
                    if (peek() == '-' && pos + 1 < src.size() && src[pos + 1] != ']') {
                        ++pos;
                        std::vector<std::pair<unsigned char, unsigned char>> ignored;
                        bool ignoredSet = false;
                        if (!parseClassChar(hi, ignored, ignoredSet)) return false;
                        if (ignoredSet) {
                            for (const auto& r : ignored) node.ranges.push_back(r);
                            anyItem = true;
                            continue;
                        }
                    }
                    if (hi < lo) return fail("inverted range in []");
                    node.ranges.push_back({ lo, hi });
                    anyItem = true;
                }
                if (peek() != ']' || !anyItem) return fail("unterminated []");
                ++pos;
                out = std::move(node);
                return true;
            }
            if (c == '.') { ++pos; out.kind = Kind::Any; return true; }
            if (c == '^') { ++pos; out.kind = Kind::Begin; return true; }
            if (c == '$') { ++pos; out.kind = Kind::End; return true; }
            if (c == '*') return fail("nothing to repeat");
            if (c == '+') return fail("nothing to repeat");
            if (c == '?') return fail("nothing to repeat");
            if (c == '\\') {
                ++pos;
                if (atEnd()) return fail("pattern ends in a backslash");
                char e = peek();
                ++pos;
                if (e == 'b') { out.kind = Kind::Boundary; return true; }
                std::vector<std::pair<unsigned char, unsigned char>> base;
                if (setRanges((char)std::tolower(e), base)) {
                    // The negated forms expand to the complement ranges, so the class
                    // itself stays positive and [^...] keeps meaning one negation.
                    out.kind = Kind::Class;
                    out.negated = false;
                    out.ranges.clear();
                    if (e >= 'A' && e <= 'Z') appendSet(out.ranges, base, true);
                    else out.ranges = base;
                    return true;
                }
                bool known = false;
                char value = singleEscape(e, known);
                if (!known) {
                    if (std::isalpha((unsigned char)e)) { out.kind = Kind::Char; out.ch = (unsigned char)e; return true; }
                    return fail(std::string("unsupported escape \\") + e);
                }
                out.kind = Kind::Char;
                out.ch = (unsigned char)value;
                return true;
            }
            ++pos;
            out.kind = Kind::Char;
            out.ch = (unsigned char)c;
            return true;
        }

        // One item of a class: a byte, a range endpoint, or a whole set for \d \w \s and
        // their negations. `set` receives the ranges when isSet comes back true.
        bool parseClassChar(unsigned char& value,
                            std::vector<std::pair<unsigned char, unsigned char>>& set,
                            bool& isSet) {
            isSet = false;
            if (atEnd() || peek() == ']') return fail("unterminated []");
            char c = peek();
            if (c != '\\') { ++pos; value = (unsigned char)c; return true; }
            ++pos;
            if (atEnd()) return fail("pattern ends in a backslash");
            char e = peek();
            ++pos;
            std::vector<std::pair<unsigned char, unsigned char>> base;
            if (setRanges((char)std::tolower(e), base)) {
                isSet = true;
                set = base;
                if (e >= 'A' && e <= 'Z') {
                    set.clear();
                    appendSet(set, base, true);
                }
                return true;
            }
            bool known = false;
            char single = singleEscape(e, known);
            if (!known) {
                if (std::isalpha((unsigned char)e)) { value = (unsigned char)e; return true; }
                return fail(std::string("unsupported escape \\") + e);
            }
            value = (unsigned char)single;
            return true;
        }

        // ---- emit ----

        bool emit(const Node& node) {
            if (prog.size() >= maxProgramInstructions) return fail("pattern too complex");
            switch (node.kind) {
                case Kind::Char: {
                    Instr in; in.op = Op::Char; in.ch = node.ch; prog.push_back(in);
                    return true;
                }
                case Kind::Any: { Instr in; in.op = Op::Any; prog.push_back(in); return true; }
                case Kind::Begin: { Instr in; in.op = Op::Begin; prog.push_back(in); return true; }
                case Kind::End: { Instr in; in.op = Op::End; prog.push_back(in); return true; }
                case Kind::Boundary: { Instr in; in.op = Op::WordBoundary; prog.push_back(in); return true; }
                case Kind::Class: {
                    Instr in; in.op = Op::Class; in.negated = node.negated;
                    for (const auto& r : node.ranges) in.ranges.push_back(ClassRange{ r.first, r.second });
                    prog.push_back(in);
                    return true;
                }
                case Kind::Group: {
                    Instr open; open.op = Op::GroupOpen; open.arg = 2 * node.group; prog.push_back(open);
                    if (!emit(node.body.front())) return false;
                    Instr close; close.op = Op::GroupClose; close.arg = 2 * node.group + 1; prog.push_back(close);
                    return true;
                }
                case Kind::Concat: {
                    for (const Node& child : node.children) if (!emit(child)) return false;
                    return true;
                }
                case Kind::Alt:
                    return emitAlternation(node.children, 0);
                case Kind::Repeat:
                    return emitRepeat(node);
            }
            return true;
        }

        // One Split per alternative, chained: the preferred branch is the current
        // alternative at fall-through, the other is the rest of the list.
        bool emitAlternation(const std::vector<Node>& alts, size_t index) {
            if (index + 1 >= alts.size()) return emit(alts[index]);
            size_t split = prog.size();
            Instr in; in.op = Op::Split; in.negated = false;
            prog.push_back(in);
            if (!emit(alts[index])) return false;
            size_t jump = prog.size();
            Instr skip; skip.op = Op::Jump; prog.push_back(skip);
            prog[split].arg = (int)prog.size();
            if (!emitAlternation(alts, index + 1)) return false;
            prog[jump].arg = (int)prog.size();
            return true;
        }

        bool emitRepeat(const Node& node) {
            const Node& body = node.body.front();
            for (int i = 0; i < node.minRepeat; ++i) if (!emit(body)) return false;
            if (node.maxRepeat < 0) return emitStar(body, node.negated);
            for (int i = 0; i < node.maxRepeat - node.minRepeat; ++i) {
                size_t split = prog.size();
                Instr in; in.op = Op::Split; in.negated = node.negated;
                prog.push_back(in);
                if (!emit(body)) return false;
                prog[split].arg = (int)prog.size();
            }
            return true;
        }

        // body* with the exit patched after the loop-back Jump. Greedy prefers the body
        // at fall-through, lazy prefers the exit held in arg.
        bool emitStar(const Node& body, bool lazy) {
            size_t split = prog.size();
            Instr in; in.op = Op::Split; in.negated = lazy;
            prog.push_back(in);
            if (!emit(body)) return false;
            Instr back; back.op = Op::Jump; back.arg = (int)split; prog.push_back(back);
            prog[split].arg = (int)prog.size();
            return true;
        }
    };

    bool ScriptRegex::compile(const std::string& pattern, bool caseSensitiveValue, std::string& error) {
        error.clear();
        compiled = false;
        program.clear();
        groupCountValue = 0;
        source = pattern;
        caseSensitive = caseSensitiveValue;

        if (pattern.size() > maxPattern) {
            error = "the pattern is longer than " + std::to_string(maxPattern) + " characters";
            return false;
        }

        Parser::Node root;
        Parser parser(pattern, program);
        if (!parser.parse(root) || !parser.emit(root)) {
            error = parser.error;
            program.clear();
            return false;
        }
        Instr done;
        done.op = Op::Match;
        program.push_back(done);
        groupCountValue = parser.groups;
        compiled = true;
        return true;
    }

    bool ScriptRegex::classMatches(const Instr& instr, unsigned char c, bool caseSensitiveValue) {
        unsigned char folded = foldByte(c, caseSensitiveValue);
        bool inside = false;
        for (const ClassRange& range : instr.ranges) {
            if (folded >= foldByte(range.lo, caseSensitiveValue) && folded <= foldByte(range.hi, caseSensitiveValue)) {
                inside = true;
                break;
            }
        }
        return inside != instr.negated;
    }

    bool ScriptRegex::run(size_t start, const std::string& text, RegexMatch& out) const {
        if (program.empty()) return false;

        struct Frame {
            int pc;
            size_t sp;
            size_t logSize;
        };

        const size_t n = text.size();
        const int capSlots = 2 * (groupCountValue + 1);
        std::vector<int> caps((size_t)capSlots, -1);
        std::vector<std::pair<int, int>> capLog;
        std::vector<Frame> stack;
        stack.reserve(128);

        // A (pc, position) pair already tried from this start cannot succeed now either;
        // remembering that is what keeps a nested repeat from looping forever.
        std::vector<uint64_t> visited;
        const size_t pcCount = program.size();
        const bool useVisited = pcCount * (n + 1) <= maxVisitedCells;
        if (useVisited) visited.assign((pcCount * (n + 1) + 63) / 64, 0);

        int steps = stepBudget;
        stack.push_back(Frame{ 0, start, 0 });

        while (!stack.empty()) {
            Frame frame = stack.back();
            stack.pop_back();
            while (capLog.size() > frame.logSize) {
                caps[(size_t)capLog.back().first] = capLog.back().second;
                capLog.pop_back();
            }
            int pc = frame.pc;
            size_t sp = frame.sp;

            while (pc >= 0 && pc < (int)program.size()) {
                if (--steps <= 0) return false;
                if (useVisited) {
                    size_t cell = (size_t)pc * (n + 1) + sp;
                    uint64_t& word = visited[cell >> 6];
                    uint64_t bit = (uint64_t)1 << (cell & 63);
                    if (word & bit) break;
                    word |= bit;
                }
                const Instr& instr = program[(size_t)pc];
                bool advanced = false;
                switch (instr.op) {
                    case Op::Match: {
                        out.found = true;
                        out.start = (int)start;
                        out.end = (int)sp;
                        out.groups.assign((size_t)groupCountValue + 1, std::make_pair(-1, -1));
                        out.groups[0] = { (int)start, (int)sp };
                        for (int g = 1; g <= groupCountValue; ++g) {
                            out.groups[(size_t)g] = { caps[(size_t)(2 * g)], caps[(size_t)(2 * g + 1)] };
                        }
                        return true;
                    }
                    case Op::Char: {
                        if (sp >= n || foldByte((unsigned char)text[sp], caseSensitive) != foldByte(instr.ch, caseSensitive)) break;
                        ++sp;
                        advanced = true;
                        break;
                    }
                    case Op::Any: {
                        if (sp >= n) break;
                        ++sp;
                        advanced = true;
                        break;
                    }
                    case Op::Class: {
                        if (sp >= n || !classMatches(instr, (unsigned char)text[sp], caseSensitive)) break;
                        ++sp;
                        advanced = true;
                        break;
                    }
                    case Op::GroupOpen:
                    case Op::GroupClose: {
                        if (instr.arg >= 0 && instr.arg < capSlots) {
                            capLog.push_back({ instr.arg, caps[(size_t)instr.arg] });
                            caps[(size_t)instr.arg] = (int)sp;
                        }
                        advanced = true;
                        break;
                    }
                    case Op::Jump: {
                        pc = instr.arg;
                        continue;
                    }
                    case Op::Split: {
                        const int preferred = instr.negated ? instr.arg : pc + 1;
                        const int fallback = instr.negated ? pc + 1 : instr.arg;
                        if (fallback >= 0 && fallback < (int)program.size()) {
                            stack.push_back(Frame{ fallback, sp, capLog.size() });
                        }
                        pc = preferred;
                        continue;
                    }
                    case Op::Begin: {
                        // Line starts, so a find over a whole buffer anchors on every line.
                        if (sp != 0 && text[sp - 1] != '\n') break;
                        advanced = true;
                        break;
                    }
                    case Op::End: {
                        if (sp != n && text[sp] != '\n') break;
                        advanced = true;
                        break;
                    }
                    case Op::WordBoundary: {
                        const bool before = sp > 0 && sp <= n && isWordByte((unsigned char)text[sp - 1]);
                        const bool after = sp < n && isWordByte((unsigned char)text[sp]);
                        if (before == after) break;
                        advanced = true;
                        break;
                    }
                }
                if (!advanced) break;
                ++pc;
            }
        }
        out.found = false;
        return false;
    }

    bool ScriptRegex::search(const std::string& text, size_t from, RegexMatch& out) const {
        out = RegexMatch();
        if (!compiled) return false;
        if (from > text.size()) return false;
        for (size_t start = from; start <= text.size(); ++start) {
            RegexMatch candidate;
            if (run(start, text, candidate)) {
                out = candidate;
                return true;
            }
        }
        return false;
    }

    bool ScriptRegex::fullMatch(const std::string& text) const {
        if (!compiled) return false;
        if (text.empty()) {
            RegexMatch empty;
            return search(text, 0, empty);
        }
        // Anchoring through the public path keeps the group numbering of `this` intact.
        ScriptRegex anchored;
        std::string error;
        if (!anchored.compile("^(?:" + source + ")$", caseSensitive, error)) return false;
        RegexMatch out;
        return anchored.search(text, 0, out) && out.start == 0 && out.end == (int)text.size();
    }

    void ScriptRegex::findAll(const std::string& text, std::vector<RegexMatch>& out, size_t limit) const {
        size_t from = 0;
        while (out.size() < limit && from <= text.size()) {
            RegexMatch one;
            if (!search(text, from, one)) break;
            out.push_back(one);
            from = one.end == one.start ? (size_t)one.end + 1 : (size_t)one.end;
        }
    }

}
