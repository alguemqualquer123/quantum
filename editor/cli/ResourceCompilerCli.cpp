// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#include "ResourceCompilerCli.h"

#include "cli/CommandLine.h"

#include "AppSettings.h"
#include "Doriax.h"
#include "EditorHost.h"
#include "Out.h"
#include "Platform.h"
#include "Project.h"
#include "pool/ShaderPool.h"
#include "shader/ShaderBuilder.h"
#include "util/SHA1.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <vector>

#if defined(_WIN32)
    #define NOMINMAX
    #include <windows.h>
    #include <cstdio>
#endif

namespace doriax::editor::cli {

using ::doriax::Engine;
using ::doriax::ShaderBuildResult;
using ::doriax::ShaderKey;
using ::doriax::ShaderPool;

namespace {
    constexpr char RESOURCE_PACK_MAGIC[] = {'D', 'X', 'P', 'K', '1'};
    // The reader seeks with off_t, still 32-bit on the armeabi-v7a and x86 ABIs
    constexpr uint64_t MAX_RESOURCE_PACK_SIZE = INT32_MAX;

    struct ResourcePackBuildEntry {
        std::string path;      // key inside the pack, relative to the export root
        fs::path source;
        uint64_t size = 0;
        uint64_t offset = 0;
        uint8_t key = 0;
        uint32_t shift = 0;
    };

    void writeU8(std::ofstream& out, uint8_t value) {
        out.put(static_cast<char>(value));
    }

    void writeU16(std::ofstream& out, uint16_t value) {
        out.put(static_cast<char>(value & 0xff));
        out.put(static_cast<char>((value >> 8) & 0xff));
    }

    void writeU32(std::ofstream& out, uint32_t value) {
        for (int i = 0; i < 4; i++) {
            out.put(static_cast<char>((value >> (i * 8)) & 0xff));
        }
    }

    void writeU64(std::ofstream& out, uint64_t value) {
        for (int i = 0; i < 8; i++) {
            out.put(static_cast<char>((value >> (i * 8)) & 0xff));
        }
    }

    // Undone by ResourcePack::deobfuscate. Not encryption: the key is in the header.
    void obfuscate(std::vector<unsigned char>& data, uint8_t key, uint32_t shift) {
        if (data.empty()) return;

        shift %= data.size();
        if (shift > 0) {
            std::rotate(data.begin(), data.end() - shift, data.end());
        }

        for (unsigned char& byte : data) {
            byte ^= key;
        }
    }

    // From what the header already carries, so both passes agree without the bytes
    void deriveObfuscation(ResourcePackBuildEntry& entry) {
        const size_t hash = std::hash<std::string>{}(entry.path) ^ (static_cast<size_t>(entry.size) << 1);
        entry.key = static_cast<uint8_t>((hash % 255) + 1);
        entry.shift = entry.size == 0 ? 0 : static_cast<uint32_t>(hash % entry.size);
    }

    bool readWholeFile(const fs::path& path, std::vector<unsigned char>& data) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        in.seekg(0, std::ios::end);
        std::streamoff size = in.tellg();
        if (size < 0) return false;
        in.seekg(0, std::ios::beg);
        data.resize(static_cast<size_t>(size));
        return data.empty() || static_cast<bool>(in.read(reinterpret_cast<char*>(data.data()), size));
    }

    // Mirrors engine ResourcePack::deobfuscate, the inverse of the obfuscate above.
    void deobfuscate(std::vector<unsigned char>& data, uint8_t key, uint32_t shift) {
        if (data.empty()) return;

        for (unsigned char& byte : data) {
            byte ^= key;
        }

        shift %= data.size();
        if (shift > 0) {
            std::rotate(data.begin(), data.begin() + shift, data.end());
        }
    }

    // A GUI-subsystem binary has no console attached; reclaim the parent's so the
    // CLI output reaches the terminal (same as the export/shaders subcommands).
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
            return "doriax-resource-compiler";
        }

        const fs::path executablePath(executableName);
        const std::string filename = executablePath.filename().string();
        return filename.empty() ? std::string("doriax-resource-compiler") : filename;
    }

    // Same shape as CommandLine's normalizeProjectPath.
    fs::path normalizeProjectPath(const fs::path& path) {
        if (path.filename() == "project.yaml") {
            return path.parent_path();
        }
        return path;
    }

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
}

