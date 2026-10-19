// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "imgui.h"

namespace fs = std::filesystem;

namespace doriax::editor {
    struct WelcomeCallbacks {
        std::function<void(const std::filesystem::path& templateName)> createFromTemplate;
        std::function<void()> createEmptyProject;
        std::function<void()> openProjectDialog;
        std::function<void(const std::filesystem::path& projectPath)> openRecentProject;
        std::function<void()> openDocumentation;
    };

    class WelcomeScreenWindow {
    private:
        WelcomeCallbacks callbacks;
        std::vector<std::string> templates;
        std::filesystem::path templatesDir;
        bool windowOpen;
        bool rescanPending;

        void rescanTemplates();
        void drawLogo();
        void drawColumnHeader(const char* label, float width);
        void drawRecentProjects(float width, float height);
        void drawTemplateCards(float width, float height);
        static std::string humanizeTemplateName(const std::string& name);
    public:
        static constexpr const char* WINDOW_NAME = "Welcome";

        WelcomeScreenWindow();

        void setCallbacks(const WelcomeCallbacks& callbacks);
        void setTemplatesDirectory(const std::filesystem::path& directory);

        // Must be called every frame after the docked windows
        void show();

        void setOpen(bool open);
        bool isOpen() const;
    };
}
