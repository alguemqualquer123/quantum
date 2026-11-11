// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

// Before anything else pulls windows.h in through the engine headers: the min/max
// macros there break std::max() inside them and inside this file.
#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
#endif

#include "TerminalWindow.h"

#include "external/IconsFontAwesome6.h"
#include "App.h"
#include "AppSettings.h"
#include "Backend.h"
#include "Project.h"

#include "util/ProcessLaunch.h"
#include "util/ShellEnv.h"

#include <algorithm>
#include <cstring>
#include <utility>

#ifdef _WIN32
    #include <windows.h>
#endif

namespace quantum::editor {

namespace fs = std::filesystem;

namespace {

    // The xterm 256-colour table. The first sixteen are lifted from the stock VGA
    // values, which are nearly unreadable on a dark panel, and the rest is the
    // standard 6x6x6 cube plus grayscale, so what a real CLI prints lands where a user
    // expects it.
    const std::vector<ImU32>& termPalette() {
        static const std::vector<ImU32> colors = [] {
            std::vector<ImU32> table(256);
            static const int base16[16][3] = {
                { 12,  12,  12}, {197,  15,  31}, { 19, 161,  14}, {193, 156,   0},
                { 56, 117, 216}, {136,  23, 152}, {  0, 134, 179}, {204, 204, 204},
                {118, 118, 118}, {231,  72,  86}, { 22, 198,  12}, {249, 241, 165},
                { 59, 120, 255}, {180,   0, 158}, { 97, 214, 214}, {242, 242, 242}
            };
            for (int i = 0; i < 16; i++)
                table[i] = IM_COL32(base16[i][0], base16[i][1], base16[i][2], 255);

            static const int cube[6] = { 0, 95, 135, 175, 215, 255 };
            for (int r = 0; r < 6; r++) {
                for (int g = 0; g < 6; g++) {
                    for (int b = 0; b < 6; b++)
                        table[16 + 36 * r + 6 * g + b] = IM_COL32(cube[r], cube[g], cube[b], 255);
                }
            }
            for (int i = 0; i < 24; i++) {
                const int gray = 8 + i * 10;
                table[232 + i] = IM_COL32(gray, gray, gray, 255);
            }
            return table;
        }();
        return colors;
    }

    std::string lineText(const TermLine& line) {
        std::string text;
        for (const TermSpan& span : line.spans)
            text += span.text;
        return text;
    }

    std::string trimmed(const std::string& text) {
        const size_t begin = text.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos)
            return "";
        const size_t end = text.find_last_not_of(" \t\r\n");
        return text.substr(begin, end - begin + 1);
    }

    const char* shortShellName(const std::string& kind) {
        if (kind == "powershell") return "PowerShell";
        if (kind == "pwsh") return "PowerShell 7";
        if (kind == "cmd") return "Command Prompt";
        if (kind == "bash") return "Bash";
        if (kind == "sh") return "sh";
        return "shell";
    }

    // Everything the panel cannot show, a TUI needs a real terminal for. This is the
    // bare shell executable, without the "-Command -" that makes the in-panel session
    // read its commands from a pipe.
    std::string shellExecutablePath(const std::string& launch) {
        const size_t begin = launch.find('"');
        if (begin != std::string::npos) {
            const size_t end = launch.find('"', begin + 1);
            if (end != std::string::npos)
                return launch.substr(begin + 1, end - begin - 1);
        }
        const size_t space = launch.find(' ');
        return space == std::string::npos ? launch : launch.substr(0, space);
    }

}

TerminalWindow::TerminalWindow(Project* project)
    : project(project) {
    input.assign(INPUT_SIZE, '\0');
    // paragraphs[0] is the whole committed transcript; the live line is appended only
    // while the shell has written something it has not terminated with a newline.
    paragraphs.resize(1);
}

TerminalWindow::~TerminalWindow() {
    session.stop();
}

void TerminalWindow::setOpen(bool open) {
    windowOpen = open;
    if (open)
        focusInputRequested = true;
}

bool TerminalWindow::isOpen() const {
    return windowOpen;
}

