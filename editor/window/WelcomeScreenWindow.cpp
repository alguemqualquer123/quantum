// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "WelcomeScreenWindow.h"
#include "AppSettings.h"
#include "external/IconsFontAwesome6.h"

#include <algorithm>
#include <cctype>

#include "imgui.h"

using namespace quantum::editor;

static void CenterText(const char* text, float areaX, float areaWidth) {
    const ImVec2 size = ImGui::CalcTextSize(text);
    ImGui::SetCursorPosX(areaX + (areaWidth - size.x) * 0.5f);
    ImGui::TextUnformatted(text);
}

std::string WelcomeScreenWindow::humanizeTemplateName(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    bool capitalize = true;
    for (char c : name) {
        if (c == '-' || c == '_') {
            out.push_back(' ');
            capitalize = true;
            continue;
        }
        if (c == ' ') {
            out.push_back(c);
            capitalize = true;
            continue;
        }
        out.push_back(capitalize
            ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
            : c);
        capitalize = false;
    }
    return out;
}

WelcomeScreenWindow::WelcomeScreenWindow() {
    windowOpen = false;
    rescanPending = false;
}

void WelcomeScreenWindow::setCallbacks(const WelcomeCallbacks& callbacks) {
    this->callbacks = callbacks;
}

void WelcomeScreenWindow::setTemplatesDirectory(const std::filesystem::path& directory) {
    templatesDir = directory;
    rescanPending = true;
}

void WelcomeScreenWindow::rescanTemplates() {
    templates.clear();
    std::error_code ec;
    if (templatesDir.empty() || !fs::is_directory(templatesDir, ec)) {
        return;
    }
    for (const auto& entry : fs::directory_iterator(templatesDir, ec)) {
        if (entry.is_directory()) {
            templates.push_back(entry.path().filename().string());
        }
    }
    std::sort(templates.begin(), templates.end());
}

void WelcomeScreenWindow::setOpen(bool open) {
    if (open && !windowOpen) {
        rescanPending = true;
    }
    windowOpen = open;
}

bool WelcomeScreenWindow::isOpen() const {
    return windowOpen;
}

