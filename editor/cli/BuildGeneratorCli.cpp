// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "cli/BuildGeneratorCli.h"

#include "AppSettings.h"
#include "Quantum.h"
#include "EditorHost.h"
#include "Generator.h"
#include "Out.h"
#include "Platform.h"
#include "Project.h"
#include "pool/ShaderPool.h"
#include "shader/ShaderBuilder.h"
#include "util/CommandRunner.h"
#include "util/FileUtils.h"
#include "util/MsBuildProgress.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
    #define NOMINMAX
    #include <windows.h>
    #include <cstdio>
#endif

namespace quantum::editor::cli {

using ::quantum::Engine;
using ::quantum::ShaderBuildResult;
using ::quantum::ShaderKey;
using ::quantum::ShaderPool;

namespace {

    constexpr const char* INTERNAL_DIRNAME = ".quantum";
    constexpr const char* BUILD_DIRNAME = "build";
    constexpr const char* PROJECT_FILE = "project.yaml";

    // ---------------------------------------------------------------------
    // Console / naming helpers (same shape as the resource compiler CLI).
    // ---------------------------------------------------------------------

    // A GUI-subsystem binary has no console attached; reclaim the parent's so the
    // CLI output reaches the terminal.
    void attachHostConsoleIfNeeded() {
#if defined(_WIN32)
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE* stdoutFile = nullptr;
            FILE* stderrFile = nullptr;
            FILE* stdinFile = nullptr;
            (void)freopen_s(&stdoutFile, "CONOUT$", "w", stdout);
            (void)freopen_s(&stderrFile, "CONOUT$", "w", stderr);
            (void)freopen_s(&stdinFile,  "CONIN$",  "r", stdin);
        }
#endif
    }

    std::string getCommandName(const char* executableName) {
        if (!executableName || !*executableName) {
            return "quantum-build-generator";
        }
        const fs::path executablePath(executableName);
        const std::string filename = executablePath.filename().string();
        return filename.empty() ? std::string("quantum-build-generator") : filename;
    }

    // Accepts either the project directory or its project.yaml.
    fs::path normalizeProjectPath(const fs::path& path) {
        if (path.filename() == PROJECT_FILE) {
            return path.parent_path();
        }
        return path;
    }

    // CMake re-parses stored paths treating backslashes as escapes; forward
    // slashes work on every platform (see Generator::configureCMake).
    std::string toCMakePath(const std::string& p) {
        std::string out = p;
        std::replace(out.begin(), out.end(), '\\', '/');
        return out;
    }

    std::string trimLine(const std::string& line) {
        size_t begin = 0;
        while (begin < line.size() && (line[begin] == ' ' || line[begin] == '\t')) begin++;
        size_t end = line.size();
        while (end > begin && (line[end - 1] == '\r' || line[end - 1] == '\n'
                               || line[end - 1] == ' ' || line[end - 1] == '\t')) end--;
        return line.substr(begin, end - begin);
    }

    // ---------------------------------------------------------------------
    // Argument helpers.
    // ---------------------------------------------------------------------

    bool requireValue(int argc, char** argv, int& index, std::string& value, std::string& error) {
        if (index + 1 >= argc) {
            error = std::string("Missing value for ") + argv[index];
            return false;
        }
        value = argv[++index];
        return true;
    }

    // A compiler/generator selection parsed from --kit. Values may be empty,
    // meaning "leave this to CMake's own default".
    struct Kit {
        std::string cxxCompiler;
        std::string cCompiler;
        std::string generator;
        bool any() const {
            return !cxxCompiler.empty() || !cCompiler.empty() || !generator.empty();
        }
    };

    // Parses one --kit string. Tokens are key=value split on whitespace, but a
    // value that itself contains spaces (e.g. "Visual Studio 17 2022") is
    // supported: whitespace-separated tokens without a '=' continue the previous
    // value. Recognised keys: cxx, cc, gen. Unknown keys are reported.
    bool parseKitValue(const std::string& value, Kit& kit, std::string& error) {
        std::vector<std::string> tokens;
        {
            std::istringstream stream(value);
            std::string token;
            while (stream >> token) tokens.push_back(token);
        }

        std::string lastKey;
        for (const std::string& token : tokens) {
            const size_t eq = token.find('=');
            if (eq != std::string::npos) {
                lastKey = token.substr(0, eq);
                std::string val = token.substr(eq + 1);
                if (lastKey == "cxx") kit.cxxCompiler = val;
                else if (lastKey == "cc") kit.cCompiler = val;
                else if (lastKey == "gen") kit.generator = val;
                else {
                    error = "Unknown --kit key '" + lastKey + "' (expected cxx=, cc= or gen=)";
                    return false;
                }
            } else if (!lastKey.empty()) {
                // Continuation of a quoted value that held spaces.
                if (lastKey == "cxx") kit.cxxCompiler += " " + token;
                else if (lastKey == "cc") kit.cCompiler += " " + token;
                else if (lastKey == "gen") kit.generator += " " + token;
            } else {
                error = "Malformed --kit argument '" + token + "' (expected key=value)";
                return false;
            }
        }
        return true;
    }

    // Falls back to the editor's last-chosen kit for whatever --kit left unset,
    // so an unattended CI run matches what the developer picked interactively.
    // Only public static getters are used.
    void applyKitDefaults(Kit& kit) {
        if (kit.cxxCompiler.empty()) kit.cxxCompiler = AppSettings::getLastCMakeCxxCompiler();
        if (kit.cCompiler.empty()) kit.cCompiler = AppSettings::getLastCMakeCCompiler();
        if (kit.generator.empty()) kit.generator = AppSettings::getLastCMakeGenerator();
    }