void TerminalWindow::focus() {
    windowOpen = true;
    focusRequested = true;
    focusInputRequested = true;
}

ImU32 TerminalWindow::colorFor(uint16_t index, bool bold) const {
    if (index == TERM_COLOR_DEFAULT)
        return defaultFg;
    uint16_t slot = index;
    if (bold && slot < 8)
        slot += 8;   // the classic "bold means bright": one font cannot be both
    return termPalette()[slot & 0xFF];
}

void TerminalWindow::resetHistory() {
    lines.clear();
    lineOverride.clear();
    lineTextStart.clear();
    lineSpanEnd.clear();
    paragraphs.clear();
    paragraphs.resize(1);
    parser.reset();
}

void TerminalWindow::appendLineGeometry(const TermLine& line, size_t index) {
    SelectableTextView::Paragraph& paragraph = paragraphs[0];
    const ImU32 override = index < lineOverride.size() ? lineOverride[index] : 0;

    lineTextStart.push_back(paragraph.text.size());
    size_t cursor = paragraph.text.size();
    for (const TermSpan& span : line.spans) {
        if (span.text.empty())
            continue;
        if (override == 0) {
            const ImU32 fg = colorFor(span.fg, span.bold);
            const ImU32 bg = span.bg == TERM_COLOR_DEFAULT ? 0 : termPalette()[span.bg & 0xFF];
            if (fg != defaultFg || bg != 0)
                paragraph.colorSpans.push_back({ static_cast<int>(cursor),
                                                 static_cast<int>(cursor + span.text.size()),
                                                 fg, bg });
        }
        paragraph.text += span.text;
        cursor += span.text.size();
    }
    paragraph.text += '\n';
    lineSpanEnd.push_back(paragraph.colorSpans.size());
}

void TerminalWindow::rebuildScrollback() {
    SelectableTextView::Paragraph& paragraph = paragraphs[0];
    paragraph.text.clear();
    paragraph.colorSpans.clear();
    lineTextStart.clear();
    lineSpanEnd.clear();
    for (size_t i = 0; i < lines.size(); i++)
        appendLineGeometry(lines[i], i);
}

void TerminalWindow::appendEcho(const std::string& text, ImU32 color) {
    TermSpan span;
    span.text = text;
    TermLine line;
    line.spans.push_back(std::move(span));

    lineOverride.push_back(color);
    lines.push_back(std::move(line));
    appendLineGeometry(lines.back(), lines.size() - 1);

    if (lines.size() > MAX_LINES) {
        const size_t drop = lines.size() - TRIM_TO_LINES;
        lines.erase(lines.begin(), lines.begin() + static_cast<long>(drop));
        lineOverride.erase(lineOverride.begin(), lineOverride.begin() + static_cast<long>(drop));
        rebuildScrollback();
    }
}

void TerminalWindow::commit(const TermLine& incoming) {
    TermLine line = incoming;
    // A shell paints its own errors; only when it does not, the fact that the bytes
    // came down stderr is what makes the line red.
    if (chunkIsStderr) {
        bool painted = false;
        for (const TermSpan& span : line.spans)
            painted = painted || span.fg != TERM_COLOR_DEFAULT || span.bg != TERM_COLOR_DEFAULT;
        if (!painted) {
            for (TermSpan& span : line.spans)
                span.fg = 1;
        }
    }

    if (line.rewrite && !lines.empty()) {
        // A progress bar redraws the row it just printed, so the row it holds is
        // dropped before the new frame goes down the same way.
        const size_t textStart = lineTextStart.back();
        lines.pop_back();
        lineOverride.pop_back();
        lineTextStart.pop_back();
        lineSpanEnd.pop_back();
        paragraphs[0].text.resize(textStart);
        paragraphs[0].colorSpans.resize(lineSpanEnd.empty() ? 0 : lineSpanEnd.back());
    }

    lineOverride.push_back(0);
    lines.push_back(std::move(line));
    appendLineGeometry(lines.back(), lines.size() - 1);

    if (lines.size() > MAX_LINES) {
        const size_t drop = lines.size() - TRIM_TO_LINES;
        lines.erase(lines.begin(), lines.begin() + static_cast<long>(drop));
        lineOverride.erase(lineOverride.begin(), lineOverride.begin() + static_cast<long>(drop));
        rebuildScrollback();
    }
}

