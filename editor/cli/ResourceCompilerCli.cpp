// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "ResourceCompilerCli.h"

#include "cli/CommandLine.h"
#include "json.hpp"

#include "AppSettings.h"
#include "Quantum.h"
#include "EditorHost.h"
#include "Out.h"
#include "Platform.h"
#include "Project.h"
#include "graph/GraphCompiler.h"
#include "graph/GraphDump.h"
#include "graph/GraphReader.h"
#include "graph/GraphSchema.h"
#include "graph/GraphSerializerYaml.h"
#include "graph/ExecutableGraph.h"
#include "graph/NodeCatalog.h"
#include "io/ResourcePack.h"
#include "pool/ShaderPool.h"
#include "shader/ShaderBuilder.h"
#include "util/Checksum.h"
#include "util/SHA1.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <map>
#include <set>
#include <string>
#include <vector>

#if defined(_WIN32)
    #define NOMINMAX
    #include <windows.h>
    #include <cstdio>
    #include <cstdlib>
#endif

namespace quantum::editor::cli {

using ::quantum::Engine;
using ::quantum::ShaderBuildResult;
using ::quantum::ShaderKey;
using ::quantum::ShaderPool;

namespace {
    constexpr char RESOURCE_PACK_MAGIC_V1[] = {'D', 'X', 'P', 'K', '1'};
    constexpr char RESOURCE_PACK_MAGIC[] = {'D', 'X', 'P', 'K', '2'};
    // The reader seeks with off_t, still 32-bit on the armeabi-v7a and x86 ABIs
    constexpr uint64_t MAX_RESOURCE_PACK_SIZE = INT32_MAX;

    struct ResourcePackBuildEntry {
        std::string path;      // key inside the pack, relative to the export root
        fs::path source;
        uint64_t size = 0;
        uint64_t offset = 0;
        uint8_t key = 0;
        uint32_t shift = 0;
        std::array<unsigned char, 32> digest{};
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

    // Stable FNV-1a over the entry identity. `std::hash` is deliberately not used:
    // its result is implementation-defined and made identical projects produce
    // different obfuscation metadata on different standard libraries.
    uint64_t deterministicEntryHash(const ResourcePackBuildEntry& entry) {
        uint64_t hash = 1469598103934665603ULL;
        auto add = [&hash](unsigned char byte) {
            hash ^= byte;
            hash *= 1099511628211ULL;
        };
        for (unsigned char byte : entry.path) add(byte);
        for (int i = 0; i < 8; ++i) add(static_cast<unsigned char>((entry.size >> (i * 8)) & 0xff));
        return hash;
    }

