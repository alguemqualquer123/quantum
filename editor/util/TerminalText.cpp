// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "util/TerminalText.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace quantum::editor {

namespace {

    // Codepoints, not bytes, so a tab stop does not land early after UTF-8 and the
    // length cap never cuts a character in half. Given the codepoints still allowed on
    // the line, returns where to cut the run and how many characters that is.
    std::pair<size_t, size_t> clampRun(const char* data, size_t size, size_t budget) {
        size_t cp = 0;
        for (size_t i = 0; i < size; i++) {
            const unsigned char c = static_cast<unsigned char>(data[i]);
            if ((c & 0xC0) == 0x80)
                continue;
            if (cp == budget)
                return { i, cp };
            cp++;
        }
        return { size, cp };
    }

    // Erases the last codepoint of the line, dropping the spans it runs out of, and
    // says whether anything went away so the caller can keep its count honest.
    bool eraseLastCodepoint(std::vector<TermSpan>& spans) {
        while (!spans.empty()) {
            std::string& text = spans.back().text;
            if (text.empty()) {
                spans.pop_back();
                continue;
            }
            size_t start = text.size() - 1;
            while (start > 0 && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80)
                start--;
            text.erase(start);
            if (!text.empty())
                return true;
            spans.pop_back();
        }
        return false;
    }

    // xterm 6x6x6 cube level for one 8-bit channel.
    int cubeLevel(int v) {
        if (v < 48) return 0;
        if (v < 115) return 1;
        if (v < 155) return 2;
        if (v < 195) return 3;
        if (v < 235) return 4;
        return 5;
    }

}

void TerminalText::appendSpan(const char* data, size_t size) {
    if (size == 0 || pendingChars >= MAX_PENDING_CHARS)
        return; // a line that never ends would eat the memory of the whole session

    const std::pair<size_t, size_t> taken = clampRun(data, size, MAX_PENDING_CHARS - pendingChars);
    if (taken.first == 0)
        return;

    if (!pending.empty() && pending.back().fg == curFg && pending.back().bg == curBg
        && pending.back().bold == curBold) {
        pending.back().text.append(data, taken.first);
    } else {
        TermSpan span;
        span.text.assign(data, taken.first);
        span.fg = curFg;
        span.bg = curBg;
        span.bold = curBold;
        pending.push_back(std::move(span));
    }
    pendingChars += taken.second;
}

void TerminalText::emitLine(bool rewrite) {
    TermLine line;
    line.spans = std::move(pending);
    line.rewrite = rewrite;
    pending.clear();
    pendingChars = 0;
    ready.push_back(std::move(line));
}

void TerminalText::applySgr(const std::string& parameters) {
    std::vector<int> args;
    size_t pos = 0;
    while (pos <= parameters.size()) {
        size_t next = parameters.find(';', pos);
        if (next == std::string::npos)
            next = parameters.size();
        // An empty field is the CSI default of 0, which is what ":red" means.
        args.push_back(next > pos ? std::atoi(parameters.substr(pos, next - pos).c_str()) : 0);
        pos = next + 1;
    }
    if (args.empty())
        args.push_back(0);

    for (size_t i = 0; i < args.size(); i++) {
        const int a = args[i];
        if (a == 38 || a == 48) {
            const bool foreground = (a == 38);
            if (i + 2 < args.size() && args[i + 1] == 5) {
                const int index = std::clamp(args[i + 2], 0, 255);
                if (foreground) curFg = static_cast<uint16_t>(index);
                else curBg = static_cast<uint16_t>(index);
                i += 2;
                continue;
            }
            if (i + 4 < args.size() && args[i + 1] == 2) {
                // Quantised into the same 256-slot space the renderer already reads,
                // so one table serves both.
                const int r = std::clamp(args[i + 2], 0, 255);
                const int g = std::clamp(args[i + 3], 0, 255);
                const int b = std::clamp(args[i + 4], 0, 255);
                const int hi = std::max(r, std::max(g, b));
                const int lo = std::min(r, std::min(g, b));
                int index;
                if (hi - lo <= 24)
                    index = 232 + std::clamp((r + g + b) / 30, 0, 23);
                else
                    index = 16 + 36 * cubeLevel(r) + 6 * cubeLevel(g) + cubeLevel(b);
                if (foreground) curFg = static_cast<uint16_t>(index);
                else curBg = static_cast<uint16_t>(index);
                i += 4;
                continue;
            }
            break; // truncated sequence, stop rather than misread the rest
        }

        if (a == 0) {
            curFg = TERM_COLOR_DEFAULT;
            curBg = TERM_COLOR_DEFAULT;
            curBold = false;
        } else if (a == 1) {
            curBold = true;
        } else if (a == 22) {
            curBold = false;
        } else if (a >= 30 && a <= 37) {
            curFg = static_cast<uint16_t>(a - 30);
        } else if (a == 39) {
            curFg = TERM_COLOR_DEFAULT;
        } else if (a >= 90 && a <= 97) {
            curFg = static_cast<uint16_t>(a - 90 + 8);
        } else if (a >= 40 && a <= 47) {
            curBg = static_cast<uint16_t>(a - 40);
        } else if (a == 49) {
            curBg = TERM_COLOR_DEFAULT;
        } else if (a >= 100 && a <= 107) {
            curBg = static_cast<uint16_t>(a - 100 + 8);
        }
        // Everything else (intensity, underline, blink, inverse) is dropped: the
        // renderer has one font and no underline, so acting on it would lie.
    }
}