void TerminalWindow::consume(const RawChunk& chunk) {
    chunkIsStderr = chunk.fromStderr;
    parser.feed(chunk.data.data(), chunk.data.size());
    TermLine line;
    while (parser.pop(line)) {
        const std::string text = lineText(line);
        // The directory probe is the panel asking its own question, not something the
        // user typed to see: read the answer away and drop the line.
        if (text.rfind(ShellSession::PWD_MARKER, 0) == 0) {
            cwd = trimmed(text.substr(std::strlen(ShellSession::PWD_MARKER)));
            continue;
        }
        commit(line);
    }
    chunkIsStderr = false;
}

void TerminalWindow::drainIncoming() {
    std::vector<RawChunk> fresh;
    {
        std::lock_guard<std::mutex> lock(incomingMutex);
        fresh.swap(incoming);
    }
    for (const RawChunk& chunk : fresh)
        consume(chunk);
}

std::string TerminalWindow::promptPrefix() const {
    std::string dir = cwd;
    if (dir.empty())
        dir = project ? project->getProjectPath().string() : std::string(".");
    if (shellKind == "powershell" || shellKind == "pwsh")
        return "PS " + dir + "> ";
    if (shellKind == "bash" || shellKind == "sh")
        return dir + " $ ";
    return dir + "> ";
}

void TerminalWindow::requestCwd() {
    const std::string probe = ShellSession::cwdProbe(shellKind);
    if (!probe.empty())
        session.write(probe + "\n");
}

void TerminalWindow::startSession() {
    session.stop();
    // Only the escape-sequence state is dropped: a restarted shell is a new reader of
    // the same transcript, and clearing it here would eat the command that asked for it.
    parser.reset();
    startError.clear();

    shellLaunch = AppSettings::getTerminalShell();
    if (shellLaunch.empty())
        shellLaunch = ShellSession::defaultShellCommand();
    shellKind = ShellSession::kindOf(shellLaunch);

    fs::path dir;
    if (project)
        dir = project->getProjectPath();
    std::error_code ec;
    if (dir.empty() || !fs::exists(dir, ec))
        dir = fs::current_path(ec);
    cwd = dir.string();

    appendEcho(std::string("quantum ") + WINDOW_NAME + " - " + shortShellName(shellKind)
               + " over pipes: no prompt echo, and full-screen programs open in the"
                 " system terminal.", dimFg);

    const bool began = session.start(dir, shellLaunch,
        [this](const char* data, size_t size, bool fromStderr) {
            RawChunk chunk;
            chunk.data.assign(data, size);
            chunk.fromStderr = fromStderr;
            {
                std::lock_guard<std::mutex> lock(incomingMutex);
                incoming.push_back(std::move(chunk));
            }
            // Output lands on a reader thread while the editor may be idling, and an
            // idle editor draws nothing: without this the panel looks frozen.
            Backend::getApp().requestRedraw();
        });

    if (!began) {
        startError = session.lastError();
        appendEcho(startError, termPalette()[1]);
        return;
    }

    // cmd repeats every line it is handed, which would show each command twice - once
    // as the panel's own echo and once as the shell's.
    if (shellKind == "cmd")
        session.write("@echo off\n");

    requestCwd();
    forceScrollBottom = true;
}

void TerminalWindow::stopSession() {
    session.stop();
    appendEcho("(session stopped)", dimFg);
}

