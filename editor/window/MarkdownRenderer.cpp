// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#include "MarkdownRenderer.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace doriax::editor {

namespace {

// Simple syntax highlighting for code blocks.
void highlightCodeLine(const std::string& line, ImU32 baseColor,
                       std::vector<SelectableTextView::ColorSpan>& spans) {
    // Lua keywords
    static const char* luaKeywords[] = {
        "local", "function", "end", "if", "then", "else", "elseif",
        "for", "while", "do", "repeat", "until", "return", "break",
        "true", "false", "nil", "and", "or", "not", "self",
        "in", "pairs", "ipairs", "print", "require", "type",
        "metatable", "setmetatable", "getmetatable", "tostring", "tonumber",
        "pcall", "xpcall", "error", "assert", "select", "unpack",
        "table", "string", "math", "io", "os", "coroutine",
        nullptr
    };
    // C++ keywords
    static const char* cppKeywords[] = {
        "class", "struct", "void", "int", "float", "double", "bool", "char",
        "auto", "const", "static", "virtual", "override", "inline",
        "public", "private", "protected", "namespace", "using",
        "return", "if", "else", "for", "while", "do", "switch", "case",
        "break", "continue", "new", "delete", "nullptr", "true", "false",
        "this", "template", "typename", "typedef", "enum", "union",
        "sizeof", "dynamic_cast", "static_cast", "reinterpret_cast", "const_cast",
        "try", "catch", "throw", "noexcept",
        "int8_t", "int16_t", "int32_t", "int64_t",
        "uint8_t", "uint16_t", "uint32_t", "uint64_t",
        "size_t", "nullptr_t",
        nullptr
    };

    ImU32 keywordColor = IM_COL32(86, 156, 214, 255);   // blue
    ImU32 stringColor = IM_COL32(206, 145, 120, 255);   // orange
    ImU32 numberColor = IM_COL32(181, 206, 168, 255);   // green
    ImU32 commentColor = IM_COL32(87, 166, 74, 255);    // green dark
    ImU32 macroColor = IM_COL32(197, 134, 214, 255);    // purple

    size_t i = 0;
    while (i < line.size()) {
        // Single-line comment
        if (line[i] == '/' && i + 1 < line.size() && line[i + 1] == '/') {
            int start = static_cast<int>(i);
            spans.push_back({start, static_cast<int>(line.size()), commentColor});
            return;
        }
        // Lua comment
        if (line[i] == '-' && i + 1 < line.size() && line[i + 1] == '-') {
            int start = static_cast<int>(i);
            spans.push_back({start, static_cast<int>(line.size()), commentColor});
            return;
        }
        // String literal (double or single quote)
        if (line[i] == '"' || line[i] == '\'') {
            char quote = line[i];
            int start = static_cast<int>(i);
            ++i;
            while (i < line.size() && line[i] != quote) {
                if (line[i] == '\\' && i + 1 < line.size()) ++i;
                ++i;
            }
            if (i < line.size()) ++i; // closing quote
            spans.push_back({start, static_cast<int>(i), stringColor});
            continue;
        }
        // Number
        if (std::isdigit(static_cast<unsigned char>(line[i])) ||
            (line[i] == '.' && i + 1 < line.size() &&
             std::isdigit(static_cast<unsigned char>(line[i + 1])))) {
            int start = static_cast<int>(i);
            while (i < line.size() && (std::isalnum(static_cast<unsigned char>(line[i])) || line[i] == '.' || line[i] == '_')) {
                ++i;
            }
            spans.push_back({start, static_cast<int>(i), numberColor});
            continue;
        }
        // Preprocessor macro
        if (line[i] == '#') {
            int start = static_cast<int>(i);
            while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) {
                ++i;
            }
            spans.push_back({start, static_cast<int>(i), macroColor});
            continue;
        }
        // Word — check against keyword lists
        if (std::isalpha(static_cast<unsigned char>(line[i])) || line[i] == '_') {
            int start = static_cast<int>(i);
            while (i < line.size() && (std::isalnum(static_cast<unsigned char>(line[i])) || line[i] == '_')) {
                ++i;
            }
            std::string word = line.substr(static_cast<size_t>(start), i - static_cast<size_t>(start));
            bool isKeyword = false;
            for (const char** kw = luaKeywords; *kw; ++kw) {
                if (word == *kw) { isKeyword = true; break; }
            }
            if (!isKeyword) {
                for (const char** kw = cppKeywords; *kw; ++kw) {
                    if (word == *kw) { isKeyword = true; break; }
                }
            }
            if (isKeyword) {
                spans.push_back({start, static_cast<int>(i), keywordColor});
            }
            continue;
        }
        ++i;
    }
}

