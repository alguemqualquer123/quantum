// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "imgui.h"

#include "util/ShellSession.h"
#include "util/TerminalText.h"
#include "window/widget/SelectableTextView.h"

namespace quantum::editor {

    class Project;

    // A shell kept alive inside the editor: type a command, get its real output back,
    // and the directory, variables and background jobs survive from one command to the
    // next because it is the same process the whole time.
    //
    // The shell is wired to pipes rather than to a pseudo-terminal, and that is the
    // whole trade: without a TTY it never paints a prompt and a full-screen program
    // (vim, htop, the opencode interface) refuses to draw - in exchange the output is
    // a plain byte stream, so it arrives as selectable, copyable lines and a runaway
    // command is still killable as a process tree. The toolbar hands those programs to
    // a real OS terminal window instead.
    class TerminalWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Terminal";

        explicit TerminalWindow(Project* project);
        ~TerminalWindow();

        TerminalWindow(const TerminalWindow&) = delete;
        TerminalWindow& operator=(const TerminalWindow&) = delete;

        void show();
        void setOpen(bool open);
        bool isOpen() const;

        // Open the panel and put the caret in the command line (the Ctrl+` shortcut).
        void focus();

    private:
        static constexpr size_t MAX_LINES = 4000;      // scrollback kept on screen
        static constexpr size_t TRIM_TO_LINES = 3000;  // trimmed in bulk, so the rebuild stays amortised
        static constexpr size_t MAX_HISTORY = 200;
        static constexpr int INPUT_SIZE = 2048;

        struct RawChunk {
            std::string data;
            bool fromStderr = false;
        };

        void drawToolbar();
        void drawBody();
        void drawCommandLine();
        void drawShellPicker();
        void openExternalTerminal();

        void drainIncoming();                            // worker-thread bytes -> parsed lines
        void consume(const RawChunk& chunk);
        void commit(const TermLine& line);
        void appendEcho(const std::string& text, ImU32 color);
        void appendLineGeometry(const TermLine& line, size_t index);
        void rebuildScrollback();
        void resetHistory();

        void submit(const std::string& command);
        void requestCwd();
        void startSession();
        void stopSession();

        std::string promptPrefix() const;
        ImU32 colorFor(uint16_t index, bool bold) const;

        Project* project;
        ShellSession session;
        TerminalText parser;

        std::mutex incomingMutex;
        std::vector<RawChunk> incoming;   // filled by the reader threads, drained on the UI thread

        // The transcript is kept twice: as parsed lines (so a rewrite or a trim can be
        // replayed) and as the one flat paragraph the selectable view wants, which is
        // only ever appended to. lineTextStart/lineSpanEnd record where each line lands
        // in that paragraph so it can be cut back out.
        std::vector<TermLine> lines;
        std::vector<ImU32> lineOverride;   // forced colour for the panel's own echo lines
        std::vector<size_t> lineTextStart;
        std::vector<size_t> lineSpanEnd;
        std::vector<SelectableTextView::Paragraph> paragraphs;
        SelectableTextView view;

        std::string shellLaunch;
        std::string shellKind;
        std::string cwd;
        std::string startError;

        std::vector<std::string> commandHistory;
        int historyCursor = -1;
        std::vector<char> input;

        ImU32 defaultFg = 0;   // refreshed every frame: the theme's own text colour
        ImU32 dimFg = 0;
        bool chunkIsStderr = false;

        bool windowOpen = false;
        bool focusRequested = false;
        bool focusInputRequested = false;
        bool forceScrollBottom = false;
        bool started = false;
    };

}
