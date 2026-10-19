// Application.cpp - window/ImGui bootstrap and the per-frame orchestration.
#include "app/Application.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>

#if defined(_WIN32)
#include <windows.h>
#include <GL/gl.h>
#endif

#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include "canvas/CanvasScene.h"
#include "core/Registry.h"
#include "core/Types.h"
#include "demo/DemoHost.h"
#include "demo/SelfTest.h"
#include "layout/DockingLayout.h"
#include "panels/Panels.h"
#include "ui/Widgets.h"

namespace uilab {

namespace {

const char* PlatformName() {
#if defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#else
    return "Linux";
#endif
}

float StatusBarStripHeight() {
    return ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2.0f;
}

bool NextValue(int argc, char** argv, int& index, const std::string& flag, long& outValue, std::string& errorOut) {
    // Accepts "--flag 12" and "--flag=12".
    const std::string arg(argv[index]);
    const size_t equals = arg.find('=');
    std::string value;
    if (equals != std::string::npos) {
        value = arg.substr(equals + 1);
    } else if (index + 1 < argc) {
        value = argv[++index];
    } else {
        errorOut = flag + " needs a value.";
        return false;
    }
    errno = 0;
    const char* begin = value.c_str();
    char* end = nullptr;
    const long parsed = std::strtol(begin, &end, 10);
    if (end == begin || errno != 0 || (end != nullptr && *end != '\0')) {
        errorOut = flag + " expects an integer, got '" + value + "'.";
        return false;
    }
    outValue = parsed;
    return true;
}

// glReadPixels returns bottom-up rows, which is the order a 24-bit BMP stores
// them in, so the pixel buffer reaches the file untouched.
bool WriteScreenshotBmp(const std::string& path, int w, int h, const std::vector<unsigned char>& bgr,
                        std::string& errorOut) {
    const int stride = (w * 3 + 3) & ~3;
    const uint32_t pixelBytes = static_cast<uint32_t>(stride) * static_cast<uint32_t>(h);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        errorOut = "Cannot write " + path;
        return false;
    }

    unsigned char header[54] = {};
    const auto put32 = [&header](size_t at, uint32_t v) {
        header[at] = static_cast<unsigned char>(v & 0xFFu);
        header[at + 1] = static_cast<unsigned char>((v >> 8) & 0xFFu);
        header[at + 2] = static_cast<unsigned char>((v >> 16) & 0xFFu);
        header[at + 3] = static_cast<unsigned char>((v >> 24) & 0xFFu);
    };
    const auto put16 = [&header](size_t at, uint16_t v) {
        header[at] = static_cast<unsigned char>(v & 0xFFu);
        header[at + 1] = static_cast<unsigned char>((v >> 8) & 0xFFu);
    };
    header[0] = 'B';
    header[1] = 'M';
    put32(2, 54u + pixelBytes);
    put32(10, 54u);
    put32(14, 40u);
    put32(18, static_cast<uint32_t>(w));
    put32(22, static_cast<uint32_t>(h));
    put16(26, 1u);
    put16(28, 24u);
    put32(34, pixelBytes);

    out.write(reinterpret_cast<const char*>(header), sizeof(header));
    const std::vector<unsigned char> pad(stride - w * 3, 0);
    const size_t rowBytes = static_cast<size_t>(w) * 3;
    for (int y = 0; y < h; ++y) {
        out.write(reinterpret_cast<const char*>(bgr.data() + rowBytes * y), rowBytes);
        out.write(reinterpret_cast<const char*>(pad.data()), static_cast<std::streamsize>(pad.size()));
    }
    if (!out) {
        errorOut = "Disk error while writing " + path;
        return false;
    }
    errorOut.clear();
    return true;
}

bool CaptureFramebuffer(GLFWwindow* window, const std::string& path, std::string& errorOut) {
    int w = 0;
    int h = 0;
    glfwGetFramebufferSize(window, &w, &h);
    if (w <= 0 || h <= 0) {
        errorOut = "Framebuffer is empty (window minimised?).";
        return false;
    }
    std::vector<unsigned char> bgr(static_cast<size_t>(w) * static_cast<size_t>(h) * 3u);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, bgr.data());
    return WriteScreenshotBmp(path, w, h, bgr, errorOut);
}