    // From what the header already carries, so both passes agree without the bytes
    void deriveObfuscation(ResourcePackBuildEntry& entry) {
        const uint64_t hash = deterministicEntryHash(entry);
        entry.key = static_cast<uint8_t>((hash % 255ULL) + 1ULL);
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

    bool fillDigest(ResourcePackBuildEntry& entry, std::string& error) {
        std::string hashError;
        const std::string hex = sha256HexOfFile(entry.source.string(), hashError);
        if (hex.size() != entry.digest.size() * 2) {
            error = hashError.empty() ? "failed to calculate resource digest: " + entry.source.string() : hashError;
            return false;
        }
        auto nibble = [](char value) -> int {
            if (value >= '0' && value <= '9') return value - '0';
            if (value >= 'a' && value <= 'f') return value - 'a' + 10;
            if (value >= 'A' && value <= 'F') return value - 'A' + 10;
            return -1;
        };
        for (size_t i = 0; i < entry.digest.size(); i++) {
            const int high = nibble(hex[i * 2]);
            const int low = nibble(hex[i * 2 + 1]);
            if (high < 0 || low < 0) {
                error = "invalid resource digest for: " + entry.source.string();
                return false;
            }
            entry.digest[i] = static_cast<unsigned char>((high << 4) | low);
        }
        return true;
    }

    std::string digestToHex(const std::array<unsigned char, 32>& digest) {
        static const char* digits = "0123456789abcdef";
        std::string result;
        result.reserve(digest.size() * 2);
        for (unsigned char byte : digest) {
            result.push_back(digits[byte >> 4]);
            result.push_back(digits[byte & 0x0f]);
        }
        return result;
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
    // Double-clicked from Explorer, the tool gets a console of its own that is
    // destroyed when it exits, so the usage text flashed past and read as "the
    // program does not open". Only that case waits: a shell shares its console
    // with the child (two or more attached processes), and a spawned tool has
    // none at all, so scripted runs keep their non-blocking behaviour.
#ifdef _WIN32
    void waitForReaderIfWeOwnTheConsole() {
        DWORD attached[2] = {0, 0};
        if (GetConsoleProcessList(&attached[0], 2) != 1) return;
        std::atexit([] {
            std::fputs("\nPress Enter to close this window.\n", stdout);
            std::fflush(stdout);
            std::getchar();
        });
    }
#endif

    void attachHostConsoleIfNeeded() {
#if defined(_WIN32)
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE* stdoutFile = nullptr;
            FILE* stderrFile = nullptr;
            FILE* stdinFile = nullptr;
            (void)freopen_s(&stdoutFile, "CONOUT$", "w", stdout);
            (void)freopen_s(&stderrFile, "CONOUT$", "w", stderr);
            (void)freopen_s(&stdinFile,  "CONIN$",  "r", stdin);
            return;
        }
        waitForReaderIfWeOwnTheConsole();
#endif
    }

    std::string getCommandName(const char* executableName) {
        if (!executableName || !*executableName) {
            return "quantum-resource-compiler";
        }

        const fs::path executablePath(executableName);
        const std::string filename = executablePath.filename().string();
        return filename.empty() ? std::string("quantum-resource-compiler") : filename;
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
            if (!fillDigest(entry, error)) return false;

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

    // magic + count, then per entry: u16 path length, path, u64 offset, u64 size,
    // u8 key, u32 shift and the raw SHA-256 of the unobfuscated body.
    uint64_t offset = sizeof(RESOURCE_PACK_MAGIC) + 4;
    for (const auto& entry : entries) {
        offset += 2 + entry.path.size() + 8 + 8 + 1 + 4 + entry.digest.size();
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
        out.write(reinterpret_cast<const char*>(entry.digest.data()),
                  static_cast<std::streamsize>(entry.digest.size()));
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
        if (sha256Hex(data.data(), data.size()) != digestToHex(entry.digest)) {
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
            << "  " << commandName << " inspect --pack <file.pak> [--verbose] [--verify]\n"
            << "      [--manifest <assetdb.json>]\n\n"
            << "Options:\n"
            << "  --pack <path>               resources.pak file to list.\n"
            << "      --verbose               Also print offset, key and shift per entry.\n"
            << "      --verify                Validate structure, metadata and all entry bodies.\n"
            << "      --manifest <path>       Compare packed assets/lua SHA-1 values with assetdb.json.\n"
            << "  -h, --help                  Show this help.\n";
    }

    void printGraphUsage(const std::string& commandName) {
        std::cout
            << "Usage:\n"
            << "  " << commandName << " graph [options] <file.graph>\n\n"
            << "Options:\n"
            << "      --check                 Validate the graph and report diagnostics (default).\n"
            << "      --dump                  Print the parsed asset as text.\n"
            << "      --compile               Validate and write the runtime .gdat blob.\n"
            << "      --roundtrip             Compile, decode and compare the graph in memory.\n"
            << "      --read                  Decode an existing .gdat blob and dump it.\n"
            << "  -o, --out <path>            Blob path for --compile. Default: input with .gdat.\n"
            << "      --catalog               List the built-in node kinds.\n"
            << "      --catalog-file <path>   Validate a generated reflection catalog JSON.\n"
            << "      --validate-runtime     Prepare the graph with the runtime handlers.\n"
            << "      --json                  Emit machine-readable output for catalog/runtime validation.\n"
            << "  -h, --help                  Show this help.\n\n"
            << "Exit codes: 0 valid, 1 validation errors, 2 usage or parse errors.\n";
    }

    struct ReflectionCatalogSummary {
        int version = 0;
        size_t classes = 0;
        size_t methods = 0;
        size_t luaMethods = 0;
        size_t properties = 0;
        size_t events = 0;
        size_t enums = 0;
        size_t constants = 0;
        size_t nodes = 0;
    };

    bool readReflectionCatalog(const fs::path& path, ReflectionCatalogSummary& summary,
                               std::string& error) {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            error = "cannot open reflection catalog: " + path.generic_string();
            return false;
        }
        std::stringstream buffer;
        buffer << stream.rdbuf();
        const nlohmann::json document = nlohmann::json::parse(buffer.str(), nullptr, false);
        if (document.is_discarded() || !document.is_object()) {
            error = "reflection catalog is not valid JSON";
            return false;
        }
        summary.version = document.value("version", 0);
        if (summary.version != 1) {
            error = "unsupported reflection catalog version " + std::to_string(summary.version);
            return false;
        }
        const nlohmann::json classes = document.value("classes", nlohmann::json::array());
        if (!classes.is_array()) {
            error = "reflection catalog classes is not an array";
            return false;
        }

        std::set<std::string> classNames;
        for (const nlohmann::json& classValue : classes) {
            if (!classValue.is_object() || !classValue.contains("name") ||
                !classValue.at("name").is_string()) {
                error = "reflection catalog has a class without a name";
                return false;
            }
            const std::string className = classValue.at("name").get<std::string>();
            if (className.empty() || !classNames.insert(className).second) {
                error = "reflection catalog has a duplicate or empty class name";
                return false;
            }
            ++summary.classes;

            const nlohmann::json methods = classValue.value("methods", nlohmann::json::array());
            if (!methods.is_array()) {
                error = "reflection catalog methods is not an array for " + className;
                return false;
            }
            for (const nlohmann::json& method : methods) {
                if (!method.is_object() || !method.contains("name") ||
                    !method.at("name").is_string() || !method.contains("signature") ||
                    !method.at("signature").is_string()) {
                    error = "reflection catalog has an incomplete method in " + className;
                    return false;
                }
                ++summary.methods;
                if (!method.value("cpp_only", false)) ++summary.luaMethods;
            }

            const nlohmann::json properties = classValue.value("properties", nlohmann::json::array());
            const nlohmann::json events = classValue.value("events", nlohmann::json::array());
            const nlohmann::json constants = classValue.value("constants", nlohmann::json::array());
            if (!properties.is_array() || !events.is_array() || !constants.is_array()) {
                error = "reflection catalog has a non-array member list for " + className;
                return false;
            }
            summary.properties += properties.size();
            summary.events += events.size();
            summary.constants += constants.size();
        }
        const nlohmann::json enums = document.value("enums", nlohmann::json::array());
        if (!enums.is_array()) {
            error = "reflection catalog enums is not an array";
            return false;
        }
        std::set<std::string> enumNames;
        for (const nlohmann::json& enumValue : enums) {
            if (!enumValue.is_object() || !enumValue.contains("name") ||
                !enumValue.at("name").is_string() || !enumValue.contains("members") ||
                !enumValue.at("members").is_array()) {
                error = "reflection catalog has an incomplete enum";
                return false;
            }
            const std::string enumName = enumValue.at("name").get<std::string>();
            if (enumName.empty() || !enumNames.insert(enumName).second) {
                error = "reflection catalog has a duplicate or empty enum name";
                return false;
            }
            ++summary.enums;
        }
        summary.nodes = summary.methods + summary.properties + summary.events;
        return true;
    }

    void printUsage(const std::string& commandName) {
        std::cout
            << "Usage:\n"
            << "  " << commandName << " pak [options]        Pack a project's assets into resources.pak.\n"
            << "  " << commandName << " shaders [options]    Compile project shaders (headless).\n"
            << "  " << commandName << " inspect [options]    List the entries of a resources.pak.\n"
            << "  " << commandName << " graph [options]      Validate or compile a visual-script graph.\n"
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
    bool verify = false;
    fs::path manifestPath;
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
        } else if (arg == "--verify") {
            verify = true;
        } else if (arg == "--manifest") {
            if (!requireValue(argc, argv, i, value, error)) {
                std::cerr << commandName << " inspect: " << error << "\n";
                return 2;
            }
            manifestPath = value;
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

    if (verify && !ResourcePack::verify(packPath.string())) {
        std::cerr << commandName << " inspect: pack verification failed: "
                  << packPath.string() << "\n";
        return 1;
    }
    if (verify) {
        std::cout << "VERIFIED " << packPath.generic_string() << "\n";
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
        || (std::memcmp(magic, RESOURCE_PACK_MAGIC_V1, sizeof(RESOURCE_PACK_MAGIC_V1)) != 0
            && std::memcmp(magic, RESOURCE_PACK_MAGIC, sizeof(RESOURCE_PACK_MAGIC)) != 0)) {
        std::cerr << commandName << " inspect: Not a quantum resource pack (bad magic): "
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
        bool hasDigest = false;
        std::array<unsigned char, 32> digest{};
    };
    const bool hasDigest = std::memcmp(magic, RESOURCE_PACK_MAGIC, sizeof(RESOURCE_PACK_MAGIC)) == 0;
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
        entry.hasDigest = hasDigest;
        if (hasDigest && !in.read(reinterpret_cast<char*>(entry.digest.data()),
                                  static_cast<std::streamsize>(entry.digest.size()))) {
            std::cerr << commandName << " inspect: Truncated entry digest at index " << i << ".\n";
            return 1;
        }
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
    std::map<std::string, std::string> packedSha1;
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
        const std::string sha256 = sha256Hex(data.data(), data.size());
        totalSize += entry.size;
        packedSha1[entry.path] = sha1;

        std::cout << "  " << entry.path << "\n";
        std::cout << "      size  " << entry.size << " bytes\n";
        std::cout << "      sha1  " << sha1 << "\n";
        if (entry.hasDigest) {
            std::cout << "      sha256 " << sha256 << (sha256 == digestToHex(entry.digest) ? "\n" : " (MISMATCH)\n");
            if (sha256 != digestToHex(entry.digest)) return 1;
        }
        if (verbose) {
            std::cout << "      offset " << entry.offset
                      << "  key 0x" << std::hex << static_cast<int>(entry.key) << std::dec
                      << "  shift " << entry.shift << "\n";
        }
    }
    std::cout << "TOTAL " << totalSize << " bytes packed, " << entries.size() << " entries\n";

    if (!manifestPath.empty()) {
        std::ifstream manifestStream(manifestPath, std::ios::binary);
        if (!manifestStream) {
            std::cerr << commandName << " inspect: Failed to open asset manifest: "
                      << manifestPath.string() << "\n";
            return 2;
        }
        nlohmann::json document = nlohmann::json::parse(manifestStream, nullptr, false);
        if (document.is_discarded() || !document.is_object()
            || !document.value("files", nlohmann::json::array()).is_array()) {
            std::cerr << commandName << " inspect: Invalid asset manifest: "
                      << manifestPath.string() << "\n";
            return 2;
        }

        std::set<std::string> manifestPaths;
        size_t missing = 0;
        size_t changed = 0;
        const nlohmann::json files = document["files"];
        for (const nlohmann::json& value : files) {
            const std::string path = value.value("path", "");
            if (path.rfind("assets/", 0) != 0 && path.rfind("lua/", 0) != 0) continue;
            const std::string expected = value.value("sha1", "");
            manifestPaths.insert(path);
            const auto packed = packedSha1.find(path);
            if (packed == packedSha1.end()) {
                ++missing;
                std::cout << "  MANIFEST_MISSING " << path << "\n";
            } else if (expected.empty() || packed->second != expected) {
                ++changed;
                std::cout << "  MANIFEST_CHANGED " << path << " expected=" << expected
                          << " actual=" << packed->second << "\n";
            }
        }

        size_t extra = 0;
        for (const auto& packed : packedSha1) {
            if (manifestPaths.find(packed.first) == manifestPaths.end()) {
                ++extra;
                std::cout << "  MANIFEST_EXTRA " << packed.first << "\n";
            }
        }
        std::cout << "MANIFEST " << manifestPath.generic_string()
                  << " missing=" << missing << " changed=" << changed
                  << " extra=" << extra << "\n";
        if (missing != 0 || changed != 0 || extra != 0) return 1;
    }

    return 0;
}

int runGraphCommand(int argc, char** argv, const char* executableName) {
    attachHostConsoleIfNeeded();

    const std::string commandName = getCommandName(executableName);

    fs::path inputPath;
    fs::path outputPath;
    bool help = false;
    bool dump = false;
    bool compile = false;
    bool roundtrip = false;
    bool catalog = false;
    bool catalogFile = false;
    bool validateRuntime = false;
    bool jsonOutput = false;
    bool readBlob = false;
    int modes = 0;
    std::string error;

    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        std::string value;

        if (arg == "-h" || arg == "--help") {
            help = true;
        } else if (arg == "--check") {
            // Validation is the default; accepted so CI invocations are explicit.
        } else if (arg == "--dump") {
            dump = true;
            modes++;
        } else if (arg == "--compile") {
            compile = true;
            modes++;
        } else if (arg == "--roundtrip") {
            roundtrip = true;
            modes++;
        } else if (arg == "--catalog") {
            catalog = true;
            modes++;
        } else if (arg == "--catalog-file") {
            if (!requireValue(argc, argv, i, value, error)) break;
            inputPath = value;
            catalogFile = true;
            modes++;
        } else if (arg == "--validate-runtime") {
            validateRuntime = true;
            modes++;
        } else if (arg == "--json") {
            jsonOutput = true;
        } else if (arg == "--read") {
            readBlob = true;
            modes++;
        } else if (arg == "-o" || arg == "--out") {
            if (!requireValue(argc, argv, i, value, error)) break;
            outputPath = value;
        } else if (!arg.empty() && arg[0] == '-') {
            error = "Unknown argument: " + arg;
            break;
        } else {
            if (!inputPath.empty()) {
                error = "More than one input file given";
                break;
            }
            inputPath = arg;
        }
    }

    if (!error.empty()) {
        std::cerr << commandName << " graph: " << error << "\n\n";
        printGraphUsage(commandName);
        return 2;
    }
    if (help) {
        printGraphUsage(commandName);
        return 0;
    }
    if (modes > 1) {
        std::cerr << commandName << " graph: choose only one graph operation\n\n";
        printGraphUsage(commandName);
        return 2;
    }

    if (catalog) {
        const graph::NodeCatalog& nodeCatalog = graph::NodeCatalog::core();
        std::cout << "Built-in graph node kinds (" << nodeCatalog.getDefinitions().size() << "):\n";
        for (const graph::GraphNodeDef& definition : nodeCatalog.getDefinitions()) {
            std::cout << "  " << definition.kind << "  \"" << definition.title << "\"  ["
                      << definition.category << "]\n";
            for (const graph::GraphPinDef& pin : definition.pins) {
                std::cout << "      " << (pin.dir == graph::GraphPinDir::Input ? "in " : "out ")
                          << (pin.kind == graph::GraphPinKind::Exec ? "exec" : graph::graphValueTypeName(pin.type))
                          << " " << pin.name << "\n";
            }
        }
        return 0;
    }

    if (catalogFile) {
        ReflectionCatalogSummary summary;
        if (!readReflectionCatalog(inputPath, summary, error)) {
            std::cerr << commandName << " graph: " << error << "\n";
            return 1;
        }
        if (jsonOutput) {
            const nlohmann::json result = {
                {"catalog", inputPath.generic_string()},
                {"version", summary.version},
                {"classes", summary.classes},
                {"methods", summary.methods},
                {"luaMethods", summary.luaMethods},
                {"properties", summary.properties},
                {"events", summary.events},
                {"enums", summary.enums},
                {"constants", summary.constants},
                {"nodes", summary.nodes}
            };
            std::cout << result.dump(2) << "\n";
        } else {
            std::cout << "Reflection catalog v" << summary.version
                      << ": classes=" << summary.classes
                      << ", methods=" << summary.methods
                      << ", Lua methods=" << summary.luaMethods
                      << ", properties=" << summary.properties
                      << ", events=" << summary.events
                      << ", enums=" << summary.enums
                      << ", constants=" << summary.constants
                      << ", nodes=" << summary.nodes << "\n";
        }
        return 0;
    }

    if (inputPath.empty()) {
        std::cerr << commandName << " graph: missing <file.graph>\n\n";
        printGraphUsage(commandName);
        return 2;
    }

    if (validateRuntime) {
        graph::GraphAsset asset;
        graph::GraphBlobInfo blobInfo;
        bool loadedBlob = inputPath.extension() == ".gdat";
        if (loadedBlob) {
            if (!graph::GraphReader::readFile(inputPath.string(), asset, blobInfo, error)) {
                std::cerr << commandName << " graph: " << inputPath.generic_string()
                          << ": " << error << "\n";
                return 2;
            }
        } else if (!GraphSerializerYaml::loadFile(inputPath.string(), asset, error)) {
            std::cerr << commandName << " graph: " << inputPath.generic_string()
                      << ": " << error << "\n";
            return 2;
        }

        std::vector<graph::GraphDiagnostic> diagnostics;
        graph::GraphSchema::validate(asset, diagnostics);
        const bool schemaFailed = graph::GraphSchema::hasErrors(diagnostics);
        std::string runtimeError;
        graph::ExecutableGraph executable;
        const bool runtimePrepared = !schemaFailed && executable.prepare(asset, runtimeError);

        if (!diagnostics.empty()) {
            std::cerr << graph::GraphSchema::format(diagnostics);
        }
        if (!runtimePrepared && !schemaFailed) {
            std::cerr << "[error] runtime preparation failed: " << runtimeError << "\n";
        }

        const bool failed = schemaFailed || !runtimePrepared;
        if (jsonOutput) {
            const nlohmann::json result = {
                {"file", inputPath.generic_string()},
                {"format", loadedBlob ? "gdat" : "graph"},
                {"nodes", asset.nodes.size()},
                {"edges", asset.edges.size()},
                {"schemaValid", !schemaFailed},
                {"runtimePrepared", runtimePrepared},
                {"ok", !failed}
            };
            std::cout << result.dump(2) << "\n";
        } else if (!failed) {
            std::cout << inputPath.generic_string() << ": runtime ok ("
                      << executable.getNodes().size() << " nodes, "
                      << executable.getRegisterCount() << " registers)\n";
        }
        return failed ? 1 : 0;
    }

    if (readBlob || (dump && inputPath.extension() == ".gdat")) {
        graph::GraphAsset decoded;
        graph::GraphBlobInfo info;
        if (!graph::GraphReader::readFile(inputPath.string(), decoded, info, error)) {
            std::cerr << commandName << " graph: " << inputPath.generic_string() << ": " << error << "\n";
            return 2;
        }
        char hashText[32];
        std::snprintf(hashText, sizeof(hashText), "%016llx",
                      static_cast<unsigned long long>(info.sourceHash));
        std::cout << "blob v" << info.version << ", " << info.blobSize
                  << " bytes, source hash " << hashText << "\n";
        std::cout << graph::GraphDump::toString(decoded);
        return 0;
    }

    graph::GraphAsset asset;
    if (!GraphSerializerYaml::loadFile(inputPath.string(), asset, error)) {
        std::cerr << commandName << " graph: " << inputPath.generic_string() << ": " << error << "\n";
        return 2;
    }

    std::vector<graph::GraphDiagnostic> diagnostics;
    graph::GraphSchema::validate(asset, diagnostics);
    const std::string formatted = graph::GraphSchema::format(diagnostics);
    if (!formatted.empty()) {
        std::cerr << formatted;
    }
    const bool failed = graph::GraphSchema::hasErrors(diagnostics);

    if (dump) {
        std::cout << graph::GraphDump::toString(asset);
    }

    if (compile) {
        if (failed) {
            return 1;
        }

        std::ifstream sourceFile(inputPath, std::ios::binary);
        if (!sourceFile.is_open()) {
            std::cerr << commandName << " graph: cannot reopen '" << inputPath.generic_string() << "'\n";
            return 2;
        }
        const std::string source((std::istreambuf_iterator<char>(sourceFile)),
                                 std::istreambuf_iterator<char>());
        const uint64_t sourceHash = graph::graphSourceHash(source.data(), source.size());

        if (outputPath.empty()) {
            outputPath = inputPath;
            outputPath.replace_extension(".gdat");
        }

        if (!graph::GraphCompiler::writeFile(outputPath.string(), asset, sourceHash, error)) {
            std::cerr << commandName << " graph: " << error << "\n";
            return 2;
        }

        std::error_code sizeError;
        const uintmax_t blobSize = fs::file_size(outputPath, sizeError);
        char hashText[32];
        std::snprintf(hashText, sizeof(hashText), "%016llx",
                      static_cast<unsigned long long>(sourceHash));
        std::cout << outputPath.generic_string() << ": " << (sizeError ? 0 : blobSize)
                  << " bytes, source hash " << hashText << "\n";
        return 0;
    }

    if (roundtrip) {
        std::ifstream sourceFile(inputPath, std::ios::binary);
        if (!sourceFile.is_open()) {
            std::cerr << commandName << " graph: cannot reopen '" << inputPath.generic_string() << "'\n";
            return 2;
        }
        const std::string source((std::istreambuf_iterator<char>(sourceFile)),
                                 std::istreambuf_iterator<char>());
        const uint64_t sourceHash = graph::graphSourceHash(source.data(), source.size());
        std::vector<uint8_t> blob;
        if (!graph::GraphCompiler::compile(asset, sourceHash, blob, error)) {
            std::cerr << commandName << " graph: roundtrip compile: " << error << "\n";
            return 2;
        }
        graph::GraphAsset decoded;
        graph::GraphBlobInfo info;
        if (!graph::GraphReader::read(blob, decoded, info, error)) {
            std::cerr << commandName << " graph: roundtrip decode: " << error << "\n";
            return 2;
        }
        std::vector<graph::GraphDiagnostic> decodedDiagnostics;
        graph::GraphSchema::validate(decoded, decodedDiagnostics);
        if (graph::GraphSchema::hasErrors(decodedDiagnostics)
                || graph::GraphDump::toString(asset) != graph::GraphDump::toString(decoded)) {
            std::cerr << commandName << " graph: roundtrip mismatch\n";
            if (!decodedDiagnostics.empty()) std::cerr << graph::GraphSchema::format(decodedDiagnostics);
            return 1;
        }
        std::cout << inputPath.generic_string() << ": roundtrip ok (" << blob.size()
                  << " bytes, schema " << info.version << ")\n";
        return 0;
    }

    if (!dump && !failed) {
        std::cout << inputPath.generic_string() << ": ok (" << asset.nodes.size()
                  << " nodes, " << asset.edges.size() << " edges)\n";
    }

    return failed ? 1 : 0;
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