// Process inline markdown formatting within a single line.
// Returns the text with ColorSpan overrides for bold, italic, inline code, and links.
void processInlineMarkdown(const std::string& line, ImU32 baseColor,
                           const MarkdownStyle& style,
                           std::string& outText,
                           std::vector<SelectableTextView::ColorSpan>& outSpans) {
    outText.clear();
    outSpans.clear();

    size_t i = 0;
    while (i < line.size()) {
        // Inline code `...`
        if (line[i] == '`') {
            size_t end = line.find('`', i + 1);
            if (end == std::string::npos) end = line.size();
            else ++end; // include closing backtick
            int spanStart = static_cast<int>(outText.size());
            outText.append(line, i, end - i);
            int spanEnd = static_cast<int>(outText.size());
            outSpans.push_back({spanStart, spanEnd, style.codeColor, style.codeBgColor});
            i = end;
            continue;
        }
        // Bold **...**
        if (i + 1 < line.size() && line[i] == '*' && line[i + 1] == '*') {
            size_t end = line.find("**", i + 2);
            if (end == std::string::npos) {
                // No closing **, treat as literal
                outText += line[i];
                ++i;
                continue;
            }
            int spanStart = static_cast<int>(outText.size());
            outText.append(line, i + 2, end - i - 2);
            int spanEnd = static_cast<int>(outText.size());
            outSpans.push_back({spanStart, spanEnd, style.boldColor});
            i = end + 2;
            continue;
        }
        // Italic *...*
        if (line[i] == '*' && (i + 1 < line.size() && line[i + 1] != '*')) {
            size_t end = line.find('*', i + 1);
            if (end == std::string::npos || end == i + 1) {
                outText += line[i];
                ++i;
                continue;
            }
            int spanStart = static_cast<int>(outText.size());
            outText.append(line, i + 1, end - i - 1);
            int spanEnd = static_cast<int>(outText.size());
            outSpans.push_back({spanStart, spanEnd, style.italicColor});
            i = end + 1;
            continue;
        }
        // Link [text](url)
        if (line[i] == '[') {
            size_t closeBracket = line.find(']', i + 1);
            if (closeBracket != std::string::npos && closeBracket + 1 < line.size() &&
                line[closeBracket + 1] == '(') {
                size_t closeParen = line.find(')', closeBracket + 2);
                if (closeParen != std::string::npos) {
                    int spanStart = static_cast<int>(outText.size());
                    outText.append(line, i + 1, closeBracket - i - 1);
                    int spanEnd = static_cast<int>(outText.size());
                    outSpans.push_back({spanStart, spanEnd, style.linkColor});
                    i = closeParen + 1;
                    continue;
                }
            }
            outText += line[i];
            ++i;
            continue;
        }
        outText += line[i];
        ++i;
    }
}

} // namespace