// --page accepts a demo category index or a (case insensitive) fragment of its
// short/full name: 3, UMG, "ui toolkit".
int PageIndexFromName(const std::string& raw) {
    const std::string key = ToLower(raw);
    if (key.empty())
        return -1;
    bool digits = true;
    for (char c : key)
        digits = digits && c >= '0' && c <= '9';
    if (digits) {
        const int index = std::atoi(key.c_str());
        return index >= 0 && index < static_cast<int>(Category::Count) ? index : -1;
    }
    for (int i = 0; i < static_cast<int>(Category::Count); ++i) {
        const Category c = static_cast<Category>(i);
        if (ContainsNoCase(ToLower(CategoryShortName(c)), key) ||
            ContainsNoCase(ToLower(CategoryName(c)), key))
            return i;
    }
    return -1;
}

} // namespace

bool ParseArgs(int argc, char** argv, AppArgs& out, std::string& errorOut) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        const std::string flag = arg.substr(0, arg.find('='));
        long value = 0;

        if (flag == "--width") {
            if (!NextValue(argc, argv, i, flag, value, errorOut))
                return false;
            out.width = static_cast<int>(value);
        } else if (flag == "--height") {
            if (!NextValue(argc, argv, i, flag, value, errorOut))
                return false;
            out.height = static_cast<int>(value);
        } else if (flag == "--frames") {
            if (!NextValue(argc, argv, i, flag, value, errorOut))
                return false;
            out.maxFrames = static_cast<int>(value);
        } else if (flag == "--self-test") {
            out.selfTest = true;
        } else if (flag == "--verbose" || flag == "-v") {
            out.verbose = true;
        } else if (flag == "--no-docking") {
            out.noDocking = true;
        } else if (flag == "--no-vsync") {
            out.vsync = false;
        } else if (flag == "--no-maximize") {
            out.startMaximized = false;
        } else if (flag == "--help" || flag == "-h") {
            errorOut.clear();
            return false;
        } else if (flag == "--layout") {
            const size_t equals = arg.find('=');
            if (equals != std::string::npos) {
                out.layoutPath = arg.substr(equals + 1);
            } else if (i + 1 < argc) {
                out.layoutPath = argv[++i];
            } else {
                errorOut = "--layout needs a path.";
                return false;
            }
            if (out.layoutPath.empty()) {
                errorOut = "--layout needs a non-empty path.";
                return false;
            }
        } else if (flag == "--screenshot") {
            const size_t equals = arg.find('=');
            if (equals != std::string::npos) {
                out.screenshotPath = arg.substr(equals + 1);
            } else if (i + 1 < argc) {
                out.screenshotPath = argv[++i];
            } else {
                errorOut = "--screenshot needs a path.";
                return false;
            }
            if (out.screenshotPath.empty()) {
                errorOut = "--screenshot needs a non-empty path.";
                return false;
            }
        } else if (flag == "--page") {
            std::string name;
            const size_t equals = arg.find('=');
            if (equals != std::string::npos) {
                name = arg.substr(equals + 1);
            } else if (i + 1 < argc) {
                name = argv[++i];
            }
            out.startPage = PageIndexFromName(name);
            if (out.startPage < 0) {
                errorOut = "--page expects a category name (Basic, Layout, Slate, UMG, UI Toolkit, "
                           "IMGUI, uGUI) or its index.";
                return false;
            }
        } else {
            errorOut = "Unknown option: '" + arg + "'.";
            return false;
        }
    }

    if (out.width < 320 || out.height < 240) {
        errorOut = "--width/--height must be at least 320x240.";
        return false;
    }
    if (out.maxFrames < 0) {
        errorOut = "--frames must be positive.";
        return false;
    }
    errorOut.clear();
    return true;
}

const char* UsageText() {
    return
        "Universal UI Components Lab\n"
        "Usage: UniversalUIComponentsLab [options]\n"
        "  --width=N --height=N      initial window size (default 1680x980)\n"
        "  --no-maximize             start windowed instead of maximized\n"
        "  --frames=N                render N frames and exit (smoke test)\n"
        "  --screenshot=PATH         write a BMP of the last rendered frame (use with --frames)\n"
        "  --page=NAME               start on a demo page (Basic, Layout, Slate, UMG,\n"
        "                            UI Toolkit, IMGUI, uGUI)\n"
        "  --self-test               run the headless logic checks and exit\n"
        "  --verbose                 also list the checks that passed\n"
        "  --no-docking              open panels as floating windows\n"
        "  --no-vsync                disable vsync\n"
        "  --layout=PATH             use another docking ini file\n"
        "  --help                    show this text\n";
}

// ---------------------------------------------------------------------------
// Application
// ---------------------------------------------------------------------------
Application::Application() = default;

Application::~Application() { Shutdown(); }

