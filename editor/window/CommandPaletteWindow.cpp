// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "CommandPaletteWindow.h"
#include "ai/EditorActionRegistry.h"
#include "external/IconsFontAwesome6.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "imgui.h"
#include "imgui_internal.h"

using namespace quantum::editor;

static char ToLowerChar(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

static bool IsWordStartAt(const char* text, int idx) {
    if (idx <= 0) {
        return idx == 0;
    }
    const char prev = text[idx - 1];
    return prev == ' ' || prev == '_' || prev == '-' || prev == ':' || prev == '.' || prev == '/';
}

int CommandPaletteWindow::fuzzyScore(const char* query, const char* haystack) {
    const size_t queryLen = std::strlen(query);
    const size_t hayLen = std::strlen(haystack);
    if (queryLen == 0) {
        return 0;
    }
    if (hayLen == 0) {
        return -1;
    }

    int score = 0;
    int prev = -2;
    int run = 0;
    size_t pos = 0;
    for (size_t qi = 0; qi < queryLen; ++qi) {
        const char qc = ToLowerChar(query[qi]);
        size_t found = hayLen;
        for (size_t k = pos; k < hayLen; ++k) {
            if (ToLowerChar(haystack[k]) == qc) {
                found = k;
                break;
            }
        }
        if (found == hayLen) {
            return -1;
        }

        int item = 1;
        if (static_cast<int>(found) == prev + 1) {
            item += 2 + run * 2;
            ++run;
        } else {
            run = 0;
        }
        if (IsWordStartAt(haystack, static_cast<int>(found))) {
            item += 4;
        }
        if (found > pos + 1) {
            item -= 1;
        }
        score += item;

        prev = static_cast<int>(found);
        pos = found + 1;
    }
    return score;
}

CommandPaletteWindow::CommandPaletteWindow() {
    searchBuffer[0] = '\0';
    selectedIndex = 0;
    visible = false;
    openRequested = false;
    closeRequested = false;
    focusInputRequested = false;
    scrollRequested = false;
    rebuildCommands();
}

void CommandPaletteWindow::rebuildCommands() {
    commands = customCommands;

    for (const auto& tool : ai::EditorActionRegistry::tools()) {
        PaletteCommand command;
        command.id = tool.name;
        command.label = tool.name;
        command.description = tool.description;
        commands.push_back(std::move(command));
    }

    filterMatches();
}

void CommandPaletteWindow::setExecuteCallback(std::function<void(const std::string& actionId)> callback) {
    executeCallback = std::move(callback);
}

void CommandPaletteWindow::setCustomCommands(std::vector<PaletteCommand> commands) {
    customCommands = std::move(commands);
    rebuildCommands();
}

void CommandPaletteWindow::filterMatches() {
    matches.clear();

    if (searchBuffer[0] == '\0') {
        matches.reserve(commands.size());
        for (int i = 0; i < static_cast<int>(commands.size()); ++i) {
            matches.push_back(i);
        }
        selectedIndex = 0;
        return;
    }

    std::vector<std::pair<int, int>> scored;
    for (int i = 0; i < static_cast<int>(commands.size()); ++i) {
        const PaletteCommand& command = commands[i];
        const int labelScore = fuzzyScore(searchBuffer, command.label.c_str());
        const int descScore = fuzzyScore(searchBuffer, command.description.c_str());
        const int idScore = command.id != command.label
            ? fuzzyScore(searchBuffer, command.id.c_str())
            : -1;
        if (labelScore < 0 && descScore < 0 && idScore < 0) {
            continue;
        }
        const int total = std::max(labelScore, 0) * 3 +
                          std::max(std::max(descScore, 0), std::max(idScore, 0));
        scored.push_back({total, i});
    }

    std::sort(scored.begin(), scored.end(), [this](const std::pair<int, int>& a, const std::pair<int, int>& b) {
        if (a.first != b.first) {
            return a.first > b.first;
        }
        return commands[a.second].label < commands[b.second].label;
    });

    if (scored.size() > MAX_RESULTS) {
        scored.resize(MAX_RESULTS);
    }
    for (const auto& entry : scored) {
        matches.push_back(entry.second);
    }
    selectedIndex = 0;
    scrollRequested = true;
}

void CommandPaletteWindow::executeSelected() {
    if (selectedIndex < 0 || selectedIndex >= static_cast<int>(matches.size())) {
        return;
    }
    pendingExecuteId = commands[matches[selectedIndex]].id;
    visible = false;
    closeRequested = false;
    ImGui::CloseCurrentPopup();
}

void CommandPaletteWindow::toggle() {
    if (visible) {
        closeRequested = true;
    } else {
        openRequested = true;
    }
}

bool CommandPaletteWindow::isVisible() const {
    return visible;
}

void CommandPaletteWindow::show() {
    if (!visible && openRequested) {
        visible = true;
        openRequested = false;
        closeRequested = false;
        focusInputRequested = true;
        searchBuffer[0] = '\0';
        selectedIndex = 0;
        filterMatches();
        ImGui::OpenPopup(PALETTE_NAME);
    }

    if (!visible) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f,
                                   viewport->Pos.y + viewport->Size.y * 0.28f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(620.0f, 0.0f), ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_Modal |
                             ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_AlwaysAutoResize;

    if (ImGui::BeginPopupModal(PALETTE_NAME, nullptr, flags)) {
        if (closeRequested) {
            closeRequested = false;
            visible = false;
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 10.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextUnformatted(ICON_FA_MAGNIFYING_GLASS);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (focusInputRequested) {
            ImGui::SetKeyboardFocusHere(0);
            focusInputRequested = false;
        }
        const bool enterPressed = ImGui::InputText("##palette_search", searchBuffer,
                                                   SEARCH_BUFFER_SIZE,
                                                   ImGuiInputTextFlags_EnterReturnsTrue |
                                                   ImGuiInputTextFlags_NoHorizontalScroll);
        const bool inputActive = ImGui::IsItemActive();
        ImGui::PopStyleVar();

        if (lastQuery != searchBuffer) {
            lastQuery = searchBuffer;
            filterMatches();
        }

        if (inputActive) {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                visible = false;
                closeRequested = false;
                ImGui::CloseCurrentPopup();
            } else if (!matches.empty() && ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) {
                selectedIndex = std::min(selectedIndex + 1, static_cast<int>(matches.size()) - 1);
                scrollRequested = true;
            } else if (!matches.empty() && ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) {
                selectedIndex = std::max(selectedIndex - 1, 0);
                scrollRequested = true;
            } else if (!matches.empty() && ImGui::IsKeyPressed(ImGuiKey_PageDown, true)) {
                selectedIndex = std::min(selectedIndex + 8, static_cast<int>(matches.size()) - 1);
                scrollRequested = true;
            } else if (!matches.empty() && ImGui::IsKeyPressed(ImGuiKey_PageUp, true)) {
                selectedIndex = std::max(selectedIndex - 8, 0);
                scrollRequested = true;
            }
        }

        if (enterPressed) {
            executeSelected();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const float lineHeight = ImGui::GetFrameHeight();
        const int visibleRows = std::max(std::min(static_cast<int>(matches.size()), 14), 1);
        if (matches.empty()) {
            ImGui::TextDisabled("No matching commands");
        } else if (ImGui::BeginChild("##palette_results", ImVec2(0, lineHeight * visibleRows), false)) {
            const ImGuiStyle& style = ImGui::GetStyle();
            const ImU32 selectedBg = ImGui::GetColorU32(ImGuiCol_HeaderActive);
            const ImU32 hoveredBg = ImGui::GetColorU32(ImGuiCol_HeaderHovered);

            for (int i = 0; i < static_cast<int>(matches.size()); ++i) {
                const PaletteCommand& command = commands[matches[i]];
                const bool selected = (i == selectedIndex);
                const float rowHeight = lineHeight + style.ItemSpacing.y;
                const ImVec2 rowMin = ImGui::GetCursorScreenPos();
                const float rowLocalX = ImGui::GetCursorPosX();
                const float rowWidth = ImGui::GetContentRegionAvail().x;

                ImGui::PushID(i);
                ImGui::InvisibleButton("##palette_row", ImVec2(rowWidth, rowHeight),
                                       ImGuiButtonFlags_NoNavFocus);
                const bool clicked = ImGui::IsItemClicked();
                const bool hovered = ImGui::IsItemHovered();
                if (clicked) {
                    executeSelected();
                }
                if (hovered && !selected) {
                    selectedIndex = i;
                }

                if (selected || hovered) {
                    ImDrawList* rowList = ImGui::GetWindowDrawList();
                    rowList->AddRectFilled(rowMin, ImVec2(rowMin.x + rowWidth, rowMin.y + rowHeight),
                                           selected ? selectedBg : hoveredBg, style.FrameRounding);
                }

                const float textY = rowMin.y + (rowHeight - ImGui::GetFontSize()) * 0.5f;
                ImGui::SetCursorScreenPos(ImVec2(rowMin.x + style.FramePadding.x, textY));
                ImGui::TextUnformatted(command.label.c_str());
                if (!command.description.empty()) {
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    ImGui::TextUnformatted(command.description.c_str());
                    ImGui::PopStyleColor();
                }
                if (!command.shortcut.empty()) {
                    const ImVec2 shortcutSize = ImGui::CalcTextSize(command.shortcut.c_str());
                    ImGui::SameLine(rowLocalX + rowWidth - shortcutSize.x - style.FramePadding.x);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    ImGui::TextUnformatted(command.shortcut.c_str());
                    ImGui::PopStyleColor();
                }

                if (selected && scrollRequested) {
                    ImGui::SetScrollHereY(0.5f);
                }

                ImGui::SetCursorScreenPos(ImVec2(rowMin.x, rowMin.y + rowHeight));
                ImGui::PopID();
            }
        }
        if (!matches.empty()) {
            ImGui::EndChild();
        }
        scrollRequested = false;

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::Text(ICON_FA_ARROW_UP ICON_FA_ARROW_DOWN " navigate    " ICON_FA_KEYBOARD " run    Esc close");
        ImGui::PopStyleColor();

        ImGui::EndPopup();
    }

    if (!pendingExecuteId.empty()) {
        std::string id = std::move(pendingExecuteId);
        pendingExecuteId.clear();
        if (executeCallback) {
            executeCallback(id);
        }
    }
}