    // ---------------------------------------------------------------------
    // Engine runtime / editor host guards (mirrors ResourceCompilerCli).
    // ---------------------------------------------------------------------

    struct EngineRuntimeGuard {
        bool initialized = false;
        ~EngineRuntimeGuard() {
            if (!initialized) {
                return;
            }
            ShaderPool::setShaderBuilder(nullptr);
            ShaderBuilder::requestShutdown();
            Engine::systemShutdown();
        }
    };

    struct EditorHostOverrideGuard {
        EditorHost* previousHost = nullptr;
        explicit EditorHostOverrideGuard(EditorHost* host)
            : previousHost(&getEditorHost()) {
            setEditorHost(host);
        }
        ~EditorHostOverrideGuard() {
            setEditorHost(previousHost);
        }
    };

    // ---------------------------------------------------------------------
    // Filesystem snapshot helpers for the generate / status reports.
    // ---------------------------------------------------------------------

    // generic_string -> last write time. Missing files and read errors are
    // silently skipped (callers treat absence as "not present").
    std::map<std::string, fs::file_time_type> snapshotTree(const fs::path& root) {
        std::map<std::string, fs::file_time_type> snapshot;
        std::error_code ec;
        if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
            return snapshot;
        }
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
        fs::recursive_directory_iterator end;
        for (; it != end; it.increment(ec)) {
            if (ec) break;
            std::error_code fec;
            if (!it->is_regular_file(fec) || fec) continue;
            // The stamp changes on every run; keeping it out of the snapshots stops
            // the report from listing it as a regenerated file.
            if (it->path().filename() == "generation.stamp") continue;
            std::error_code mec;
            const fs::file_time_type when = fs::last_write_time(it->path(), mec);
            if (mec) continue;
            snapshot[it->path().generic_string()] = when;
        }
        return snapshot;
    }

    fs::file_time_type mtimeOf(const fs::path& path, bool& ok) {
        std::error_code ec;
        fs::file_time_type when = fs::last_write_time(path, ec);
        ok = !ec;
        return when;
    }

    // Every path the Generator path writes into the project: the root CMakeLists,
    // AGENTS.md, the .vscode helper, and the whole .quantum internal tree
    // (scene_scripts.cpp, generated/, engine-api/).
    std::map<std::string, fs::file_time_type> snapshotGeneratedOutputs(const fs::path& projectDir) {
        std::map<std::string, fs::file_time_type> snapshot;
        for (const fs::path& file : {projectDir / "CMakeLists.txt",
                                     projectDir / "AGENTS.md",
                                     projectDir / ".vscode" / "settings.json"}) {
            std::error_code ec;
            if (fs::is_regular_file(file, ec)) {
                std::error_code mec;
                snapshot[file.generic_string()] = fs::last_write_time(file, mec);
            }
        }
        auto internal = snapshotTree(projectDir / INTERNAL_DIRNAME);
        snapshot.insert(internal.begin(), internal.end());
        return snapshot;
    }

    bool isSourceFile(const fs::path& path) {
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        static const std::set<std::string> exts = {
            ".scene", ".bundle", ".h", ".hpp", ".hh", ".hxx", ".inl",
            ".cpp", ".cc", ".cxx", ".c++", ".c"
        };
        return exts.count(ext) > 0;
    }

    // Newest mtime over the project's generator inputs: *.scene / *.bundle /
    // scripts anywhere outside .quantum / .vscode / build, plus project.yaml.
    fs::file_time_type newestSourceMtime(const fs::path& projectDir, bool& found) {
        found = false;
        fs::file_time_type newest{};

        auto consider = [&](const fs::path& file) {
            bool ok = false;
            const fs::file_time_type when = mtimeOf(file, ok);
            if (!ok) return;
            if (!found || when > newest) newest = when;
            found = true;
        };

        std::error_code ec;
        const fs::path projectFile = projectDir / PROJECT_FILE;
        if (fs::is_regular_file(projectFile, ec)) consider(projectFile);

        if (!fs::is_directory(projectDir, ec)) return newest;
        fs::recursive_directory_iterator it(projectDir, fs::directory_options::skip_permission_denied, ec);
        fs::recursive_directory_iterator end;
        for (; it != end; it.increment(ec)) {
            if (ec) break;
            const fs::path name = it->path().filename();
            const std::string stem = name.string();
            if (it->is_directory()) {
                if (stem == INTERNAL_DIRNAME || stem == ".vscode" || stem == BUILD_DIRNAME
                    || (!stem.empty() && stem[0] == '.')) {
                    it.disable_recursion_pending();
                }
                continue;
            }
            std::error_code fec;
            if (it->is_regular_file(fec) && !fec && isSourceFile(it->path())) {
                consider(it->path());
            }
        }
        return newest;
    }

    // Newest mtime over the generated outputs. found=false when none exist.
    fs::file_time_type newestGeneratedMtime(const fs::path& projectDir, bool& found) {
        found = false;
        fs::file_time_type newest{};
        std::error_code ec;
        for (const fs::path& file : {projectDir / "CMakeLists.txt",
                                     projectDir / INTERNAL_DIRNAME / "scene_scripts.cpp",
                                     projectDir / INTERNAL_DIRNAME / "generated" / "main.cpp"}) {
            if (!fs::is_regular_file(file, ec)) continue;
            bool ok = false;
            const fs::file_time_type when = mtimeOf(file, ok);
            if (!ok) continue;
            if (!found || when > newest) newest = when;
            found = true;
        }
        // Any per-scene factory under generated/ counts too.
        const fs::path generatedDir = projectDir / INTERNAL_DIRNAME / "generated";
        if (fs::is_directory(generatedDir, ec)) {
            fs::recursive_directory_iterator it(generatedDir, fs::directory_options::skip_permission_denied, ec);
            fs::recursive_directory_iterator end;
            for (; it != end; it.increment(ec)) {
                if (ec) break;
                std::error_code fec;
                if (!it->is_regular_file(fec) || fec) continue;
                bool ok = false;
                const fs::file_time_type when = mtimeOf(it->path(), ok);
                if (!ok) continue;
                if (!found || when > newest) newest = when;
                found = true;
            }
        }
        return newest;
    }

    bool generationMissing(const fs::path& projectDir) {
        std::error_code ec;
        return !fs::is_regular_file(projectDir / "CMakeLists.txt", ec)
            || !fs::is_regular_file(projectDir / INTERNAL_DIRNAME / "scene_scripts.cpp", ec)
            || !fs::is_regular_file(projectDir / INTERNAL_DIRNAME / "generated" / "main.cpp", ec);
    }

    // A generate run rewrites project.yaml *after* the sources, so comparing source
    // mtimes against output mtimes would call every successful run stale. The stamp
    // records when generation last finished and stands in for the outputs' time.
    fs::path generationStamp(const fs::path& projectDir) {
        return projectDir / INTERNAL_DIRNAME / "generation.stamp";
    }

    void writeGenerationStamp(const fs::path& projectDir) {
        const fs::path stamp = generationStamp(projectDir);
        std::error_code ec;
        if (!fs::is_directory(projectDir / INTERNAL_DIRNAME, ec)) return;
        std::ofstream out(stamp, std::ios::binary);
        if (!out) return;
        out << "quantum-build-generator\n";
        out.close();
        // Rewriting an existing file keeps its old mtime on some platforms.
        fs::last_write_time(stamp, fs::file_time_type::clock::now(), ec);
    }

    fs::path defaultBuildDir(const fs::path& projectDir) {
        return projectDir / INTERNAL_DIRNAME / BUILD_DIRNAME;
    }

    // CMake writes the cache before the generate step, so a configure that fails to
    // generate still leaves one behind; require a generated build system as well.
    bool isConfigured(const fs::path& buildDir) {
        std::error_code ec;
        if (!fs::is_regular_file(buildDir / "CMakeCache.txt", ec)) return false;
        if (fs::is_regular_file(buildDir / "cmake_install.cmake", ec)) return true;
        for (fs::directory_iterator it(buildDir, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::directory_iterator(); it.increment(ec)) {
            if (it->path().extension() == ".sln") return true;
        }
        return false;
    }

    // Reads CMAKE_GENERATOR from a configured build tree, "" when absent.
    std::string cachedGenerator(const fs::path& buildDir) {
        std::ifstream cache(buildDir / "CMakeCache.txt");
        if (!cache) return "";
        const std::string prefix = "CMAKE_GENERATOR:INTERNAL=";
        std::string line;
        while (std::getline(cache, line)) {
            if (line.rfind(prefix, 0) == 0) return line.substr(prefix.size());
        }
        return "";
    }

    // Parses make-style "[ 47%]" and ninja-style "[123/456]" build-line prefixes
    // into a 0..1 fraction (same rule the export build uses).
    bool parseBuildProgress(const std::string& line, float& fraction) {
        if (line.empty() || line[0] != '[') return false;
        size_t close = line.find(']');
        if (close == std::string::npos || close > 16) return false;
        std::string inner = line.substr(1, close - 1);
        try {
            size_t slash = inner.find('/');
            if (slash != std::string::npos) {
                int current = std::stoi(inner.substr(0, slash));
                int total = std::stoi(inner.substr(slash + 1));
                if (total <= 0 || current < 0 || current > total) return false;
                fraction = static_cast<float>(current) / static_cast<float>(total);
                return true;
            }
            size_t percent = inner.find('%');
            if (percent == std::string::npos) return false;
            int value = std::stoi(inner.substr(0, percent));
            if (value < 0 || value > 100) return false;
            fraction = value / 100.0f;
            return true;
        } catch (...) {
            return false;
        }
    }

    unsigned int resolveJobs(unsigned int requested) {
        unsigned int jobs = requested == 0 ? Generator::getAutomaticParallelBuildJobs() : requested;
        return std::min(jobs, Generator::getMaxParallelBuildJobs());
    }

    // ---------------------------------------------------------------------
    // Usage text.
    // ---------------------------------------------------------------------

    void printGenerateUsage(const std::string& cmd) {
        std::cout
            << "Usage:\n"
            << "  " << cmd << " generate --project <dir|project.yaml> [options]\n\n"
            << "Runs only the project-generation step: writes CMakeLists.txt and the\n"
            << ".quantum/generated sources (scene factories, scene_scripts.cpp, main.cpp)\n"
            << "into the project. Idempotent: unchanged files keep their bytes and mtime.\n\n"
            << "Options:\n"
            << "  -p, --project <path>        Project directory (or its project.yaml).\n"
            << "  -o, --out <dir>             Also copy the generated sources there.\n"
            << "      --kit <k=v ...>         cxx=, cc=, gen= (accepted for symmetry; the\n"
            << "                              generated CMakeLists is toolchain-agnostic).\n"
            << "      --verbose               Print per-file and progress detail.\n"
            << "  -h, --help                  Show this help.\n";
    }

    void printConfigureUsage(const std::string& cmd) {
        std::cout
            << "Usage:\n"
            << "  " << cmd << " configure --project <dir|project.yaml> [options]\n\n"
            << "Runs cmake configure on the project's generated CMakeLists.txt through\n"
            << "the same command runner and cmake path lookup the editor uses.\n\n"
            << "Options:\n"
            << "  -p, --project <path>        Project directory holding CMakeLists.txt.\n"
            << "  -b, --build-dir <dir>       Build tree. Default: <project>/.quantum/build.\n"
            << "      --config <type>         CMAKE_BUILD_TYPE. Default: Release.\n"
            << "      --kit <k=v ...>         cxx=<path> cc=<path> gen=\"<name>\".\n"
            << "      --jobs <n>              Parallel job cap passed to the generated build\n"
            << "                              (0 = automatic). Default: 0.\n"
            << "      --verbose               Stream cmake output to stdout.\n"
            << "  -h, --help                  Show this help.\n";
    }

    void printBuildUsage(const std::string& cmd) {
        std::cout
            << "Usage:\n"
            << "  " << cmd << " build --project <dir|project.yaml> [options]\n\n"
            << "Runs cmake --build on a configured build tree, streaming machine-parseable\n"
            << "progress lines to stdout:  progress <0..100> <stage> <detail>\n\n"
            << "Options:\n"
            << "  -p, --project <path>        Project directory (used to locate the build tree).\n"
            << "  -b, --build-dir <dir>       Build tree. Default: <project>/.quantum/build.\n"
            << "      --config <Debug|Release> Configuration. Default: Release.\n"
            << "      --target <name>         Build a single target.\n"
            << "  -j, --jobs <n>              --parallel jobs (0 = automatic). Default: 0.\n"
            << "      --verbose               Also echo raw build output lines.\n"
            << "  -h, --help                  Show this help.\n";
    }

    void printStatusUsage(const std::string& cmd) {
        std::cout
            << "Usage:\n"
            << "  " << cmd << " status --project <dir|project.yaml> [--build-dir <dir>] [--verbose]\n\n"
            << "Reports whether the generated sources are stale relative to the project's\n"
            << "*.scene / *.bundle / script / project.yaml inputs, and whether a build tree\n"
            << "is configured. Exit 0 when up to date, 1 when stale or not generated.\n";
    }

    void printUsage(const std::string& cmd) {
        std::cout
            << "Usage:\n"
            << "  " << cmd << " generate  [options]    Generate the project's CMake + sources (headless).\n"
            << "  " << cmd << " configure [options]    Run cmake configure.\n"
            << "  " << cmd << " build     [options]    Run cmake --build with progress output.\n"
            << "  " << cmd << " status    [options]    Report staleness and build-dir state.\n\n"
            << "Run a subcommand with --help for its options.\n";
    }

    // ---------------------------------------------------------------------
    // generate
    // ---------------------------------------------------------------------

    struct GenerateCliOptions {
        fs::path projectPath;
        fs::path outDir;
        Kit kit;
        bool verbose = false;
        bool help = false;
    };

    bool parseGenerateArgs(int argc, char** argv, GenerateCliOptions& options, std::string& error) {
        for (int i = 1; i < argc; i++) {
            const std::string arg = argv[i];
            std::string value;
            if (arg == "-h" || arg == "--help") {
                options.help = true;
            } else if (arg == "-p" || arg == "--project") {
                if (!requireValue(argc, argv, i, value, error)) return false;
                options.projectPath = value;
            } else if (arg == "-o" || arg == "--out") {
                if (!requireValue(argc, argv, i, value, error)) return false;
                options.outDir = value;
            } else if (arg == "--kit") {
                if (!requireValue(argc, argv, i, value, error)) return false;
                if (!parseKitValue(value, options.kit, error)) return false;
            } else if (arg == "--verbose") {
                options.verbose = true;
            } else {
                error = "Unknown argument: " + arg;
                return false;
            }
        }
        if (options.help) return true;
        if (options.projectPath.empty()) {
            error = "Missing required --project path.";
            return false;
        }
        return true;
    }

    // Reports what the generation wrote by comparing the before/after snapshots.
    void reportWrittenFiles(const std::map<std::string, fs::file_time_type>& before,
                            const std::map<std::string, fs::file_time_type>& after,
                            bool verbose) {
        size_t createdCount = 0, changedCount = 0;
        for (const auto& [path, when] : after) {
            auto it = before.find(path);
            if (it == before.end()) {
                createdCount++;
                std::cout << "generated (new)  " << path << "\n";
            } else if (it->second != when) {
                changedCount++;
                std::cout << "generated (update)  " << path << "\n";
            } else if (verbose) {
                std::cout << "unchanged  " << path << "\n";
            }
        }
        if (createdCount == 0 && changedCount == 0) {
            std::cout << "No changes: generated sources already up to date.\n";
        } else {
            std::cout << "Wrote " << createdCount << " new and " << changedCount
                      << " updated generated file(s).\n";
        }
    }

    bool copyGeneratedToOut(const fs::path& projectDir, const fs::path& outDir, std::string& error) {
        std::error_code ec;
        fs::create_directories(outDir, ec);
        if (ec) {
            error = "Failed to create --out directory: " + ec.message();
            return false;
        }

        for (const fs::path& file : {projectDir / "CMakeLists.txt", projectDir / "AGENTS.md"}) {
            if (fs::is_regular_file(file, ec)) {
                fs::copy_file(file, outDir / file.filename(),
                              fs::copy_options::overwrite_existing, ec);
                if (ec) {
                    error = "Failed to copy " + file.filename().string() + ": " + ec.message();
                    return false;
                }
            }
        }

        const fs::path internalSrc = projectDir / INTERNAL_DIRNAME;
        if (fs::is_directory(internalSrc, ec)) {
            fs::path internalDst = outDir / INTERNAL_DIRNAME;
            fs::remove_all(internalDst, ec);
            ec.clear();
            fs::copy(internalSrc, internalDst,
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            if (ec) {
                error = "Failed to copy generated sources to " + internalDst.string()
                        + ": " + ec.message();
                return false;
            }
        }
        return true;
    }

    int runGenerate(const std::string& commandName, const GenerateCliOptions& options,
                    int argc, char** argv) {
        const fs::path projectDir = normalizeProjectPath(options.projectPath);
        std::error_code ec;
        if (!fs::is_directory(projectDir, ec)) {
            std::cerr << commandName << " generate: not a project directory: "
                      << projectDir.string() << "\n";
            return 3;
        }

        // Snapshot before so the report reflects only this run's writes.
        const auto before = snapshotGeneratedOutputs(projectDir);

        // Same headless runtime the shaders/export subcommands build on: no window
        // is created and the backend never starts.
        EditorHostOverrideGuard hostOverride(nullptr);
        Out::setOutputWindow(nullptr);
        AppSettings::initialize();

        Project project;
        auto platform = std::make_unique<Platform>(&project);
        EngineRuntimeGuard engineRuntime;
        if (options.verbose) std::cerr << commandName << " generate: initializing headless engine\n";
        Engine::systemInit(argc, argv, platform.get());
        engineRuntime.initialized = true;
        Engine::pauseGameEvents(true);
        Engine::setAsyncLoading(true);

        ShaderPool::setShaderBuilder([projectPtr = &project](ShaderKey shaderKey) -> ShaderBuildResult {
            static ShaderBuilder builder;
            return builder.buildShader(shaderKey, projectPtr);
        });

        if (options.verbose) std::cerr << commandName << " generate: loading project\n";
        if (!project.loadProject(projectDir, false)) {
            std::cerr << commandName << " generate: failed to load project: "
                      << projectDir.string() << "\n";
            return 1;
        }
        if (options.verbose) {
            std::cerr << commandName << " generate: project loaded with "
                      << project.getScenes().size() << " scene(s)\n";
        }

        // Load every scene's data before saving: an unopened scene has no in-memory
        // entities for writeSceneToPath to turn into factories.
        {
            std::vector<fs::path> filepaths;
            for (const auto& sceneProject : project.getScenes()) {
                if (!sceneProject.filepath.empty() && sceneProject.scene == nullptr) {
                    filepaths.push_back(sceneProject.filepath);
                }
            }
            for (const fs::path& filepath : filepaths) {
                if (options.verbose) {
                    std::cerr << commandName << " generate: loading scene "
                              << filepath.generic_string() << "\n";
                }
                project.loadScene(filepath, false, false, true);
                if (options.verbose) {
                    std::cerr << commandName << " generate: loaded scene "
                              << filepath.generic_string() << "\n";
                }
            }
        }

        // Order the saves so the project's start scene is generated last: the final
        // configure() call marks the last-saved scene as the initial load in main.cpp.
        const uint32_t startSceneId = project.getStartSceneId();
        std::vector<uint32_t> saveOrder;
        std::map<uint32_t, fs::path> scenePaths;
        uint32_t deferredStart = NULL_PROJECT_SCENE;
        bool haveStart = false;
        for (const auto& sceneProject : project.getScenes()) {
            if (sceneProject.filepath.empty() || sceneProject.scene == nullptr) {
                continue;
            }
            scenePaths[sceneProject.id] = sceneProject.filepath;
            if (sceneProject.id == startSceneId) {
                deferredStart = sceneProject.id;
                haveStart = true;
                continue;
            }
            saveOrder.push_back(sceneProject.id);
        }
        if (haveStart) saveOrder.push_back(deferredStart);

        bool anySaved = false;
        for (uint32_t sceneId : saveOrder) {
            auto it = scenePaths.find(sceneId);
            if (it == scenePaths.end()) continue;
            if (options.verbose) {
                std::cout << "Generating from scene: " << it->second.generic_string() << "\n";
            }
            if (project.saveSceneToPath(sceneId, it->second)) {
                anySaved = true;
            } else {
                std::cerr << commandName << " generate: failed to generate scene: "
                          << it->second.generic_string() << "\n";
                return 1;
            }
        }

        if (!anySaved) {
            std::cerr << commandName << " generate: project has no scenes to generate.\n";
            return 1;
        }

        const auto after = snapshotGeneratedOutputs(projectDir);
        reportWrittenFiles(before, after, options.verbose);
        writeGenerationStamp(projectDir);

        if (!options.outDir.empty()) {
            std::string error;
            if (!copyGeneratedToOut(projectDir, options.outDir, error)) {
                std::cerr << commandName << " generate: " << error << "\n";
                return 1;
            }
            std::cout << "Copied generated sources to " << options.outDir.generic_string() << "\n";
        }

        return 0;
    }

    // ---------------------------------------------------------------------
    // configure
    // ---------------------------------------------------------------------

    struct ConfigureCliOptions {
        fs::path projectPath;
        fs::path buildDir;
        std::string configType = "Release";
        Kit kit;
        unsigned int jobs = 0;
        bool verbose = false;
        bool help = false;
    };

    bool parseConfigureArgs(int argc, char** argv, ConfigureCliOptions& options, std::string& error) {
        for (int i = 1; i < argc; i++) {
            const std::string arg = argv[i];
            std::string value;
            if (arg == "-h" || arg == "--help") {
                options.help = true;
            } else if (arg == "-p" || arg == "--project") {
                if (!requireValue(argc, argv, i, value, error)) return false;
                options.projectPath = value;
            } else if (arg == "-b" || arg == "--build-dir") {
                if (!requireValue(argc, argv, i, value, error)) return false;
                options.buildDir = value;
            } else if (arg == "--config") {
                if (!requireValue(argc, argv, i, value, error)) return false;
                options.configType = value;
            } else if (arg == "--kit") {
                if (!requireValue(argc, argv, i, value, error)) return false;
                if (!parseKitValue(value, options.kit, error)) return false;
            } else if (arg == "--jobs") {
                if (!requireValue(argc, argv, i, value, error)) return false;
                try { options.jobs = static_cast<unsigned int>(std::stoul(value)); }
                catch (...) { error = "Invalid --jobs value: " + value; return false; }
            } else if (arg == "--verbose") {
                options.verbose = true;
            } else {
                error = "Unknown argument: " + arg;
                return false;
            }
        }
        if (options.help) return true;
        if (options.projectPath.empty()) {
            error = "Missing required --project path.";
            return false;
        }
        return true;
    }

    int runConfigure(const std::string& commandName, const ConfigureCliOptions& options) {
        const fs::path projectDir = normalizeProjectPath(options.projectPath);
        std::error_code ec;
        if (!fs::is_directory(projectDir, ec)) {
            std::cerr << commandName << " configure: not a project directory: "
                      << projectDir.string() << "\n";
            return 3;
        }
        const fs::path cmakeLists = projectDir / "CMakeLists.txt";
        if (!fs::is_regular_file(cmakeLists, ec)) {
            std::cerr << commandName << " configure: no CMakeLists.txt in "
                      << projectDir.generic_string() << "; run '" << commandName
                      << " generate' first.\n";
            return 1;
        }

        fs::path buildDir = options.buildDir.empty() ? defaultBuildDir(projectDir) : options.buildDir;
        fs::create_directories(buildDir, ec);
        if (ec) {
            std::cerr << commandName << " configure: failed to create build directory: "
                      << ec.message() << "\n";
            return 1;
        }

        // The cmake executable honors the AppSettings override (Generator::cmakeExecutable).
        const std::string cmakeExe = Generator::cmakeExecutable();

        Kit kit = options.kit;
        applyKitDefaults(kit);

        std::string cmd = cmakeExe + " ";
        if (!kit.generator.empty()) {
            cmd += "-G \"" + kit.generator + "\" ";
        }
        if (!kit.cCompiler.empty()) {
            cmd += "-DCMAKE_C_COMPILER=\"" + toCMakePath(kit.cCompiler) + "\" ";
        }
        if (!kit.cxxCompiler.empty()) {
            cmd += "-DCMAKE_CXX_COMPILER=\"" + toCMakePath(kit.cxxCompiler) + "\" ";
        }
        cmd += "-DCMAKE_BUILD_TYPE=" + options.configType + " ";
        cmd += "-DQUANTUM_PARALLEL_BUILD_JOBS=" + std::to_string(resolveJobs(options.jobs)) + " ";
        // Deliberately no -DQUANTUM_LIB_DIR here: passing it would skip the
        // generated CMakeLists' own resolution (QUANTUM_LIB_DIR environment
        // variable, then the editor directory recorded at generate time) and,
        // with it, the QUANTUM_SHARED/QUANTUM_EDITOR macros that make the engine
        // import library link. Override the engine tree with that variable.
        cmd += "\"" + toCMakePath(projectDir.string()) + "\" ";
        cmd += "-B \"" + toCMakePath(buildDir.string()) + "\"";

        CommandRunner runner;
        cmd = CommandRunner::msvcEnvPrefix(kit.generator) + cmd;

        if (options.verbose) {
            std::cout << "cmd> " << cmd << "\n";
        } else {
            std::cout << "Configuring " << projectDir.generic_string()
                      << " -> " << buildDir.generic_string() << "\n";
        }

        bool ok = runner.run(cmd, projectDir, [&options](const std::string& line) {
            if (options.verbose) {
                std::cout << line << "\n";
            }
        });

        if (!ok) {
            std::cerr << commandName << " configure: cmake configure failed (exit != 0).\n";
            return 1;
        }
        std::cout << "Configured " << buildDir.generic_string() << "\n";
        return 0;
    }

    // ---------------------------------------------------------------------
    // build
    // ---------------------------------------------------------------------

    struct BuildCliOptions {
        fs::path projectPath;
        fs::path buildDir;
        std::string configType = "Release";
        std::string target;
        unsigned int jobs = 0;
        bool verbose = false;
        bool help = false;
    };

    bool parseBuildArgs(int argc, char** argv, BuildCliOptions& options, std::string& error) {
        for (int i = 1; i < argc; i++) {
            const std::string arg = argv[i];
            std::string value;
            if (arg == "-h" || arg == "--help") {
                options.help = true;
            } else if (arg == "-p" || arg == "--project") {
                if (!requireValue(argc, argv, i, value, error)) return false;
                options.projectPath = value;
            } else if (arg == "-b" || arg == "--build-dir") {
                if (!requireValue(argc, argv, i, value, error)) return false;
                options.buildDir = value;
            } else if (arg == "--config") {
                if (!requireValue(argc, argv, i, value, error)) return false;
                options.configType = value;
            } else if (arg == "--target") {
                if (!requireValue(argc, argv, i, value, error)) return false;
                options.target = value;
            } else if (arg == "-j" || arg == "--jobs") {
                if (!requireValue(argc, argv, i, value, error)) return false;
                try { options.jobs = static_cast<unsigned int>(std::stoul(value)); }
                catch (...) { error = "Invalid --jobs value: " + value; return false; }
            } else if (arg == "--verbose") {
                options.verbose = true;
            } else {
                error = "Unknown argument: " + arg;
                return false;
            }
        }
        if (options.help) return true;
        if (options.projectPath.empty()) {
            error = "Missing required --project path.";
            return false;
        }
        return true;
    }

    void emitProgress(const std::string& stage, float fraction, const std::string& detail) {
        if (fraction < 0.0f) fraction = 0.0f;
        if (fraction > 1.0f) fraction = 1.0f;
        const int percent = static_cast<int>(fraction * 100.0f + 0.5f);
        std::cout << "progress " << percent << " " << stage;
        if (!detail.empty()) std::cout << " " << detail;
        std::cout << std::endl;
    }

    int runBuild(const std::string& commandName, const BuildCliOptions& options) {
        const fs::path projectDir = normalizeProjectPath(options.projectPath);
        std::error_code ec;
        if (!fs::is_directory(projectDir, ec)) {
            std::cerr << commandName << " build: not a project directory: "
                      << projectDir.string() << "\n";
            return 3;
        }
        fs::path buildDir = options.buildDir.empty() ? defaultBuildDir(projectDir) : options.buildDir;
        if (!isConfigured(buildDir)) {
            std::cerr << commandName << " build: build tree is not configured: "
                      << buildDir.generic_string() << "; run '" << commandName
                      << " configure' first.\n";
            return 1;
        }

        const std::string cmakeExe = Generator::cmakeExecutable();
        // The build step invokes the compiler/linker, so it needs the same MSVC
        // environment (vcvars) as configure when the generator is Ninja.
        const std::string generator = cachedGenerator(buildDir);

        std::string cmd = cmakeExe + " --build \"" + buildDir.string() + "\""
            + " --config " + options.configType;
        if (!options.target.empty()) {
            cmd += " --target \"" + options.target + "\"";
        }
        cmd += " --parallel " + std::to_string(resolveJobs(options.jobs));
        cmd = CommandRunner::msvcEnvPrefix(generator) + cmd;

        emitProgress("start", 0.0f, "cmake --build " + buildDir.generic_string());

        // MSBuild has no percentage prefix; MsBuildProgress estimates it from the
        // source filenames it streams. On other platforms (or when it cannot) we
        // fall back to the make/ninja "[ n/m ]" prefix. No fake precision.
        MsBuildProgress msBuildProgress(buildDir, options.configType);
        float lastFraction = -1.0f;

        CommandRunner runner;
        bool ok = runner.run(cmd, buildDir, [&](const std::string& line) {
            if (options.verbose) {
                std::cout << "out> " << line << "\n";
            }
            float fraction = 0.0f;
            bool hasProgress = parseBuildProgress(line, fraction);
            if (!hasProgress && msBuildProgress.consumeLine(line, fraction)) {
                hasProgress = true;
            }
            if (hasProgress) {
                // Never step backwards within a single build pass.
                if (lastFraction < 0.0f || fraction >= lastFraction) {
                    lastFraction = fraction;
                } else {
                    fraction = lastFraction;
                }
                emitProgress("compile", fraction, trimLine(line));
            }
        });

        if (!ok) {
            emitProgress("failed", lastFraction < 0.0f ? 0.0f : lastFraction, "build failed");
            std::cerr << commandName << " build: cmake --build failed (exit != 0).\n";
            return 1;
        }

        emitProgress("done", 1.0f, "build succeeded");
        std::cout << "Built " << buildDir.generic_string() << " (" << options.configType
                  << (options.target.empty() ? "" : ", target " + options.target) << ")\n";
        return 0;
    }

    // ---------------------------------------------------------------------
    // status
    // ---------------------------------------------------------------------

    struct StatusCliOptions {
        fs::path projectPath;
        fs::path buildDir;
        bool verbose = false;
        bool help = false;
    };

    bool parseStatusArgs(int argc, char** argv, StatusCliOptions& options, std::string& error) {
        for (int i = 1; i < argc; i++) {
            const std::string arg = argv[i];
            std::string value;
            if (arg == "-h" || arg == "--help") {
                options.help = true;
            } else if (arg == "-p" || arg == "--project") {
                if (!requireValue(argc, argv, i, value, error)) return false;
                options.projectPath = value;
            } else if (arg == "-b" || arg == "--build-dir") {
                if (!requireValue(argc, argv, i, value, error)) return false;
                options.buildDir = value;
            } else if (arg == "--verbose") {
                options.verbose = true;
            } else {
                error = "Unknown argument: " + arg;
                return false;
            }
        }
        if (options.help) return true;
        if (options.projectPath.empty()) {
            error = "Missing required --project path.";
            return false;
        }
        return true;
    }

    long long mtimeSeconds(const fs::file_time_type& when) {
        // file_time_type's epoch is implementation-defined; report ticks since that
        // epoch, which is stable and comparable within one run (reporting only).
        const auto dur = std::chrono::duration_cast<std::chrono::seconds>(
            when.time_since_epoch());
        return dur.count();
    }

    int runStatus(const std::string& commandName, const StatusCliOptions& options) {
        const fs::path projectDir = normalizeProjectPath(options.projectPath);
        std::error_code ec;
        if (!fs::is_directory(projectDir, ec)) {
            std::cerr << commandName << " status: not a project directory: "
                      << projectDir.string() << "\n";
            return 3;
        }

        fs::path buildDir = options.buildDir.empty() ? defaultBuildDir(projectDir) : options.buildDir;

        const bool missing = generationMissing(projectDir);
        bool haveSource = false;
        bool haveOutput = false;
        const fs::file_time_type newestSource = newestSourceMtime(projectDir, haveSource);
        fs::file_time_type newestOutput = newestGeneratedMtime(projectDir, haveOutput);

        // A completed generate run is newer than the project.yaml it just wrote;
        // treat the stamp as the outputs' time so the run is not reported stale.
        bool haveStamp = false;
        const fs::file_time_type stamp = mtimeOf(generationStamp(projectDir), haveStamp);
        if (haveStamp && (!haveOutput || stamp > newestOutput)) {
            newestOutput = stamp;
            haveOutput = true;
        }

        const bool stale = missing
            || !haveOutput
            || (haveSource && newestSource > newestOutput);

        const bool configured = isConfigured(buildDir);

        std::cout << "PROJECT " << projectDir.generic_string() << "\n";
        std::cout << "generated " << (missing ? "no" : "yes") << "\n";
        std::cout << "stale " << (stale ? "yes" : "no") << "\n";
        std::cout << "build_dir " << buildDir.generic_string() << "\n";
        std::cout << "build_dir_configured " << (configured ? "yes" : "no") << "\n";

        if (options.verbose) {
            if (haveSource) std::cout << "newest_source_mtime " << mtimeSeconds(newestSource) << "\n";
            if (haveOutput) std::cout << "newest_generated_mtime " << mtimeSeconds(newestOutput) << "\n";
        }

        if (stale) {
            if (missing) {
                std::cout << "Reason: generated sources are missing; run '"
                          << commandName << " generate'.\n";
            } else {
                std::cout << "Reason: a project source is newer than the generated output; "
                             "run '" << commandName << " generate'.\n";
            }
            return 1;
        }
        std::cout << "Generated sources are up to date.\n";
        return 0;
    }

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public command entry points. Each never lets an exception reach main().
// ---------------------------------------------------------------------------

int runGenerateCommand(int argc, char** argv, const char* executableName) {
    attachHostConsoleIfNeeded();
    const std::string commandName = getCommandName(executableName);
    try {
        GenerateCliOptions options;
        std::string error;
        if (!parseGenerateArgs(argc, argv, options, error)) {
            std::cerr << commandName << " generate: " << error << "\n\n";
            printGenerateUsage(commandName);
            return 2;
        }
        if (options.help) {
            printGenerateUsage(commandName);
            return 0;
        }
        return runGenerate(commandName, options, argc, argv);
    } catch (const std::exception& ex) {
        std::cerr << commandName << " generate: unexpected error: " << ex.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << commandName << " generate: unexpected error.\n";
        return 1;
    }
}

int runConfigureCommand(int argc, char** argv, const char* executableName) {
    attachHostConsoleIfNeeded();
    const std::string commandName = getCommandName(executableName);
    try {
        ConfigureCliOptions options;
        std::string error;
        if (!parseConfigureArgs(argc, argv, options, error)) {
            std::cerr << commandName << " configure: " << error << "\n\n";
            printConfigureUsage(commandName);
            return 2;
        }
        if (options.help) {
            printConfigureUsage(commandName);
            return 0;
        }
        return runConfigure(commandName, options);
    } catch (const std::exception& ex) {
        std::cerr << commandName << " configure: unexpected error: " << ex.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << commandName << " configure: unexpected error.\n";
        return 1;
    }
}

int runBuildCommand(int argc, char** argv, const char* executableName) {
    attachHostConsoleIfNeeded();
    const std::string commandName = getCommandName(executableName);
    try {
        BuildCliOptions options;
        std::string error;
        if (!parseBuildArgs(argc, argv, options, error)) {
            std::cerr << commandName << " build: " << error << "\n\n";
            printBuildUsage(commandName);
            return 2;
        }
        if (options.help) {
            printBuildUsage(commandName);
            return 0;
        }
        return runBuild(commandName, options);
    } catch (const std::exception& ex) {
        std::cerr << commandName << " build: unexpected error: " << ex.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << commandName << " build: unexpected error.\n";
        return 1;
    }
}

int runStatusCommand(int argc, char** argv, const char* executableName) {
    attachHostConsoleIfNeeded();
    const std::string commandName = getCommandName(executableName);
    try {
        StatusCliOptions options;
        std::string error;
        if (!parseStatusArgs(argc, argv, options, error)) {
            std::cerr << commandName << " status: " << error << "\n\n";
            printStatusUsage(commandName);
            return 2;
        }
        if (options.help) {
            printStatusUsage(commandName);
            return 0;
        }
        return runStatus(commandName, options);
    } catch (const std::exception& ex) {
        std::cerr << commandName << " status: unexpected error: " << ex.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << commandName << " status: unexpected error.\n";
        return 1;
    }
}

int runBuildGeneratorHelpCommand(int argc, char** argv, const char* executableName) {
    attachHostConsoleIfNeeded();
    const std::string commandName = getCommandName(executableName);
    if (argc < 1) {
        std::cerr << commandName << ": no command given\n\n";
        printUsage(commandName);
        return 2;
    }

    const std::string argument = argv[0];
    if (argument == "-h" || argument == "--help" || argument == "help") {
        printUsage(commandName);
        return 0;
    }

    std::cerr << commandName << ": unknown argument '" << argument << "'\n\n";
    printUsage(commandName);
    return 2;
}

}
