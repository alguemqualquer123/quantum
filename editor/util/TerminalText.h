// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace quantum::editor {

    // Outside the 0..255 xterm palette, so it means "whatever the theme's own
    // foreground or background is".
    inline constexpr uint16_t TERM_COLOR_DEFAULT = 0x1FF;

    struct TermSpan {
        std::string text;
        uint16_t fg = TERM_COLOR_DEFAULT;
        uint16_t bg = TERM_COLOR_DEFAULT;
        bool bold = false;
    };

    struct TermLine {
        std::vector<TermSpan> spans;
        // A progress bar rewinds with a bare carriage return instead of a newline,
        // so this line is the previous one redrawn rather than a new one.
        bool rewrite = false;
    };

    // Turns the raw byte stream a shell writes into coloured lines. It covers what
    // command output actually uses - SGR colours, the carriage return a progress bar
    // rewinds with, backspace, tab - and drops cursor and OSC sequences rather than
    // acting on them. It is deliberately not a VT100: there is no screen grid, so a
    // full-screen program (opencode's own interface, vim, htop) cannot be shown here.
    class TerminalText {
    public:
        // Feed one chunk as it arrives from the pipe. Chunks split anywhere,
        // including inside an escape sequence or a UTF-8 codepoint.
        void feed(const char* data, size_t size);

        // Completed lines, in order. The pending line is not returned here: it is
        // what the shell typed last without a newline, usually its prompt, and the
        // caller shows it live through pendingSpans().
        bool pop(TermLine& out);
        bool hasReady() const { return !ready.empty(); }
        const std::vector<TermSpan>& pendingSpans() const { return pending; }

        // Forget a partial line, e.g. when the session restarts.
        void reset();

    private:
        enum class State { Text, Escape, Csi, Osc, OscEscape, Charset };

        static constexpr size_t MAX_PENDING_CHARS = 100000;

        State state = State::Text;
        std::string params;             // CSI parameter bytes so far
        uint16_t curFg = TERM_COLOR_DEFAULT;
        uint16_t curBg = TERM_COLOR_DEFAULT;
        bool curBold = false;
        bool rewritten = false;         // a carriage return already flushed this line

        std::vector<TermSpan> pending;
        std::vector<TermLine> ready;
        size_t pendingChars = 0;        // codepoints in pending, counted as they arrive

        void writeControl(unsigned char c);
        void writeEscapeByte(unsigned char c);
        void emitLine(bool rewrite);
        void applySgr(const std::string& parameters);
        void appendSpan(const char* data, size_t size);
    };

}
