// Application.h - GLFW/ImGui bootstrap and the per-frame orchestration.
//
// Owns the window, the OpenGL resources and the single AppContext, and decides
// each frame which panels/overlays are submitted. Everything else (panels,
// demo pages) only ever sees the AppContext reference.
#pragma once

#include <string>

#include "core/AppContext.h"
#include "ui/Texture.h"

struct GLFWwindow;

namespace uilab {

struct AppArgs {
    int width = 1680;
    int height = 980;
    int maxFrames = 0;        // 0 = run until the window closes
    bool selfTest = false;    // headless logic checks, no window
    bool verbose = false;     // also list the passing checks
    bool noDocking = false;   // floating windows instead of a dockspace
    bool vsync = true;
    bool startMaximized = true;
    std::string layoutPath;   // overrides settings.layoutIniPath when non-empty
    std::string screenshotPath; // --screenshot: write a BMP of the last frame here
    int startPage = -1;         // --page: demo category index, -1 = leave the default
    std::string error;
};

// Parses --width/--height/--frames/--self-test/--verbose/--no-docking/--vsync=0.
// Returns false when an unknown/malformed option is found (error is filled).
bool ParseArgs(int argc, char** argv, AppArgs& out, std::string& errorOut);
const char* UsageText();

class Application {
public:
    Application();
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // In self-test mode no window/OpenGL context is created.
    bool Initialize(const AppArgs& args);
    int Run();          // main loop (or the self-test); returns the exit code
    void Shutdown();

    AppContext& Context() { return ctx_; }

private:
    bool OpenWindow(std::string& errorOut);
    bool CreateImGui(std::string& errorOut);
    void DrawFrame(float dt);
    void DrawPanels();
    void DrawExtras();                    // about window, ImGui demos, debug HUD
    void HandleShortcuts();
    void HandleRequests();
    void UpdateWindowTitle();

    AppArgs args_;
    AppContext ctx_;
    TextureLibrary textures_;
    GLFWwindow* window_ = nullptr;
    double lastTime_ = 0.0;
    bool firstFrame_ = true;
    bool showFpsOverlay_ = false;
    bool initialized_ = false;
};

} // namespace uilab