void TerminalWindow::submit(const std::string& command) {
    appendEcho(promptPrefix() + command, defaultFg);

    const std::string trimmedCommand = trimmed(command);
    if (!trimmedCommand.empty()) {
        commandHistory.push_back(command);
        if (commandHistory.size() > MAX_HISTORY)
            commandHistory.erase(commandHistory.begin());
    }
    historyCursor = -1;
    forceScrollBottom = true;

    if (trimmedCommand == "clear" || trimmedCommand == "cls") {
        // Handled here rather than by the shell: its own clear paints a whole screen of
        // cursor sequences that a line-oriented view can only garble.
        resetHistory();
        return;
    }

    if (!session.isRunning()) {
        appendEcho("(the shell is gone; starting another one)", dimFg);
        startSession();
        if (!session.isRunning())
            return;
    }

    if (!session.write(command + "\n")) {
        appendEcho("(the shell did not take the command: its process is gone)",
                   termPalette()[1]);
        startSession();
        return;
    }
    requestCwd();
}

void TerminalWindow::openExternalTerminal() {
    const std::string shellPath = shellExecutablePath(shellLaunch);
    if (shellPath.empty())
        return;
#ifdef _WIN32
    // `start` is what gives the new shell its own console window;
    // ProcessLaunch::detached would create it with CREATE_NO_WINDOW and show nothing.
    const std::string line = "cmd.exe /c start \"quantum\" \"" + shellPath + "\"";
    std::vector<char> mutableLine(line.begin(), line.end());
    mutableLine.push_back('\0');
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const char* workDir = cwd.empty() ? nullptr : cwd.c_str();
    if (CreateProcessA(nullptr, mutableLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                       nullptr, workDir, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
#else
    static const char* const emulators[] = { "x-terminal-emulator", "gnome-terminal",
                                             "konsole", "xfce4-terminal", "xterm" };
    for (const char* emulator : emulators) {
        const std::string found = ShellEnv::findExecutable(emulator);
        if (found.empty())
            continue;
        ProcessLaunch::detached(found, { "-e", shellPath }, cwd);
        break;
    }
#endif
}

void TerminalWindow::drawShellPicker() {
    // A closed combo began no popup window, so it has no EndCombo to match: calling it
    // here aborts on the first frame the panel is drawn.
    if (!ImGui::BeginCombo("##TerminalShell", shortShellName(shellKind)))
        return;

    struct Choice {
        const char* label;
        std::string launch;
    };
    std::vector<Choice> choices;
    choices.push_back({ "PowerShell", ShellSession::defaultShellCommand() });
    const std::string pwsh = ShellEnv::findExecutable("pwsh");
    if (!pwsh.empty()) {
        choices.push_back({ "PowerShell 7",
                            "\"" + pwsh + "\" -NoProfile -NonInteractive -Command -" });
    }
    choices.push_back({ "Command Prompt", "cmd.exe /Q /K" });
    const std::string bash = ShellEnv::findExecutable("bash");
    if (!bash.empty())
        choices.push_back({ "Bash", "\"" + bash + "\" --noprofile --norc" });

    for (const Choice& choice : choices) {
        const bool selected = choice.launch == shellLaunch;
        if (ImGui::Selectable(choice.label, selected)) {
            AppSettings::setTerminalShell(choice.launch);
            resetHistory();
            startSession();
        }
    }
    ImGui::EndCombo();
}

void TerminalWindow::drawToolbar() {
    drawShellPicker();
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_FA_ARROW_ROTATE_RIGHT " Restart")) {
        resetHistory();
        startSession();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_FA_STOP " Stop"))
        stopSession();
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_FA_ERASER " Clear"))
        resetHistory();
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_FA_UP_RIGHT_FROM_SQUARE " System terminal"))
        openExternalTerminal();

    const char* status = session.isRunning() ? "running" : "stopped";
    const ImVec4 statusColor = session.isRunning()
        ? ImVec4(0.45f, 0.80f, 0.45f, 1.0f)
        : ImVec4(0.85f, 0.55f, 0.35f, 1.0f);
    ImGui::SameLine(std::max(0.0f, ImGui::GetContentRegionMax().x
                                     - ImGui::CalcTextSize(status).x - ImGui::GetStyle().WindowPadding.x));
    ImGui::TextColored(statusColor, "%s", status);

    ImGui::TextDisabled("%s", cwd.empty() ? "(no directory)" : cwd.c_str());
}

