// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#include "UILabHost.h"

#include "App.h"
#include "Out.h"

#include <core/AppContext.h>
#include <layout/DockingLayout.h>
#include <panels/Panels.h>

using namespace doriax::editor;

UILabHost::UILabHost(App& owner)
    : owner(owner), ctx(new uilab::AppContext()) {
    // Doriax keeps its own bottom strip: the library would otherwise reserve the
    // height of a status bar it is not drawing.
    ctx->settings.showStatusBar = false;
    // ... and its own dock node tree, so the arrangement saved in editor_layout.ini
    // is not replaced by the laboratory's default one.
    ctx->installDefaultDockTree = false;
    installHooks();
}

UILabHost::~UILabHost() {
    Out::setUILabLogMirror(nullptr);
}

uilab::AppContext& UILabHost::context() {
    return *ctx;
}

ImGuiID UILabHost::dockSpaceId() const {
    return uilab::layout::DockSpaceId();
}

void UILabHost::installHooks() {
    ctx->host.menuBar = [this](uilab::AppContext&) {
        if (menuModel != nullptr)
            owner.showMenuModelContent(*menuModel);
    };

    // The console panel reads the library's own ring buffer, so every editor log
    // line is mirrored into it. Out only calls the mirror on the main thread.
    Out::setUILabLogMirror([this](LogType type, const std::string& message) {
        uilab::LogLevel level = uilab::LogLevel::Info;
        switch (type) {
            case LogType::Warning: level = uilab::LogLevel::Warning; break;
            case LogType::Error:   level = uilab::LogLevel::Error; break;
            case LogType::Success: level = uilab::LogLevel::Success; break;
            case LogType::Info:
            case LogType::Build:   break;
        }
        ctx->log.Push(uilab::LogChannel::Console, level, "Editor", message);
    });
}

void UILabHost::beginFrame() {
    uilab::layout::BeginFrame(*ctx);
}

void UILabHost::drawMenuBar(const PlatformMenuModel& menu) {
    menuModel = &menu;
    uilab::panels::DrawMenuBar(*ctx);
    menuModel = nullptr;
}
