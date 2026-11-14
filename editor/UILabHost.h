// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

#include <memory>

#include "imgui.h"

#include "PlatformMenu.h"

namespace uilab {
class AppContext;
}

namespace quantum::editor {

class App;
class Structure;
class Properties;
class ResourcesWindow;

// The editor's UILab host. The library owns the chrome - the dockspace, the main menu
// bar, the panel windows - and this class owns the AppContext they read and the hooks
// that feed them quantum's data.
class UILabHost {
public:
    UILabHost(App& owner, Structure* structure, Properties* properties, ResourcesWindow* resources);
    ~UILabHost();

    UILabHost(const UILabHost&) = delete;
    UILabHost& operator=(const UILabHost&) = delete;

    uilab::AppContext& context();

    // The dockspace root node id: quantum's own windows and tabs dock into it.
    ImGuiID dockSpaceId() const;

    // The dockspace host window. Call after the node tree is built and before the
    // panels draw.
    void beginFrame();

    // The main menu bar, drawing quantum's own menu model inside the library's bar.
    void drawMenuBar(const PlatformMenuModel& menu);

    // The library's console panel, reading the ring buffer Out mirrors into.
    // consoleTitle() is the window name the dock layout keys it by.
    const char* consoleTitle() const;
    void drawConsole();

    // The library's data panels, showing quantum's outliner, property editor and
    // content browser. Their windows keep quantum's names, so the saved dock layout,
    // the menus and the close button address the same windows as before.
    void drawDataPanels();

private:
    void installHooks();

    App& owner;
    Structure* structureWindow;
    Properties* propertiesWindow;
    ResourcesWindow* resourcesWindow;
    std::unique_ptr<uilab::AppContext> ctx;
    const PlatformMenuModel* menuModel = nullptr;
};

}