bool Application::OpenWindow(std::string& errorOut) {
    if (!glfwInit()) {
        errorOut = "glfwInit() failed (no display server available?).";
        return false;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_ANY_PROFILE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    glfwWindowHint(GLFW_MAXIMIZED, args_.startMaximized ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_DECORATED, GLFW_TRUE);

    window_ = glfwCreateWindow(args_.width, args_.height, ctx_.appName.c_str(), nullptr, nullptr);
    if (!window_) {
        errorOut = "glfwCreateWindow() failed (OpenGL 3.3 unavailable?).";
        glfwTerminate();
        return false;
    }
    glfwMakeContextCurrent(window_);
    glfwSwapInterval(args_.vsync ? 1 : 0);
    return true;
}

bool Application::CreateImGui(std::string& errorOut) {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;   // keep everything inside one OS window
    if (args_.noDocking)
        io.ConfigFlags &= ~ImGuiConfigFlags_DockingEnable;

    // The docking layout lives in its own file so the default imgui.ini stays empty.
    io.IniFilename = nullptr;
    ctx_.settings.layoutIniPath = args_.layoutPath.empty() ? ctx_.settings.layoutIniPath : args_.layoutPath;

    if (!ImGui_ImplGlfw_InitForOpenGL(window_, true)) {
        errorOut = "ImGui_ImplGlfw_InitForOpenGL() failed.";
        return false;
    }
    if (!ImGui_ImplOpenGL3_Init("#version 330")) {
        errorOut = "ImGui_ImplOpenGL3_Init() failed (GLSL 330 unavailable?).";
        return false;
    }

    std::string textureError;
    if (!textures_.CreateAll(textureError))
        ctx_.LogConsole(LogLevel::Warning, "Textures", textureError);
    ctx_.textures = &textures_;

    const int major = glfwGetWindowAttrib(window_, GLFW_CONTEXT_VERSION_MAJOR);
    const int minor = glfwGetWindowAttrib(window_, GLFW_CONTEXT_VERSION_MINOR);
    char version[64];
    std::snprintf(version, sizeof(version), "%d.%d", major, minor);
    ctx_.rendererVersion = version;
    ctx_.platform = PlatformName();

    ctx_.theme.Apply();
    errorOut.clear();
    return true;
}

bool Application::Initialize(const AppArgs& args) {
    args_ = args;
    BuildDefaultRegistry(ctx_.registry);
    ctx_.scene.BuildDefaultScene();

    if (args_.selfTest) {
        initialized_ = true;   // no window, no GL
        return true;
    }

    std::string error;
    if (!OpenWindow(error)) {
        std::fprintf(stderr, "Initialization failed: %s\n", error.c_str());
        return false;
    }
    if (!CreateImGui(error)) {
        std::fprintf(stderr, "Initialization failed: %s\n", error.c_str());
        Shutdown();
        return false;
    }

    ctx_.LogConsole(LogLevel::Success, "App",
                   "Started on " + ctx_.platform + " with " + ctx_.renderer + " " + ctx_.rendererVersion);
    ctx_.LogConsole(LogLevel::Info, "App",
                   std::to_string(static_cast<long long>(ctx_.registry.Count())) + " components registered, " +
                       std::to_string(ctx_.registry.CountLive()) + " live bound.");
    initialized_ = true;
    return true;
}

void Application::Shutdown() {
    if (!initialized_)
        return;
    initialized_ = false;

    if (!args_.selfTest && window_) {
        if (ctx_.settings.saveLayoutOnExit) {
            std::string error;
            layout::SaveLayout(ctx_, error);
        }
        ctx_.textures = nullptr;
        textures_.DestroyAll();
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window_);
        glfwTerminate();
    }
    window_ = nullptr;
}