bool buildResourcePack(const PakOptions& options, bool& created, std::string& error) {
    if (options.onProgress) {
        options.onProgress("Packing resources...", 0.95f);
    }

    created = false;
    std::vector<ResourcePackBuildEntry> entries;
    const fs::path projectRoot = options.projectDir;
    const fs::path outputPath = options.outputPack;

    // Written aside and renamed, so a half-written pack is never left for the cleanup
    const fs::path tempPath = outputPath.parent_path() / RESOURCE_PACK_TEMP_FILENAME;

    auto checkCancelled = [&]() {
        return options.isCancelled && options.isCancelled();
    };

    // Sizing pass: paths and sizes settle the header, so no bytes are read yet
    std::error_code ec;
    for (const char* rootName : {"assets", "lua"}) {
        const fs::path root = projectRoot / rootName;

        fs::recursive_directory_iterator files(root, fs::directory_options::skip_permission_denied, ec);
        if (ec == std::errc::no_such_file_or_directory) continue;
        if (ec) {
            error = std::string("Failed to read exported ") + rootName + " directory: " + ec.message();
            return false;
        }

        for (fs::recursive_directory_iterator end; files != end; files.increment(ec)) {
            if (checkCancelled()) {
                error = "Export cancelled";
                return false;
            }

            const fs::path filePath = files->path();
            const bool regular = files->is_regular_file(ec);
            if (ec) {
                error = "Failed to read resource for pack: " + filePath.string() + ": " + ec.message();
                return false;
            }
            if (!regular) continue;
            // A pack left by an earlier export into the same directory is replaced, not packed
            if (filePath == outputPath || filePath == tempPath) continue;

            fs::path relPath = fs::relative(filePath, projectRoot, ec);
            if (ec || relPath.empty()) {
                error = "Failed to resolve resource path for pack: " + filePath.string();
                return false;
            }

            ResourcePackBuildEntry entry;
            entry.path = relPath.generic_string();
            if (entry.path.size() > UINT16_MAX) {
                error = "Resource path is too long for pack: " + entry.path;
                return false;
            }

            const uintmax_t fileSize = fs::file_size(filePath, ec);
            if (ec) {
                error = "Failed to read resource size for pack: " + filePath.string() + ": " + ec.message();
                return false;
            }
            // Data::open() reports lengths as unsigned int
            if (fileSize > UINT32_MAX) {
                error = "Resource is too large to pack: " + filePath.string();
                return false;
            }

            entry.source = filePath;
            entry.size = static_cast<uint64_t>(fileSize);
            deriveObfuscation(entry);

            entries.push_back(std::move(entry));
        }
        // increment() lands on end when it fails, so the loop condition exits first
        if (ec) {
            error = std::string("Failed to walk exported ") + rootName + " directory: " + ec.message();
            return false;
        }
    }

    if (entries.empty()) {
        return removeResourcePack(outputPath, error);
    }

    std::sort(entries.begin(), entries.end(), [](const ResourcePackBuildEntry& a, const ResourcePackBuildEntry& b) {
        return a.path < b.path;
    });

    // magic + count, then per entry: u16 path length, path, u64 offset, u64 size, u8 key, u32 shift
    uint64_t offset = sizeof(RESOURCE_PACK_MAGIC) + 4;
    for (const auto& entry : entries) {
        offset += 2 + entry.path.size() + 8 + 8 + 1 + 4;
    }
    for (auto& entry : entries) {
        entry.offset = offset;
        offset += entry.size;
    }

    if (offset > MAX_RESOURCE_PACK_SIZE) {
        error = "Packed resources exceed the 2 GiB limit. Disable Native Resource Pack in Project Settings.";
        return false;
    }

    fs::create_directories(outputPath.parent_path(), ec);
    if (ec) {
        error = "Failed to create resource pack directory: " + ec.message();
        return false;
    }

    std::ofstream out(tempPath, std::ios::binary);
    if (!out) {
        error = "Failed to create resource pack: " + tempPath.string();
        return false;
    }

    out.write(RESOURCE_PACK_MAGIC, sizeof(RESOURCE_PACK_MAGIC));
    writeU32(out, static_cast<uint32_t>(entries.size()));

    for (const auto& entry : entries) {
        writeU16(out, static_cast<uint16_t>(entry.path.size()));
        out.write(entry.path.data(), static_cast<std::streamsize>(entry.path.size()));
        writeU64(out, entry.offset);
        writeU64(out, entry.size);
        writeU8(out, entry.key);
        writeU32(out, entry.shift);
    }

    // Write pass: one resource in memory at a time, so packing does not scale with the project
    std::vector<unsigned char> data;
    bool bodiesWritten = true;
    for (const auto& entry : entries) {
        if (checkCancelled()) {
            error = "Export cancelled";
            bodiesWritten = false;
            break;
        }
        if (!readWholeFile(entry.source, data)) {
            error = "Failed to read resource for pack: " + entry.source.string();
            bodiesWritten = false;
            break;
        }
        if (data.size() != entry.size) {
            error = "Resource changed while packing: " + entry.source.string();
            bodiesWritten = false;
            break;
        }

        obfuscate(data, entry.key, entry.shift);
        if (!data.empty()) {
            out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        }
    }

    // A flush failure only surfaces on close, and Windows will not remove an open file
    out.close();
    if (bodiesWritten && !out) {
        error = "Failed to write resource pack: " + tempPath.string();
        bodiesWritten = false;
    }
    if (!bodiesWritten) {
        fs::remove(tempPath, ec);
        return false;
    }

    if (checkCancelled()) {
        fs::remove(tempPath, ec);
        error = "Export cancelled";
        return false;
    }

    fs::rename(tempPath, outputPath, ec);
    if (ec) {
        error = "Failed to replace resource pack: " + ec.message();
        fs::remove(tempPath, ec);
        return false;
    }

    created = true;
    return true;
}