void WelcomeScreenWindow::show() {
    if (!windowOpen) {
        return;
    }

    if (rescanPending) {
        rescanTemplates();
        rescanPending = false;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport->WorkSize, ImGuiCond_Always);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                                   ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoSavedSettings;

    if (!ImGui::Begin(WINDOW_NAME, nullptr, flags)) {
        ImGui::End();
        return;
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float blockWidth = std::min(880.0f, avail.x - style.WindowPadding.x * 2.0f);
    const float blockX = (avail.x - blockWidth) * 0.5f;

    // Nothing behind this overlay is reachable, so it always has to offer a way
    // out: Escape or the X in the corner. File > Welcome Screen brings it back.
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        windowOpen = false;
    }
    const float closeSide = ImGui::GetFontSize() * 1.8f;
    const ImVec2 savedCursor = ImGui::GetCursorPos();
    ImGui::SetCursorPos(ImVec2(avail.x - closeSide, 0.0f));
    if (ImGui::InvisibleButton("##welcome_close", ImVec2(closeSide, closeSide))) {
        windowOpen = false;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    const ImVec2 glyphSize = ImGui::CalcTextSize(ICON_FA_XMARK);
    const ImVec2 glyphPos = ImVec2(ImGui::GetItemRectMin().x + (closeSide - glyphSize.x) * 0.5f,
                                   ImGui::GetItemRectMin().y + (closeSide - glyphSize.y) * 0.5f);
    ImGui::GetWindowDrawList()->AddText(glyphPos, ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                        ICON_FA_XMARK);
    ImGui::SetCursorPos(savedCursor);

    drawLogo();

    ImGui::Dummy(ImVec2(0, 8.0f));

    // The labels carry a leading icon, and CalcTextSize only measures the string
    // it is given: sizing the buttons off the bare text clipped their last glyph.
    const char* openLabel = ICON_FA_FOLDER_OPEN " Open project";
    const char* docsLabel = ICON_FA_BOOK " Documentation";
    const float openWidth = ImGui::CalcTextSize(openLabel).x + style.FramePadding.x * 2.0f;
    const float docsWidth = ImGui::CalcTextSize(docsLabel).x + style.FramePadding.x * 2.0f;
    const float buttonsWidth = openWidth + docsWidth + style.ItemSpacing.x;
    ImGui::SetCursorPosX(blockX + (blockWidth - buttonsWidth) * 0.5f);
    if (ImGui::Button(openLabel, ImVec2(openWidth, 0))) {
        if (callbacks.openProjectDialog) {
            callbacks.openProjectDialog();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(docsLabel, ImVec2(docsWidth, 0))) {
        if (callbacks.openDocumentation) {
            callbacks.openDocumentation();
        }
    }

    ImGui::Dummy(ImVec2(0, 16.0f));

    const float columnHeight = std::max(ImGui::GetContentRegionAvail().y, 120.0f);
    const float columnWidth = (blockWidth - style.ItemSpacing.x) * 0.5f;

    ImGui::SetCursorPosX(blockX);
    if (ImGui::BeginChild("##welcome_recents", ImVec2(columnWidth, columnHeight), false)) {
        drawRecentProjects(columnWidth, columnHeight);
    }
    ImGui::EndChild();

    ImGui::SameLine();
    if (ImGui::BeginChild("##welcome_templates", ImVec2(columnWidth, columnHeight), false)) {
        drawTemplateCards(columnWidth, columnHeight);
    }
    ImGui::EndChild();

    ImGui::End();
}

void WelcomeScreenWindow::drawLogo() {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float blockWidth = std::min(880.0f, avail.x);
    const float blockX = (avail.x - blockWidth) * 0.5f;

    ImGui::Dummy(ImVec2(0, 24.0f));

    const float iconSize = ImGui::GetFontSize() * 2.0f;
    const char* icon = ICON_FA_CUBES_STACKED;
    const float iconWidth = iconSize * 0.7f + ImGui::CalcTextSize(icon).x;
    const float titleWidth = ImGui::CalcTextSize("quantum").x;
    ImGui::SetCursorPosX(blockX + (blockWidth - (iconWidth + titleWidth + 12.0f)) * 0.5f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_Text));
    ImGui::TextUnformatted(icon);
    ImGui::SameLine();
    ImGui::SetCursorPosX(blockX + (blockWidth - (iconWidth + titleWidth + 12.0f)) * 0.5f + iconWidth + 12.0f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + iconSize * 0.32f);
    ImGui::TextUnformatted("quantum");
    ImGui::PopStyleColor();

    ImGui::SetCursorPosX(blockX + (blockWidth - ImGui::CalcTextSize("Game Engine Editor").x) * 0.5f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextUnformatted("Game Engine Editor");
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 16.0f));
    ImGui::SetCursorPosX(blockX + blockWidth * 0.5f - 60.0f);
    ImVec2 sepMin = ImGui::GetCursorScreenPos();
    //AddLineH, not AddLine: 1.93 removed the implicit +0.5 offset a 1px
    //separator relied on to stay inside one pixel row
    ImGui::GetWindowDrawList()->AddLineH(sepMin.x, sepMin.x + 120.0f, sepMin.y,
                                         ImGui::GetColorU32(ImGuiCol_Border));
    ImGui::SetCursorScreenPos(ImVec2(sepMin.x, sepMin.y + 1.0f));
    ImGui::Dummy(ImVec2(0, 8.0f));
}

void WelcomeScreenWindow::drawColumnHeader(const char* label, float width) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::Spacing();
}

