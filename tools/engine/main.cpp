// (c) Eduardo Doria
// SPDX-License-Identifier: MIT
//
// doriax-engine: standalone game runtime.
//
// Boots the doriax engine without the editor, following the same sequence an
// exported game uses:
//   1. main() -> <platform>::init (engine/platform/win/main.cpp:6)
//   2. DoriaxWin::init -> Engine::systemInit(argc, argv, new SystemWin)
//      (engine/platform/win/DoriaxWin.cpp:304-376)
//   3. Engine::systemInit fires the DORIAX_INIT bootstrap through
//      getOnInit().call() (engine/core/Engine.cpp:903-933, engine/core/Engine.h:135-144)
//   4. window + GL/Vulkan/D3D context, then systemViewLoaded/systemViewChanged
//      and the draw loop until WM_CLOSE.
// The editor-configured build of the doriax library is compiled with
// NO_LUA_INIT (root CMakeLists.txt:321), so the automatic `main.lua` load in
// LuaBinding::init (engine/core/script/LuaBinding.cpp:429) is skipped inside
// the dll; this runner replays it with the same lookup through the public
// LuaBinding API, then falls back to a built-in demo scene so something always
// launches.
//
// CLI:
//   doriax-engine [--project <path-to-project.yaml-or-export-dir>]
//                 [--scene <name>]
//                 [--window WxH]
//                 [--fullscreen]
//                 [--log-level N]
//
// --project resolves the resource roots the same way an exported game does:
// assets/, lua/ and shaders/ next to the working directory are read relative
// to it (engine/core/io/FileData.cpp:143-167 maps the asset://, lua:// and
// shader:// schemes onto SystemWin::getAssetPath, which is "assets" when no
// DORIAX_ASSET_PATH was baked in, engine/platform/win/SystemWin.cpp:113-139),
// so the runner simply makes the project directory the current one before the
// engine starts. A resources.pak in the project directory is only read when
// the library was built without DORIAX_EDITOR (engine/core/io/ResourcePack.cpp:214-225);
// in the editor-configuration build it warns that packed resources are not
// available.

#include "Doriax.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "lua.hpp"

#if defined(_WIN32)
#include <windows.h>
#include "DoriaxWin.h"
#elif defined(__APPLE__)
#include "DoriaxMac.h"
#else
#include "DoriaxLinux.h"
#endif

using namespace doriax;

namespace {

// ---- CLI state -------------------------------------------------------------

struct Options {
    std::filesystem::path projectPath; // project.yaml file or project/export dir
    std::string sceneName;             // lua entry override (or scene name), optional
    std::string title;                 // window title (project name when known)
    int windowWidth = 0;               // 0 = keep the platform default
    int windowHeight = 0;
    bool fullscreen = false;
    int logLevel = 1;                  // 0 quiet, 1 normal, 2 verbose
};

Options g_options;
std::filesystem::path g_originalCwd;

// Compiled-in fallback so a build-tree exe still finds engine/ sources and the
// demo logo before the engine/ dir is copied next to it.
#ifndef DORIAX_ENGINE_DIR_FALLBACK
#define DORIAX_ENGINE_DIR_FALLBACK ""
#endif

// ---- logging ---------------------------------------------------------------

void say(int level, const char* fmt, ...) {
    if (level > g_options.logLevel) return;
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stdout, fmt, args);
    va_end(args);
    std::fputc('\n', stdout);
}

// ---- engine dir resolution (mirrors editor/util/FileUtils.h:96-108) --------

std::filesystem::path getExecutableDir() {
    namespace fs = std::filesystem;
#if defined(_WIN32)
    wchar_t buffer[4096];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, 4096);
    if (length == 0 || length >= 4096) return {};
    std::error_code ec;
    return fs::weakly_canonical(fs::path(buffer), ec).parent_path();
#elif defined(__linux__)
    std::error_code ec;
    const fs::path executablePath = fs::read_symlink("/proc/self/exe", ec);
    if (ec) return {};
    return fs::weakly_canonical(executablePath, ec).parent_path();