bool removeResourcePack(const fs::path& packPath, std::string& error) {
    std::error_code ec;
    for (const fs::path& path : {packPath, packPath.parent_path() / RESOURCE_PACK_TEMP_FILENAME}) {
        fs::remove(path, ec);
        if (ec) {
            error = "Failed to remove " + path.string() + ": " + ec.message();
            return false;
        }
    }

    return true;
}

bool collectPackedTreeEntries(const fs::path& dir, std::vector<fs::path>& out,
                              std::string& error, const fs::path& keep) {
    std::error_code ec;
    fs::directory_iterator entries(dir, ec);
    if (ec == std::errc::no_such_file_or_directory) return true;

    for (fs::directory_iterator end; !ec && entries != end; entries.increment(ec)) {
        if (!keep.empty() && entries->path() == keep) continue;

        out.push_back(entries->path());
    }
    if (ec) {
        error = "Failed to read exported directory " + dir.string() + ": " + ec.message();
        return false;
    }

    return true;
}

bool isReservedPackName(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return name == RESOURCE_PACK_FILENAME || name == RESOURCE_PACK_TEMP_FILENAME;
}

namespace {
    void printPakUsage(const std::string& commandName) {
        std::cout
            << "Usage:\n"
            << "  " << commandName << " pak --project <project-dir|project.yaml> [options]\n\n"
            << "Options:\n"
            << "  -p, --project <path>        Project directory holding assets/ and lua/.\n"
            << "  -o, --out <path>            Pack file. Default: <project>/assets/resources.pak.\n"
            << "      --verbose               Print progress and per-step detail.\n"
            << "  -h, --help                  Show this help.\n";
    }

    void printShadersUsage(const std::string& commandName) {
        std::cout
            << "Usage:\n"
            << "  " << commandName << " shaders --project <project-dir|project.yaml> [options]\n"
            << "  " << commandName << " shaders --out <dir> --shader <spec> [options]\n\n"
            << "Project options:\n"
            << "  -p, --project <path>        Compile the project's scene shaders into its\n"
            << "                              on-disk shader cache.\n"
            << "      --force                 Drop the cached builds of forked custom shaders\n"
            << "                              first, so they recompile.\n"
            << "      --verbose               Print per-shader progress.\n\n"
            << "The second form is the standalone generator; run it with --help for options.\n";
    }