void Application::HandleShortcuts() {
    const ImGuiIO& io = ImGui::GetIO();
    const bool ctrl = io.KeyCtrl;

    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && ctx_.dialogs.IsOpen())
        return;   // the dialog manager owns Escape while a modal is open

    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        UndoRecord applied;
        if (ctx_.undo.Undo(ctx_.registry, applied))
            ctx_.LogConsole(LogLevel::Info, "History", "Undo " + applied.label);
        else
            ctx_.LogConsole(LogLevel::Warning, "History", "Nothing to undo.");
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
        UndoRecord applied;
        if (ctx_.undo.Redo(ctx_.registry, applied))
            ctx_.LogConsole(LogLevel::Info, "History", "Redo " + applied.label);
        else
            ctx_.LogConsole(LogLevel::Warning, "History", "Nothing to redo.");
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_C, false))
        ctx_.CopySelectedProperties();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V, false))
        ctx_.PastePropertiesIntoSelected();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_D, false))
        ctx_.DuplicateSelectedWidget();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_L, false))
        ctx_.requestResetLayout = true;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_F, false))
        ctx_.LogEvent("App", "Search focus requested (use the toolbar field).");

    if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
        const ThemeMode next = ctx_.theme.mode == ThemeMode::Dark ? ThemeMode::Light : ThemeMode::Dark;
        ctx_.theme.SetMode(next);
        ctx_.LogEvent("Theme", std::string("Switched to ") + ThemeModeName(next));
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
        ctx_.demo.playing = true;
        ctx_.LogEvent("Toolbar", "Simulation playing (F5)");
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F6, false)) {
        ctx_.demo.playing = false;
        ctx_.LogEvent("Toolbar", "Simulation paused (F6)");
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F7, false)) {
        ctx_.demo.playing = false;
        ctx_.demo.time = 0.0f;
        ctx_.LogEvent("Toolbar", "Simulation stopped (F7)");
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F11, false)) {
        ctx_.showDebugOverlayWindow = !ctx_.showDebugOverlayWindow;
        ctx_.LogEvent("App", ctx_.showDebugOverlayWindow ? "Debug overlay opened" : "Debug overlay closed");
    }

    if (ctx_.SelectedWidget() && !ctrl && ImGui::IsKeyPressed(ImGuiKey_Delete, false) &&
        !io.WantTextInput && !ctx_.dialogs.IsOpen()) {
        panels::RequestDeleteComponent(ctx_, *ctx_.SelectedWidget());
    }
}

void Application::HandleRequests() {
    std::string error;
    if (ctx_.requestSaveLayout) {
        ctx_.requestSaveLayout = false;
        if (!layout::SaveLayout(ctx_, error))
            ctx_.LogConsole(LogLevel::Error, "Layout", error);
        else
            ctx_.LogConsole(LogLevel::Info, "Layout", "Layout saved to " + ctx_.settings.layoutIniPath);
    }
    if (ctx_.requestLoadLayout) {
        ctx_.requestLoadLayout = false;
        if (!layout::LoadLayout(ctx_, error))
            ctx_.LogConsole(LogLevel::Error, "Layout", "Cannot load layout: " + error);
    }
    if (ctx_.requestCloseAllFloatingWindows) {
        ctx_.requestCloseAllFloatingWindows = false;
        layout::CloseAllFloatingWindows(ctx_);
    }
}

void Application::DrawPanels() {
    const AppSettings& s = ctx_.settings;

    if (s.showToolbar)
        panels::DrawToolbar(ctx_);
    if (s.showComponentTree)
        panels::DrawComponentTree(ctx_);
    if (s.showHierarchy)
        panels::DrawHierarchy(ctx_);
    if (s.showInspector)
        panels::DrawInspector(ctx_);
    if (s.showConsole)
        panels::DrawConsole(ctx_);
    if (s.showAssetBrowser)
        panels::DrawAssetBrowser(ctx_);
    if (s.showThemeEditor)
        panels::DrawThemeEditor(ctx_);

    demo::DrawHost(ctx_);
    demo::DrawFloatingWindows(ctx_);

    if (s.showStatusBar) {
        // The status strip is pinned to the bottom of the viewport instead of a dock node.
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const float height = StatusBarStripHeight();
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - height));
        ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, height));
        ImGui::SetNextWindowViewport(viewport->ID);
        panels::DrawStatusBar(ctx_);
    }
}

