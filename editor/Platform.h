// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "System.h"
#include "Project.h"
#include <cstdarg>

namespace quantum::editor{

    class Platform : public System{
    private:
        Project* project;

        static int width;
        static int height;

    public:
        Platform(Project* project);

        static bool setSizes(int width, int height);

        int getScreenWidth() override;
        int getScreenHeight() override;

        std::string getAssetPath() override;
        std::string getLuaPath() override;
        std::string getShaderPath() override;
        std::string getUserDataPath() override;
        std::string getImportPath() override;

        sg_environment getSokolEnvironment() override;
        sg_swapchain getSokolSwapchain() override;

        bool isTouchDevice() override;

        bool readBackFramebuffer(unsigned char* out, int width, int height) override;

        void setMouseMode(MouseMode mode) override;

        void quit() override;

        void platformLog(const int type, const char *fmt, va_list args) override;
    };

}