    void printInspectUsage(const std::string& commandName) {
        std::cout
            << "Usage:\n"
            << "  " << commandName << " inspect --pack <file.pak> [--verbose]\n\n"
            << "Options:\n"
            << "  --pack <path>               resources.pak file to list.\n"
            << "      --verbose               Also print offset, key and shift per entry.\n"
            << "  -h, --help                  Show this help.\n";
    }

    void printUsage(const std::string& commandName) {
        std::cout
            << "Usage:\n"
            << "  " << commandName << " pak [options]        Pack a project's assets into resources.pak.\n"
            << "  " << commandName << " shaders [options]    Compile project shaders (headless).\n"
            << "  " << commandName << " inspect [options]    List the entries of a resources.pak.\n"
            << "  " << commandName << " export [options]     Export a project.\n\n"
            << "Run a subcommand with --help for its options.\n";
    }

    struct PakCliOptions {
        fs::path projectPath;
        fs::path outputPack;
        bool verbose = false;
        bool help = false;
    };

    bool requireValue(int argc, char** argv, int& index, std::string& value, std::string& error) {
        if (index + 1 >= argc) {
            error = std::string("Missing value for ") + argv[index];
            return false;
        }
        value = argv[++index];
        return true;
    }

    bool parsePakArgs(int argc, char** argv, PakCliOptions& options, std::string& error) {
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
                options.outputPack = value;
            } else if (arg == "--verbose") {
                options.verbose = true;
            } else {
                error = "Unknown argument: " + arg;
                return false;
            }
        }

        if (options.help) {
            return true;
        }
        if (options.projectPath.empty()) {
            error = "Missing required --project path.";
            return false;
        }
        return true;
    }

    int runPak(const std::string& commandName, const PakCliOptions& options) {
        const fs::path projectDir = normalizeProjectPath(options.projectPath);
        std::error_code ec;
        if (!fs::is_directory(projectDir, ec)) {
            std::cerr << commandName << " pak: not a project directory: " << projectDir.string() << "\n";
            return 1;
        }

        const fs::path outputPack = options.outputPack.empty()
            ? projectDir / "assets" / RESOURCE_PACK_FILENAME
            : options.outputPack;

        PakOptions packOptions;
        packOptions.projectDir = projectDir;
        packOptions.outputPack = outputPack;
        if (options.verbose) {
            packOptions.onProgress = [](const std::string& step, float value) {
                std::cout << step << " (" << static_cast<int>(value * 100.0f) << "%)\n";
            };
        }

        bool created = false;
        std::string error;
        if (!buildResourcePack(packOptions, created, error)) {
            std::cerr << commandName << " pak: " << error << "\n";
            return 1;
        }

        if (created) {
            const uintmax_t packSize = fs::file_size(outputPack, ec);
            std::cout << "Wrote " << outputPack.generic_string();
            if (!ec) {
                std::cout << " (" << packSize << " bytes)";
            }
            std::cout << "\n";
        } else {
            std::cout << "No assets/lua resources found; " << outputPack.generic_string()
                      << " was not created (stale pack removed).\n";
        }
        return 0;
    }

    struct ShadersCliOptions {
        fs::path projectPath;
        bool force = false;
        bool verbose = false;
        bool help = false;
    };

    bool parseProjectShadersArgs(int argc, char** argv, ShadersCliOptions& options, std::string& error) {
        for (int i = 1; i < argc; i++) {
            const std::string arg = argv[i];
            std::string value;

            if (arg == "-h" || arg == "--help") {
                options.help = true;
            } else if (arg == "-p" || arg == "--project") {
                if (!requireValue(argc, argv, i, value, error)) return false;
                options.projectPath = value;
            } else if (arg == "--force") {
                options.force = true;
            } else if (arg == "--verbose") {
                options.verbose = true;
            } else {
                error = "Unknown argument: " + arg;
                return false;
            }
        }

        if (options.help || !options.projectPath.empty()) {
            return true;
        }
        if (options.force || options.verbose) {
            error = "Missing required --project path.";
            return false;
        }
        return true;
    }

    int runProjectShaders(const std::string& commandName, const ShadersCliOptions& options,
                          int argc, char** argv) {
        // Same headless runtime the export subcommand builds on: no window is created.
        EditorHostOverrideGuard hostOverride(nullptr);

        Out::setOutputWindow(nullptr);
        AppSettings::initialize();

        Project project;
        auto platform = std::make_unique<Platform>(&project);
        EngineRuntimeGuard engineRuntime;
        Engine::systemInit(argc, argv, platform.get());
        engineRuntime.initialized = true;
        Engine::pauseGameEvents(true);
        Engine::setAsyncLoading(true);

        ShaderPool::setShaderBuilder([projectPtr = &project](ShaderKey shaderKey) -> ShaderBuildResult {
            static ShaderBuilder builder;
            return builder.buildShader(shaderKey, projectPtr);
        });

        const fs::path projectPath = normalizeProjectPath(options.projectPath);
        if (!project.loadProject(projectPath, false)) {
            return 1;
        }

        if (options.force) {
            // Drop the disk cache of forked variants so the next build recompiles them.
            ShaderBuilder::invalidateCustomShaders();
        }

        if (options.verbose) {
            std::cout << "Compiling project shaders for " << projectPath.generic_string() << "...\n";
        }

        // Same key set Project::buildStandaloneShaderCache() uses (that entry point is
        // private; stored scene keys are what the scenes shipped with on disk).
        std::set<ShaderKey> shaderKeys;
        for (const auto& sceneProject : project.getScenes()) {
            shaderKeys.insert(sceneProject.shaderKeys.begin(), sceneProject.shaderKeys.end());
        }

        ShaderBuilder builder;
        builder.buildMissingShaders(shaderKeys, &project);

        if (options.verbose) {
            std::cout << "Shader cache is up to date.\n";
        }
        return 0;
    }
}