void renderMarkdown(const std::string& content,
                    const MarkdownStyle& style,
                    std::vector<SelectableTextView::Paragraph>& out) {
    const size_t n = content.size();
    size_t pos = 0;
    bool inCode = false;
    std::string codeLanguage; // track language tag after opening ```

    while (pos <= n) {
        size_t nl = content.find('\n', pos);
        std::string line = content.substr(pos, (nl == std::string::npos ? n : nl) - pos);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        size_t firstNonSpace = line.find_first_not_of(" \t");
        bool isFence = (firstNonSpace != std::string::npos &&
                        line.compare(firstNonSpace, 3, "```") == 0);

        if (isFence) {
            if (!inCode) {
                // Opening fence — extract language tag for syntax highlighting.
                codeLanguage = line.substr(firstNonSpace + 3);
                // Trim whitespace
                size_t start = codeLanguage.find_first_not_of(" \t");
                if (start != std::string::npos) {
                    codeLanguage = codeLanguage.substr(start);
                } else {
                    codeLanguage.clear();
                }
            }
            inCode = !inCode;
            // Skip the fence line itself (don't output it).
        } else if (inCode) {
            // Code block line with syntax highlighting.
            std::vector<SelectableTextView::ColorSpan> codeSpans;
            highlightCodeLine(line, style.codeColor, codeSpans);
            out.push_back({line, style.codeColor, style.codeFont, false, std::move(codeSpans)});
        } else {
            // Check for heading: # ## ### etc.
            if (firstNonSpace != std::string::npos && line[firstNonSpace] == '#') {
                size_t hashEnd = firstNonSpace;
                while (hashEnd < line.size() && line[hashEnd] == '#') ++hashEnd;
                size_t headingTextStart = line.find_first_not_of(" \t", hashEnd);
                if (headingTextStart != std::string::npos) {
                    std::string headingText = line.substr(headingTextStart);
                    // Strip inline markdown from heading text for display
                    std::string cleanText;
                    std::vector<SelectableTextView::ColorSpan> headingSpans;
                    processInlineMarkdown(headingText, style.headingColor, style,
                                          cleanText, headingSpans);
                    // Override all spans to use headingColor base
                    for (auto& span : headingSpans) {
                        span.color = style.headingColor;
                    }
                    out.push_back({cleanText, style.headingColor, nullptr, false, std::move(headingSpans)});
                    goto nextLine;
                }
            }

            // Horizontal rule: ---, ***, ___
            if (firstNonSpace != std::string::npos) {
                std::string trimmed = line.substr(firstNonSpace);
                bool isHr = true;
                if (trimmed.size() >= 3) {
                    char c = trimmed[0];
                    if (c == '-' || c == '*' || c == '_') {
                        for (size_t k = 0; k < trimmed.size(); ++k) {
                            if (trimmed[k] != c && !std::isspace(static_cast<unsigned char>(trimmed[k]))) {
                                isHr = false;
                                break;
                            }
                        }
                        if (isHr && trimmed.size() >= 3) {
                            out.push_back({"---", style.hrColor, nullptr, true});
                            goto nextLine;
                        }
                    }
                }
            }

            // Blockquote: > text
            if (firstNonSpace != std::string::npos && line[firstNonSpace] == '>') {
                size_t textStart = firstNonSpace + 1;
                if (textStart < line.size() && line[textStart] == ' ') ++textStart;
                std::string quoteText = line.substr(textStart);
                std::string processed;
                std::vector<SelectableTextView::ColorSpan> quoteSpans;
                processInlineMarkdown(quoteText, style.blockquoteColor, style,
                                      processed, quoteSpans);
                // Indent blockquote
                std::string indented = "    " + processed;
                // Adjust span offsets for the indent
                for (auto& span : quoteSpans) {
                    span.begin += 4;
                    span.end += 4;
                    span.color = style.blockquoteColor;
                }
                out.push_back({indented, style.blockquoteColor, nullptr, false, std::move(quoteSpans)});
                goto nextLine;
            }

            // Unordered list: - * +
            static const std::string kListIndent = "    ";
            size_t i = (firstNonSpace == std::string::npos) ? line.size() : firstNonSpace;

            bool bullet = (i < line.size()) &&
                          (line[i] == '-' || line[i] == '*' || line[i] == '+') &&
                          (i + 1 < line.size()) && (line[i + 1] == ' ' || line[i + 1] == '\t');

            bool ordered = false;
            if (!bullet && i < line.size() && line[i] >= '0' && line[i] <= '9') {
                size_t j = i;
                while (j < line.size() && line[j] >= '0' && line[j] <= '9') ++j;
                ordered = (j < line.size()) && (line[j] == '.' || line[j] == ')') &&
                          (j + 1 < line.size()) && (line[j + 1] == ' ' || line[j + 1] == '\t');
            }

            if (bullet) {
                size_t rest = i + 1;
                while (rest < line.size() && (line[rest] == ' ' || line[rest] == '\t')) ++rest;
                std::string itemText = line.substr(rest);
                std::string processed;
                std::vector<SelectableTextView::ColorSpan> itemSpans;
                processInlineMarkdown(itemText, style.textColor, style, processed, itemSpans);
                std::string fullText = kListIndent + "\xE2\x80\xA2 " + processed;
                for (auto& span : itemSpans) {
                    span.begin += static_cast<int>(kListIndent.size()) + 2;
                    span.end += static_cast<int>(kListIndent.size()) + 2;
                }
                out.push_back({fullText, style.textColor, nullptr, false, std::move(itemSpans)});
            } else if (ordered) {
                std::string itemText = line.substr(i);
                std::string processed;
                std::vector<SelectableTextView::ColorSpan> itemSpans;
                processInlineMarkdown(itemText, style.textColor, style, processed, itemSpans);
                std::string fullText = kListIndent + processed;
                for (auto& span : itemSpans) {
                    span.begin += static_cast<int>(kListIndent.size());
                    span.end += static_cast<int>(kListIndent.size());
                }
                out.push_back({fullText, style.textColor, nullptr, false, std::move(itemSpans)});
            } else {
                // Regular paragraph text with inline formatting.
                std::string processed;
                std::vector<SelectableTextView::ColorSpan> textSpans;
                processInlineMarkdown(line, style.textColor, style, processed, textSpans);
                out.push_back({processed, style.textColor, nullptr, false, std::move(textSpans)});
            }
        }

    nextLine:
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
}

} // namespace doriax::editor