#else
    return fs::current_path();
#endif
}

std::filesystem::path getEngineDir() {
    namespace fs = std::filesystem;

    const fs::path exeDir = getExecutableDir();
#if defined(__APPLE__)
    std::error_code ec;
    const fs::path bundled = exeDir.parent_path() / "Resources" / "engine";
    if (fs::exists(bundled, ec)) {
        return bundled;
    }
#endif
    std::error_code ec;
    const fs::path nextToExe = exeDir.empty() ? fs::path() : exeDir / "engine";
    if (!nextToExe.empty() && fs::exists(nextToExe, ec)) {
        return nextToExe;
    }
    const std::string fallback = DORIAX_ENGINE_DIR_FALLBACK;
    if (!fallback.empty() && fs::exists(fallback, ec)) {
        return fallback;
    }
    return nextToExe;
}

// ---- project resolution ----------------------------------------------------

// project.yaml holds "name: <title>" near the top (templates/fps-fps/project.yaml).
// A plain scan is enough here; yaml-cpp is an editor-target dependency and the
// runner keeps its link surface minimal.
std::string readProjectName(const std::filesystem::path& projectDir) {
    const std::filesystem::path yaml = projectDir / "project.yaml";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(yaml, ec)) return {};

    std::FILE* file = std::fopen(yaml.string().c_str(), "rb");
    if (!file) return {};

    char line[512];
    std::string name;
    for (int i = 0; i < 32 && std::fgets(line, sizeof(line), file); i++) {
        const std::string text(line);
        if (text.rfind("name:", 0) == 0) {
            name = text.substr(5);
            const size_t begin = name.find_first_not_of(" \t\"'");
            const size_t end = name.find_last_not_of(" \t\"'\r\n");
            if (begin == std::string::npos) name.clear();
            else name = name.substr(begin, end - begin + 1);
            break;
        }
    }
    std::fclose(file);
    return name;
}

// Accepts a path to project.yaml or to the project/export directory itself and
// makes it the process working directory, exactly where an exported game's
// assets/, lua/ and resources.pak live next to its executable. Returns the
// resolved project directory (empty on failure).
std::filesystem::path resolveAndEnterProject(const std::filesystem::path& given) {
    namespace fs = std::filesystem;

    fs::path path = given;
    if (path.is_relative()) path = g_originalCwd / path;
    path = path.lexically_normal();

    std::error_code ec;
    if (fs::is_regular_file(path, ec)) {
        // project.yaml (or any file inside the project): the dir is the answer
        path = path.parent_path();
    }
    if (!fs::is_directory(path, ec)) {
        say(0, "error: project path not found: %s", given.string().c_str());
        return {};
    }

    fs::path pak = path / "resources.pak";
    if (fs::is_regular_file(pak, ec)) {
        // The editor-configuration doriax library compiles the pack loader out
        // (engine/core/io/ResourcePack.cpp:214-225), so packed resources stay
        // invisible to this runner. Loose assets/lua/shaders still work.
        say(1, "warning: resources.pak found but packed resources need a non-editor engine build; using loose files");
    }

    fs::current_path(path, ec);
    if (ec) {
        say(0, "error: could not enter project directory: %s", path.string().c_str());
        return {};
    }
    return path;
}

// ---- lua project boot ------------------------------------------------------