int runPakCommand(int argc, char** argv, const char* executableName) {
    attachHostConsoleIfNeeded();

    const std::string commandName = getCommandName(executableName);

    PakCliOptions options;
    std::string error;
    if (!parsePakArgs(argc, argv, options, error)) {
        std::cerr << commandName << " pak: " << error << "\n\n";
        printPakUsage(commandName);
        return 2;
    }

    if (options.help) {
        printPakUsage(commandName);
        return 0;
    }

    return runPak(commandName, options);
}

int runShadersCommand(int argc, char** argv, const char* executableName) {
    attachHostConsoleIfNeeded();

    const std::string commandName = getCommandName(executableName);

    const bool hasProject = std::any_of(argv + 1, argv + argc,
        [](const char* arg) { return std::strcmp(arg, "-p") == 0 || std::strcmp(arg, "--project") == 0; });
    if (!hasProject) {
        // Standalone generator form (--out/--shader/...): the established path handles it.
        return CommandLine::runShadersCommand(argc, argv, executableName);
    }

    ShadersCliOptions options;
    std::string error;
    if (!parseProjectShadersArgs(argc, argv, options, error)) {
        std::cerr << commandName << " shaders: " << error << "\n\n";
        printShadersUsage(commandName);
        return 2;
    }

    if (options.help) {
        printShadersUsage(commandName);
        return 0;
    }

    return runProjectShaders(commandName, options, argc, argv);
}

