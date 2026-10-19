// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#pragma once

#include "window/widget/SelectableTextView.h"

#include "imgui.h"

#include <string>
#include <vector>

namespace doriax::editor {

struct MarkdownStyle {
    ImU32 textColor = 0;
    ImU32 boldColor = 0;        // brighter text to simulate bold
    ImU32 italicColor = 0;      // slightly different tone for italic
    ImU32 codeColor = 0;        // inline code text
    ImU32 codeBgColor = 0;      // inline code background
    ImU32 headingColor = 0;
    ImU32 linkColor = 0;
    ImU32 blockquoteColor = 0;
    ImU32 hrColor = 0;
    ImFont* codeFont = nullptr;  // monospace font for code blocks
};

// Parse markdown content into SelectableTextView paragraphs with syntax-aware
// color spans. Supports: bold (**), italic (*), inline code (`), fenced code
// blocks (```), headings (#), unordered/ordered lists, blockquotes (>), and
// horizontal rules (---).
void renderMarkdown(const std::string& content,
                    const MarkdownStyle& style,
                    std::vector<SelectableTextView::Paragraph>& out);

} // namespace doriax::editor