// Same lookup LuaBinding::init performs (engine/core/script/LuaBinding.cpp:429-458):
// "lua://main.lua" at the asset root, then "lua://lua/main.lua". It is dead
// code in the editor-configuration dll (NO_LUA_INIT), so the runner replays it
// through the public LuaBinding API. `baseName` defaults to "main".
bool runLuaEntry(const std::string& baseName) {
    lua_State* L = LuaBinding::getLuaState();
    if (!L) return false;

    const std::string luadir = std::string("lua") + System::instance().getDirSeparator();
    const std::string suffix = baseName + ".lua";
    const std::string candidates[2] = {
        "lua://" + suffix,
        "lua://" + luadir + suffix,
    };

    for (const std::string& luafile : candidates) {
        Data filedata;
        if (filedata.open(luafile.c_str()) == FileErrors::FILEDATA_OK && filedata.length() > 0) {
            say(2, "boot: running %s", luafile.c_str());
            if (luaL_loadbuffer(L, (const char*)filedata.getMemPtr(), filedata.length(), luafile.c_str()) == 0) {
                if (LuaBinding::pcallWithTraceback(L, 0, LUA_MULTRET) != 0) {
                    say(0, "error: Lua error in %s: %s", luafile.c_str(),
                        LuaBinding::getLuaStackErrorString(L, -1).c_str());
                    lua_pop(L, 1);
                }
                return true; // entry exists even when it errored: do not fall back
            }
            say(0, "error: Lua load error in %s: %s", luafile.c_str(),
                LuaBinding::getLuaStackErrorString(L, -1).c_str());
            lua_pop(L, 1);
            return true;
        }
    }
    return false;
}

// ---- built-in demo scene ---------------------------------------------------

// The demo mirrors the standalone sample project engine/project/main.cpp:9-16.
Scene g_demoScene;
Image g_demoImage(&g_demoScene);

void bootDemoScene() {
    say(1, "boot: no project entry found, running the built-in demo scene");

    g_demoImage.setAnchorPreset(AnchorPreset::CENTER);

    // The logo is the sample project's own asset; when running loose it is
    // looked up in the working directory, then in engine/project/assets of
    // the engine dir copied next to the executable (the editor ships one).
    const std::string candidates[3] = {
        "doriax.png",
        (getEngineDir() / "project" / "assets" / "doriax.png").generic_string(),
        (std::filesystem::path(DORIAX_ENGINE_DIR_FALLBACK).empty()
             ? std::string()
             : (std::filesystem::path(DORIAX_ENGINE_DIR_FALLBACK) / "project" / "assets" / "doriax.png").generic_string()),
    };
    for (const std::string& path : candidates) {
        if (path.empty()) continue;
        Data probe;
        if (probe.open(path.c_str()) == FileErrors::FILEDATA_OK && probe.length() > 0) {
            g_demoImage.setTexture(path);
            break;
        }
    }

    g_demoScene.setBackgroundColor(0.10f, 0.12f, 0.16f);

    Engine::setScalingMode(Scaling::FITWIDTH);
    Engine::setCanvasSize(1000, 480);
    Engine::setScene(&g_demoScene);

    // A slow alpha pulse keeps the frame loop visible without assets.
    Engine::onUpdate.add("doriax-engine-demo-pulse", []() {
        const double t = Engine::getSystemTime();
        g_demoImage.setAlpha(0.8f + 0.2f * (float)std::sin(t * 1.5));
    });
}

// ---- bootstrap + window adjustments ----------------------------------------

// Registered exactly like the exported-game main does with DORIAX_INIT
// (engine/core/Engine.h:135-144): the engine fires it inside
// Engine::systemInit via getOnInit().call().
DORIAX_INIT void init() {
    bool booted = false;
    if (!g_options.projectPath.empty()) {
        if (!g_options.sceneName.empty()) booted = runLuaEntry(g_options.sceneName);
        if (!booted) booted = runLuaEntry("main");
    }
    if (!booted) bootDemoScene();
}

// DoriaxWin::init uses compile-time window settings (engine/platform/win/
// DoriaxWin.cpp:312-326); runtime overrides from the CLI are applied as soon
// as the view exists, through the public System API.
void subscribeWindowOptions() {
    Engine::onViewLoaded.add("doriax-engine-window", []() {
        System& sys = System::instance();
        if (g_options.windowWidth > 0 && g_options.windowHeight > 0) {
            sys.setWindowSize(g_options.windowWidth, g_options.windowHeight);
        }
        if (!g_options.title.empty()) {
            sys.setWindowTitle(g_options.title);
        }
        if (g_options.fullscreen) {
            sys.requestFullscreen();
        }
    });
}