void TerminalWindow::drawBody() {
    ImFont* codeFont = App::getCodeFont();
    paragraphs[0].color = defaultFg;
    paragraphs[0].font = codeFont;

    // The shell can write a line and hold it open (a progress bar, a password prompt);
    // showing only completed lines would hide exactly the part that is live.
    std::string liveText;
    std::vector<SelectableTextView::ColorSpan> liveSpans;
    size_t cursor = 0;
    for (const TermSpan& span : parser.pendingSpans()) {
        if (span.text.empty())
            continue;
        const ImU32 fg = colorFor(span.fg, span.bold);
        const ImU32 bg = span.bg == TERM_COLOR_DEFAULT ? 0 : termPalette()[span.bg & 0xFF];
        if (fg != defaultFg || bg != 0)
            liveSpans.push_back({ static_cast<int>(cursor),
                                  static_cast<int>(cursor + span.text.size()), fg, bg });
        liveText += span.text;
        cursor += span.text.size();
    }

    if (liveText.empty()) {
        if (paragraphs.size() > 1)
            paragraphs.resize(1);
    } else {
        if (paragraphs.size() < 2)
            paragraphs.push_back(SelectableTextView::Paragraph());
        paragraphs[1].text = std::move(liveText);
        paragraphs[1].colorSpans = std::move(liveSpans);
        paragraphs[1].color = defaultFg;
        paragraphs[1].font = codeFont;
    }

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float commandRow = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y * 2.0f;
    view.draw("##TerminalTranscript", ImVec2(avail.x, std::max(20.0f, avail.y - commandRow)),
              paragraphs, forceScrollBottom, 1.0f);
    forceScrollBottom = false;
}

void TerminalWindow::drawCommandLine() {
    ImFont* codeFont = App::getCodeFont();

    if (focusInputRequested) {
        ImGui::SetKeyboardFocusHere(0);
        focusInputRequested = false;
    }

    if (codeFont)
        ImGui::PushFont(codeFont);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_TextLink), "%s", promptPrefix().c_str());
    ImGui::SameLine();
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
    const bool submitted = ImGui::InputText("##TerminalCommand", input.data(), input.size(),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
    const bool focused = ImGui::IsItemActive();
    ImGui::PopItemWidth();

    // The command line is single-line, so the arrows never move a caret here and are
    // free to walk the history instead.
    if (focused && !commandHistory.empty()) {
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            historyCursor = historyCursor < 0 ? static_cast<int>(commandHistory.size() - 1)
                                              : std::max(0, historyCursor - 1);
            const std::string& entry = commandHistory[static_cast<size_t>(historyCursor)];
            std::strncpy(input.data(), entry.c_str(), input.size() - 1);
            input[static_cast<size_t>(input.size() - 1)] = '\0';
        }
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && historyCursor >= 0) {
            const int next = historyCursor + 1;
            if (next >= static_cast<int>(commandHistory.size())) {
                historyCursor = -1;
                input[0] = '\0';
            } else {
                historyCursor = next;
                const std::string& entry = commandHistory[static_cast<size_t>(next)];
                std::strncpy(input.data(), entry.c_str(), input.size() - 1);
                input[static_cast<size_t>(input.size() - 1)] = '\0';
            }
        }
    }

    if (submitted) {
        const std::string command(input.data());
        input[0] = '\0';
        submit(command);
    }
    if (codeFont)
        ImGui::PopFont();
}

void TerminalWindow::show() {
    if (!windowOpen)
        return;

    defaultFg = ImGui::GetColorU32(ImGuiCol_Text);
    dimFg = ImGui::GetColorU32(ImGuiCol_TextDisabled);

    if (!started) {
        started = true;
        startSession();
    }

    if (focusRequested) {
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }

    const bool wasOpen = windowOpen;
    if (!ImGui::Begin(WINDOW_NAME, &windowOpen)) {
        ImGui::End();
        if (wasOpen && !windowOpen)
            setOpen(false);
        return;
    }

    drainIncoming();
    drawToolbar();
    ImGui::Separator();
    drawBody();
    drawCommandLine();
    ImGui::End();
}

}
