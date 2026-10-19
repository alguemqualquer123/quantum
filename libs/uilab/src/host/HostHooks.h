// HostHooks.h - seams for an application that embeds this library instead of
// running the standalone laboratory.
//
// The library keeps ownership of the chrome (the dockspace, the panel windows,
// the scroll regions, the look) and the host supplies the content of a panel by
// assigning a hook. A hook draws inside the panel's already-opened window or
// menu bar and must leave the ImGui stack balanced - it does not call
// Begin/End for the panel itself. When a hook is empty the panel draws the
// laboratory's own demo content, so the standalone app is unaffected.
#pragma once

#include <functional>

namespace uilab {

class AppContext;

// Where a host takes over a panel's content it usually also owns the panel's
// window: the name its saved dock layout addresses it by, and the flag its own
// menus and close button drive. Both are optional - unset keeps the library's
// defaults (its own title, no close button).
struct HostPanel {
    // The window name to open instead of the library's own title.
    const char* title = nullptr;
    // The host's visibility flag, passed as p_open so the title bar keeps a
    // close button that the host can read back.
    bool* pOpen = nullptr;

    const char* titleOr(const char* libraryTitle) const {
        return title != nullptr ? title : libraryTitle;
    }
};

struct HostHooks {
    // Main menu bar contents (called between BeginMainMenuBar and EndMainMenuBar).
    std::function<void(AppContext&)> menuBar;
    // Contents of the toolbar / status bar windows.
    std::function<void(AppContext&)> toolbar;
    std::function<void(AppContext&)> statusBar;
    // Contents of the docked data panels.
    std::function<void(AppContext&)> componentTree;
    std::function<void(AppContext&)> hierarchy;
    std::function<void(AppContext&)> inspector;
    std::function<void(AppContext&)> assetBrowser;

    // Window identity of those same panels.
    HostPanel componentTreePanel;
    HostPanel hierarchyPanel;
    HostPanel inspectorPanel;
    HostPanel assetBrowserPanel;
};

} // namespace uilab