// ---- argument parsing ------------------------------------------------------

bool parseInto(const char* value, int& out) {
    if (!value || !*value) return false;
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || (end && *end != '\0')) return false;
    out = (int)parsed;
    return true;
}

bool parseWindow(const char* value, int& w, int& h) {
    if (!value) return false;
    const std::string text(value);
    const size_t sep = text.find_first_of("xX");
    if (sep == std::string::npos) return false;
    return parseInto(text.substr(0, sep).c_str(), w) &&
           parseInto(text.substr(sep + 1).c_str(), h) && w > 0 && h > 0;
}

void printUsage(const char* argv0) {
    std::printf(
        "usage: %s [--project <path-to-project.yaml-or-export-dir>] [--scene <name>]\n"
        "          [--window WxH] [--fullscreen] [--log-level N]\n"
        "\n"
        "  --project   Project or exported-game directory. Its assets/, lua/ and\n"
        "              shaders/ become the resource roots for the run.\n"
        "  --scene     Lua entry script (name.lua) inside the project; defaults to\n"
        "              main.lua. Without a runnable project entry, the built-in\n"
        "              demo scene is launched instead.\n"
        "  --window    Initial window client size, e.g. 1280x720.\n"
        "  --fullscreen  Start in fullscreen.\n"
        "  --log-level 0 quiet, 1 normal (default), 2 verbose.\n",
        argv0);
}

bool parseArgs(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        const std::string arg(argv[i]);
        const auto next = [&](std::string& store) -> bool {
            if (i + 1 >= argc) {
                say(0, "error: %s requires a value", arg.c_str());
                return false;
            }
            store = argv[++i];
            return true;
        };

        if (arg == "--project") {
            std::string value;
            if (!next(value)) return false;
            g_options.projectPath = value;
        } else if (arg == "--scene") {
            if (!next(g_options.sceneName)) return false;
        } else if (arg == "--window") {
            std::string value;
            if (!next(value)) return false;
            if (!parseWindow(value.c_str(), g_options.windowWidth, g_options.windowHeight)) {
                say(0, "error: bad --window value: %s (expected WxH)", value.c_str());
                return false;
            }
        } else if (arg == "--fullscreen") {
            g_options.fullscreen = true;
        } else if (arg == "--log-level") {
            if (!parseInto(i + 1 < argc ? argv[i + 1] : nullptr, g_options.logLevel)) return false;
            i++;
            if (g_options.logLevel < 0 || g_options.logLevel > 2) {
                say(0, "error: --log-level must be 0, 1 or 2");
                return false;
            }
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return false;
        } else {
            say(0, "error: unknown argument: %s", arg.c_str());
            printUsage(argv[0]);
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (!parseArgs(argc, argv)) return 2;

    std::error_code ec;
    g_originalCwd = std::filesystem::current_path(ec);

    if (!g_options.projectPath.empty()) {
        const std::filesystem::path projectDir = resolveAndEnterProject(g_options.projectPath);
        if (projectDir.empty()) return 1;
        g_options.title = readProjectName(projectDir);
        say(1, "doriax-engine: project %s", projectDir.string().c_str());
    } else {
        g_options.title = "doriax-engine";
        say(1, "doriax-engine: no --project, running the built-in demo scene");
    }

    subscribeWindowOptions();

    // The platform init owns the frame loop and shuts the engine down on
    // window close (DoriaxWin::init, engine/platform/win/DoriaxWin.cpp:304-376).
#if defined(_WIN32)
    const int result = DoriaxWin::init(argc, argv);
#elif defined(__APPLE__)
    const int result = DoriaxMac::init(argc, argv);
#else
    const int result = DoriaxLinux::init(argc, argv);
#endif

    say(1, "doriax-engine: exited with %d", result);
    return result;
}