int runInspectCommand(int argc, char** argv, const char* executableName) {
    attachHostConsoleIfNeeded();

    const std::string commandName = getCommandName(executableName);

    fs::path packPath;
    bool verbose = false;
    std::string error;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        std::string value;

        if (arg == "-h" || arg == "--help") {
            printInspectUsage(commandName);
            return 0;
        } else if (arg == "--pack") {
            if (!requireValue(argc, argv, i, value, error)) {
                std::cerr << commandName << " inspect: " << error << "\n";
                return 2;
            }
            packPath = value;
        } else if (arg == "--verbose") {
            verbose = true;
        } else {
            std::cerr << commandName << " inspect: Unknown argument: " << arg << "\n\n";
            printInspectUsage(commandName);
            return 2;
        }
    }

    if (packPath.empty()) {
        std::cerr << commandName << " inspect: Missing required --pack path.\n\n";
        printInspectUsage(commandName);
        return 2;
    }

    std::ifstream in(packPath, std::ios::binary);
    if (!in) {
        std::cerr << commandName << " inspect: Failed to open pack: " << packPath.string() << "\n";
        return 1;
    }

    in.seekg(0, std::ios::end);
    const uint64_t packSize = static_cast<uint64_t>(in.tellg());
    in.seekg(0, std::ios::beg);

    auto readU8Value = [&in]() -> uint8_t {
        unsigned char byte = 0;
        in.read(reinterpret_cast<char*>(&byte), 1);
        return byte;
    };
    auto readLE = [&in](int bytes) -> uint64_t {
        uint64_t value = 0;
        for (int i = 0; i < bytes; i++) {
            unsigned char byte = 0;
            if (!in.read(reinterpret_cast<char*>(&byte), 1)) {
                in.setstate(std::ios::failbit);
                return 0;
            }
            value |= static_cast<uint64_t>(byte) << (i * 8);
        }
        return value;
    };

    char magic[sizeof(RESOURCE_PACK_MAGIC)] = {};
    if (!in.read(magic, sizeof(magic))
        || std::memcmp(magic, RESOURCE_PACK_MAGIC, sizeof(RESOURCE_PACK_MAGIC)) != 0) {
        std::cerr << commandName << " inspect: Not a doriax resource pack (bad magic): "
                  << packPath.string() << "\n";
        return 1;
    }

    const uint64_t fileCount = readLE(4);
    if (!in) {
        std::cerr << commandName << " inspect: Truncated pack header.\n";
        return 1;
    }

    struct InspectEntry {
        std::string path;
        uint64_t offset = 0;
        uint64_t size = 0;
        uint8_t key = 0;
        uint32_t shift = 0;
    };
    std::vector<InspectEntry> entries;
    entries.reserve(static_cast<size_t>(fileCount));

    for (uint64_t i = 0; i < fileCount; i++) {
        InspectEntry entry;
        const uint64_t pathLength = readLE(2);
        if (!in || pathLength == 0) {
            std::cerr << commandName << " inspect: Corrupt entry header at index " << i << ".\n";
            return 1;
        }
        entry.path.resize(static_cast<size_t>(pathLength));
        if (!in.read(entry.path.data(), static_cast<std::streamsize>(pathLength))) {
            std::cerr << commandName << " inspect: Truncated entry path at index " << i << ".\n";
            return 1;
        }
        entry.offset = readLE(8);
        entry.size = readLE(8);
        entry.key = readU8Value();
        entry.shift = static_cast<uint32_t>(readLE(4));
        if (!in) {
            std::cerr << commandName << " inspect: Truncated entry header at index " << i << ".\n";
            return 1;
        }
        // Same bounds check the engine reader applies, so a corrupt pack cannot
        // send the hashing pass out of the file.
        if (entry.offset > packSize || entry.size > packSize - entry.offset) {
            std::cerr << commandName << " inspect: Entry out of bounds at index " << i << ": "
                      << entry.path << "\n";
            return 1;
        }
        entries.push_back(std::move(entry));
    }

    uint64_t totalSize = 0;
    std::cout << "PACK " << packPath.generic_string() << " (" << packSize << " bytes, "
              << entries.size() << " entries)\n";
    for (const InspectEntry& entry : entries) {
        std::vector<unsigned char> data(static_cast<size_t>(entry.size));
        if (!data.empty()) {
            in.seekg(static_cast<std::streamoff>(entry.offset));
            if (!in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()))) {
                std::cerr << commandName << " inspect: Failed to read entry body: " << entry.path << "\n";
                return 1;
            }
            deobfuscate(data, entry.key, entry.shift);
        }
        const std::string sha1 = SHA1::hash(std::string(data.begin(), data.end()));
        totalSize += entry.size;

        std::cout << "  " << entry.path << "\n";
        std::cout << "      size  " << entry.size << " bytes\n";
        std::cout << "      sha1  " << sha1 << "\n";
        if (verbose) {
            std::cout << "      offset " << entry.offset
                      << "  key 0x" << std::hex << static_cast<int>(entry.key) << std::dec
                      << "  shift " << entry.shift << "\n";
        }
    }
    std::cout << "TOTAL " << totalSize << " bytes packed, " << entries.size() << " entries\n";

    return 0;
}

int runResourceCompilerHelpCommand(int argc, char** argv, const char* executableName) {
    attachHostConsoleIfNeeded();

    const std::string commandName = getCommandName(executableName);
    if (argc < 1) {
        // The CLI twin never opens the editor, so bare invocations just explain usage.
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