void TerminalText::writeControl(unsigned char c) {
    if (c == 0x1B) {
        state = State::Escape;
        return;
    }
    if (c == '\r') {
        // A rewound cursor: the line just flushed is the one the next text overwrites,
        // so it is marked as the rewrite only when it did overwrite something.
        if (!pending.empty())
            emitLine(rewritten);
        rewritten = true;
        return;
    }
    if (c == '\n') {
        if (!pending.empty())
            emitLine(rewritten);
        else if (!rewritten)
            emitLine(false); // an intentional blank line, not a CRLF already done by \r
        rewritten = false;
        return;
    }
    if (c == '\b') {
        if (eraseLastCodepoint(pending) && pendingChars > 0)
            pendingChars--;
        return;
    }
    if (c == '\t') {
        const size_t pad = 8 - (pendingChars % 8);
        for (size_t p = 0; p < pad; p++)
            appendSpan(" ", 1);
        return;
    }
    // BEL and the rest of the control range are dropped: a beep has nowhere to go
    // and the others are not meaningful inside a line.
}

void TerminalText::writeEscapeByte(unsigned char c) {
    switch (state) {
        case State::Escape:
            if (c == '[') {
                params.clear();
                state = State::Csi;
            } else if (c == ']') {
                state = State::Osc;
            } else if (c == '\\') {
                state = State::Text; // string terminator
            } else if (c == '(' || c == ')' || c == '*' || c == '+') {
                state = State::Charset; // eats one designator byte after this
            } else {
                state = State::Text;
            }
            break;

        case State::Csi:
            if (c >= 0x40 && c <= 0x7E) {
                if (c == 'm')
                    applySgr(params);
                state = State::Text; // every other CSI (cursor, erase, mode) is dropped
            } else {
                params.push_back(static_cast<char>(c));
            }
            break;

        case State::Osc:
            if (c == 0x07)
                state = State::Text;
            else if (c == 0x1B)
                state = State::OscEscape;
            break;

        case State::OscEscape:
        case State::Charset:
            state = State::Text;
            break;

        case State::Text:
            break;
    }
}

void TerminalText::feed(const char* data, size_t size) {
    size_t runStart = 0; // start of the plain-text run still to be flushed

    for (size_t i = 0; i < size; i++) {
        const unsigned char c = static_cast<unsigned char>(data[i]);

        if (state == State::Text) {
            const bool control = (c < 0x20) || c == 0x1B;
            if (!control)
                continue;
            appendSpan(data + runStart, i - runStart);
            writeControl(c);
        } else {
            writeEscapeByte(c);
        }
        runStart = i + 1;
    }

    if (state == State::Text)
        appendSpan(data + runStart, size - runStart);
}

bool TerminalText::pop(TermLine& out) {
    if (ready.empty())
        return false;
    out = std::move(ready.front());
    ready.erase(ready.begin());
    return true;
}

void TerminalText::reset() {
    state = State::Text;
    params.clear();
    curFg = TERM_COLOR_DEFAULT;
    curBg = TERM_COLOR_DEFAULT;
    curBold = false;
    rewritten = false;
    pendingChars = 0;
    pending.clear();
    ready.clear();
}

}
