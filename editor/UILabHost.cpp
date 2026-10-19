// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#include "UILabHost.h"

#include "App.h"
#include "AppSettings.h"
#include "Out.h"
#include "window/Properties.h"
#include "window/ResourcesWindow.h"
#include "window/Structure.h"

#include <core/AppContext.h>
#include <layout/DockingLayout.h>
#include <panels/Panels.h>

using namespace doriax::editor;

UILabHost::UILabHost(App& owner, Structure* structure, Properties* properties, ResourcesWindow* resources)
    : owner(owner),
      structureWindow(structure),
      propertiesWindow(properties),
      resourcesWindow(resources),
      ctx(new uilab::AppContext()) {
    // Doriax keeps its own bottom strip: the library would otherwise reserve the
    // height of a status bar it is not drawing.
    ctx->settings.showStatusBar = false;
    // ... and its own dock node tree, so the arrangement saved in editor_layout.ini
    // is not replaced by the laboratory's default one.
    ctx->installDefaultDockTree = false;
    // Exporting the console is a user action, and the library's default path is
    // relative to the cwd - that would drop a logs/ folder in the repo.
    ctx->settings.logExportPath =
        (AppSettings::getConfigDirectory() / "editor_console.log").string();
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

    // The library opens these three windows with doriax's names and doriax's
    // visibility flags, so the saved dock layout, the menu toggles and the close
    // button keep addressing the same windows while the panel draws the chrome.
    ctx->host.hierarchyPanel = { Structure::WINDOW_NAME, structureWindow->openFlag() };
    ctx->host.inspectorPanel = { Properties::WINDOW_NAME, propertiesWindow->openFlag() };
    ctx->host.assetBrowserPanel = { ResourcesWindow::WINDOW_NAME, resourcesWindow->openFlag() };
    structureWindow->setHostedWindow(true);
    propertiesWindow->setHostedWindow(true);
    resourcesWindow->setHostedWindow(true);

    ctx->host.hierarchy = [this](uilab::AppContext&) {
        structureWindow->drawContent();
    };
    ctx->host.inspector = [this](uilab::AppContext&) {
        propertiesWindow->drawContent();
    };
    ctx->host.assetBrowser = [this](uilab::AppContext&) {
        resourcesWindow->drawContent();
    };
}

void UILabHost::beginFrame() {
    uilab::layout::BeginFrame(*ctx);
}

void UILabHost::drawMenuBar(const PlatformMenuModel& menu) {
    menuModel = &menu;
    uilab::panels::DrawMenuBar(*ctx);
    menuModel = nullptr;
}

const char* UILabHost::consoleTitle() const {
    return uilab::panels::kConsoleTitle;
}

void UILabHost::drawConsole() {
    uilab::panels::DrawConsole(*ctx);
}

void UILabHost::drawDataPanels() {
    uilab::panels::DrawHierarchy(*ctx);
    uilab::panels::DrawInspector(*ctx);
    uilab::panels::DrawAssetBrowser(*ctx);
    // Work the content browser used to run between its End() and the next frame:
    // thumbnail eviction plus the rename / new folder confirmations.
    resourcesWindow->afterDraw();
}