void WelcomeScreenWindow::drawRecentProjects(float width, float height) {
    drawColumnHeader("RECENT PROJECTS", width);

    const std::vector<std::filesystem::path> recents = AppSettings::getRecentProjects();
    if (recents.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextUnformatted("No recent projects");
        ImGui::PopStyleColor();
        return;
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    const float rowHeight = 44.0f;

    if (ImGui::BeginChild("##recent_list", ImVec2(0, height - ImGui::GetFrameHeightWithSpacing()), false,
                          ImGuiWindowFlags_NoSavedSettings)) {
        for (size_t i = 0; i < recents.size(); ++i) {
            const std::filesystem::path& path = recents[i];
            const std::string name = path.filename().string();
            const std::string directory = path.parent_path().string();

            const ImVec2 rowMin = ImGui::GetCursorScreenPos();
            const float rowWidth = ImGui::GetContentRegionAvail().x;

            ImGui::PushID(static_cast<int>(i));
            ImGui::InvisibleButton("##recent_row", ImVec2(rowWidth, rowHeight));
            const bool clicked = ImGui::IsItemClicked();
            const bool hovered = ImGui::IsItemHovered();

            if (hovered) {
                ImGui::GetWindowDrawList()->AddRectFilled(rowMin, ImVec2(rowMin.x + rowWidth, rowMin.y + rowHeight),
                                                           ImGui::GetColorU32(ImGuiCol_HeaderHovered),
                                                           style.FrameRounding);
            }

            const float textX = rowMin.x + style.FramePadding.x * 2.0f;
            ImGui::SetCursorScreenPos(ImVec2(textX, rowMin.y + 6.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_Text));
            ImGui::Text(ICON_FA_FOLDER "  %s", name.c_str());
            ImGui::PopStyleColor();
            ImGui::SetCursorScreenPos(ImVec2(textX + ImGui::CalcTextSize(ICON_FA_FOLDER "  ").x,
                                             rowMin.y + 6.0f + ImGui::GetTextLineHeightWithSpacing()));
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::PushTextWrapPos(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - style.FramePadding.x * 2.0f);
            ImGui::TextUnformatted(directory.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();

            ImGui::SetCursorScreenPos(ImVec2(rowMin.x, rowMin.y + rowHeight));
            ImGui::PopID();

            if (clicked && callbacks.openRecentProject) {
                callbacks.openRecentProject(path);
                windowOpen = false;
            }
        }
    }
    ImGui::EndChild();
}

void WelcomeScreenWindow::drawTemplateCards(float width, float height) {
    drawColumnHeader("NEW PROJECT FROM TEMPLATE", width);

    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 cardSize((width - style.ItemSpacing.x) * 0.5f, 76.0f);

    if (ImGui::BeginChild("##template_grid", ImVec2(0, height - ImGui::GetFrameHeightWithSpacing()), false,
                          ImGuiWindowFlags_NoSavedSettings)) {
        struct Card {
            const char* icon;
            std::string title;
            std::string subtitle;
            std::string templateName;
            bool blank;
        };

        std::vector<Card> cards;
        cards.push_back({ICON_FA_PLUS, "Blank Project", "Start from nothing", "", true});
        for (const auto& name : templates) {
            cards.push_back({ICON_FA_CUBE, humanizeTemplateName(name), "Template", name, false});
        }

        const float gridWidth = ImGui::GetContentRegionAvail().x;
        for (size_t i = 0; i < cards.size(); ++i) {
            const Card& card = cards[i];
            const ImVec2 rowMin = ImGui::GetCursorScreenPos();

            ImGui::PushID(static_cast<int>(i));
            ImGui::InvisibleButton("##template_card", cardSize);
            const bool clicked = ImGui::IsItemClicked();
            const bool hovered = ImGui::IsItemHovered();

            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const ImVec2 cardMax(rowMin.x + cardSize.x, rowMin.y + cardSize.y);
            if (hovered) {
                drawList->AddRectFilled(rowMin, cardMax, ImGui::GetColorU32(ImGuiCol_HeaderHovered), style.FrameRounding);
            }
            drawList->AddRect(rowMin, cardMax, ImGui::GetColorU32(ImGuiCol_Border), style.FrameRounding,
                              ImDrawFlags_RoundCornersAll, 1.0f);

            ImGui::SetCursorScreenPos(ImVec2(rowMin.x + style.FramePadding.x * 2.0f, rowMin.y + 12.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextUnformatted(card.icon);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextUnformatted(card.title.c_str());
            ImGui::SetCursorScreenPos(ImVec2(rowMin.x + style.FramePadding.x * 2.0f,
                                             rowMin.y + 12.0f + ImGui::GetTextLineHeightWithSpacing() + 4.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextUnformatted(card.subtitle.c_str());
            ImGui::PopStyleColor();

            ImGui::SetCursorScreenPos(ImVec2(rowMin.x, rowMin.y + cardSize.y));
            ImGui::PopID();

            if (clicked) {
                if (card.blank) {
                    if (callbacks.createEmptyProject) {
                        callbacks.createEmptyProject();
                        windowOpen = false;
                    }
                } else if (callbacks.createFromTemplate) {
                    callbacks.createFromTemplate(card.templateName);
                    windowOpen = false;
                }
            }

            if (i + 1 < cards.size()) {
                if ((i + 1) % 2 == 0) {
                    ImGui::Dummy(ImVec2(0, style.ItemSpacing.y));
                } else {
                    ImGui::SameLine(gridWidth - cardSize.x);
                }
            }
        }
    }
    ImGui::EndChild();
}
