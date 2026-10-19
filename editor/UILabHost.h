// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#pragma once

#include <memory>

#include "imgui.h"

#include "PlatformMenu.h"

namespace uilab {
class AppContext;
}

namespace doriax::editor {

class App;

// The editor's UILab host. The library owns the chrome - the dockspace, the main menu
// bar, the panel windows - and this class owns the AppContext they read and the hooks
// that feed them doriax's data.
class UILabHost {
public:
    explicit UILabHost(App& owner);
    ~UILabHost();

    UILabHost(const UILabHost&) = delete;
    UILabHost& operator=(const UILabHost&) = delete;

    uilab::AppContext& context();

    // The dockspace root node id: doriax's own windows and tabs dock into it.
    ImGuiID dockSpaceId() const;

    // The dockspace host window. Call after the node tree is built and before the
    // panels draw.
    void beginFrame();

    // The main menu bar, drawing doriax's own menu model inside the library's bar.
    void drawMenuBar(const PlatformMenuModel& menu);

    // The library's console panel, reading the ring buffer Out mirrors into.
    // consoleTitle() is the window name the dock layout keys it by.
    const char* consoleTitle() const;
    void drawConsole();

private:
    void installHooks();

    App& owner;
    std::unique_ptr<uilab::AppContext> ctx;
    const PlatformMenuModel* menuModel = nullptr;
};

}