void Application::DrawExtras() {
    if (ctx_.settings.showImGuiDemo)
        ImGui::ShowDemoWindow(&ctx_.settings.showImGuiDemo);
    if (ctx_.settings.showImGuiMetrics)
        ImGui::ShowMetricsWindow(&ctx_.settings.showImGuiMetrics);
    if (ctx_.settings.showImGuiStyleEditor) {
        ImGui::SetNextWindowSize(ImVec2(460.0f, 620.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Style Editor (Dear ImGui)")) {
            ImGui::TextDisabled("The lab's own theme lives in the Theme Editor panel.");
            ImGui::Separator();
            ImGui::ShowStyleEditor();
        }
        ImGui::End();
    }

    if (ctx_.settings.showAbout) {
        ImGui::SetNextWindowSize(ImVec2(440.0f, 320.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("About Universal UI Components Lab", &ctx_.settings.showAbout)) {
            ImGui::TextUnformatted((ctx_.appName + " " + ctx_.appVersion).c_str());
            ImGui::Separator();
            ImGui::BulletText("C++17, Dear ImGui %s (docking), GLFW.", ImGui::GetVersion());
            ImGui::BulletText("%s on %s, %d frames rendered.", ctx_.renderer.c_str(), ctx_.platform.c_str(),
                              ctx_.stats.frameCount);
            ImGui::BulletText("%d components, %d live bound, %d canvas nodes.",
                              static_cast<int>(ctx_.registry.Count()), ctx_.registry.CountLive(), ctx_.scene.Count());
            ImGui::BulletText("Icons and textures are generated at runtime - no engine code or binary assets.");
            ImGui::Separator();
            ImGui::TextWrapped(
                "Pick a component in the tree, edit it in the Inspector and watch the Demo page update. "
                "Press F2 for the theme, F5/F6/F7 for the animation clock and Ctrl+L to reset the layout.");
            ImGui::Separator();
            if (ImGui::Button("Close"))
                ctx_.settings.showAbout = false;
            ImGui::SameLine();
            if (ImGui::Button("Copy version")) {
                ImGui::SetClipboardText((ctx_.appName + " " + ctx_.appVersion + " / ImGui " + ImGui::GetVersion())
                                            .c_str());
                ctx_.Notify(ToastKind::Info, "Copied", "Version information is on the clipboard.");
            }
        }
        ImGui::End();
    }
}

void Application::UpdateWindowTitle() {
    if (ctx_.stats.frameCount % 30 != 0)
        return;
    char title[192];
    std::snprintf(title, sizeof(title), "%s - %.0f fps - %d components", ctx_.appName.c_str(),
                  static_cast<double>(ctx_.stats.currentFps), static_cast<int>(ctx_.registry.Count()));
    glfwSetWindowTitle(window_, title);
}

void Application::DrawFrame(float dt) {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    ctx_.CaptureFrameStats(dt);
    ctx_.toasts.Update(dt);
    widgets::BeginEditFrame(ctx_);

    HandleShortcuts();

    const float density = ctx_.settings.compactMode ? 0.80f : 1.0f;
    if (density != ctx_.theme.style.densityScale) {
        ctx_.theme.style.densityScale = density;
        ctx_.theme.MarkDirty();
    }
    if (ctx_.theme.IsDirty())
        ctx_.theme.Apply();

    if (!args_.noDocking)
        layout::BeginFrame(ctx_);

    panels::DrawMenuBar(ctx_);
    DrawPanels();
    DrawExtras();

    // Modals and toasts are drawn last so they sit above every panel.
    ctx_.dialogs.Draw(ctx_.demo.wallTime);
    ctx_.toasts.Draw(dt);

    HandleRequests();
    UpdateWindowTitle();

    ImGui::Render();
    // The draw data only exists after Render(), so the counters are sampled here.
    ctx_.stats.CaptureImGuiStats();
    int fbWidth = 0, fbHeight = 0;
    glfwGetFramebufferSize(window_, &fbWidth, &fbHeight);
    if (fbWidth > 0 && fbHeight > 0) {
        glViewport(0, 0, fbWidth, fbHeight);
        const ImVec4 bg = ctx_.theme.Active().canvasBg;
        glClearColor(bg.x, bg.y, bg.z, bg.w);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

int Application::Run() {
    if (!initialized_)
        return 2;

    if (args_.selfTest)
        return demo::RunSelfTestAndPrint(ctx_, args_.verbose);

    if (!args_.layoutPath.empty())
        ctx_.requestLoadLayout = true;
    if (args_.startPage >= 0)
        ctx_.SelectCategory(static_cast<Category>(args_.startPage));

    int rendered = 0;
    const int shotAt = args_.screenshotPath.empty()
                           ? -1
                           : (args_.maxFrames > 0 ? args_.maxFrames : 60);
    while (!glfwWindowShouldClose(window_) && !ctx_.requestExit) {
        const double now = glfwGetTime();
        float dt = lastTime_ > 0.0 ? static_cast<float>(now - lastTime_) : 1.0f / 60.0f;
        lastTime_ = now;
        if (dt > 0.5f)
            dt = 0.5f;   // the window was dragged or the process was resumed

        glfwPollEvents();
        if (glfwGetWindowAttrib(window_, GLFW_ICONIFIED) == GLFW_TRUE)
            continue;

        DrawFrame(dt);
        if (shotAt > 0 && rendered + 1 == shotAt) {
            std::string error;
            if (CaptureFramebuffer(window_, args_.screenshotPath, error))
                std::printf("Screenshot written: %s\n", args_.screenshotPath.c_str());
            else
                std::fprintf(stderr, "Screenshot failed: %s\n", error.c_str());
        }
        glfwSwapBuffers(window_);
        ++rendered;

        if (args_.maxFrames > 0 && rendered >= args_.maxFrames)
            break;
    }

    ctx_.LogConsole(LogLevel::Info, "App", "Rendered " + std::to_string(rendered) + " frames.");
    return 0;
}

} // namespace uilab
