// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "cli/EngineAssetsCli.h"

#include "json.hpp"
#include "util/AssetDatabase.h"
#include "util/EngineAssets.h"
#include "util/FbxImporter.h"
#include "util/SHA1.h"

#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace quantum::editor;

namespace quantum::editor::cli {

    namespace fs = std::filesystem;

    namespace {

        struct AssetsOptions {
            enum class Action { None, List, Find, Copy, Index, Doctor, Verify, Import };
            Action action = Action::None;
            bool help = false;
            bool json = false;
            bool overwrite = false;
            bool force = false;
            bool deep = false;
            std::string text;              // `find` pattern
            std::string source;            // `copy` positional path
            std::string rootFilter;        // --root label
            std::string projectPath;
            std::string assetsDir;
            std::string luaDir;
            std::string engineDir;         // --engine-dir, for a build that has no copy
            int limit = 40;
        };

        std::string trimmed(const std::string& value) {
            const size_t begin = value.find_first_not_of(" \t\r\n");
            if (begin == std::string::npos) {
                return std::string();
            }
            const size_t end = value.find_last_not_of(" \t\r\n");
            return value.substr(begin, end - begin + 1);
        }

        std::string commandBaseName(const char* executableName) {
            if (!executableName || !*executableName) {
                return "quantum-editor";
            }
            const std::string full(executableName);
            const size_t slash = full.find_last_of("/\\");
            const std::string name = slash == std::string::npos ? full : full.substr(slash + 1);
            const size_t dot = name.find('.');
            return dot == std::string::npos ? name : name.substr(0, dot);
        }

        std::string lower(std::string value) {
            std::transform(value.begin(), value.end(), value.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return value;
        }

        // Paths are the only dynamic strings in the JSON form, and a Windows path is full of
        // the one character that needs escaping, so fold both separators to '/'.
        std::string jsonPath(const fs::path& path) {
            std::string text = path.generic_string();
            std::string out;
            out.reserve(text.size() + 8);
            for (const char c : text) {
                if (c == '"' || c == '\\') {
                    out += '\\';
                }
                out += c;
            }
            return out;
        }

        std::string humanBytes(unsigned long long bytes) {
            if (bytes < 1024ULL) {
                return std::to_string(bytes) + " B";
            }
            const double kib = static_cast<double>(bytes) / 1024.0;
            if (kib < 1024.0) {
                char buffer[32];
                std::snprintf(buffer, sizeof(buffer), "%.1f KiB", kib);
                return buffer;
            }
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%.1f MiB", kib / 1024.0);
            return buffer;
        }

        struct RootTally {
            unsigned long long files = 0;
            unsigned long long bytes = 0;
        };

        RootTally tallyRoot(const fs::path& root) {
            RootTally tally;
            std::error_code ec;
            for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end; it.increment(ec)) {
                if (it->is_regular_file(ec) && !ec) {
                    ++tally.files;
                    tally.bytes += static_cast<unsigned long long>(it->file_size(ec));
                    if (ec) {
                        ec.clear();
                    }
                }
            }
            return tally;
        }

        bool requireValue(int argc, char** argv, int& index, std::string& value, std::string& error) {
            if (index + 1 >= argc) {
                error = std::string(argv[index]) + " needs a value";
                return false;
            }
            value = argv[++index];
            return true;
        }

        bool parseAssetsArgs(int argc, char** argv, AssetsOptions& options, std::string& error) {
            // argv[0] is the `assets` token itself, exactly like the components and doctor
            // subcommands, so the verb is the argument behind it.
            const std::string verb = argc >= 2 && argv[1] ? argv[1] : "";
            if (verb == "list") {
                options.action = AssetsOptions::Action::List;
            } else if (verb == "find") {
                options.action = AssetsOptions::Action::Find;
            } else if (verb == "copy") {
                options.action = AssetsOptions::Action::Copy;
            } else if (verb == "index") {
                options.action = AssetsOptions::Action::Index;
            } else if (verb == "doctor") {
                options.action = AssetsOptions::Action::Doctor;
            } else if (verb == "verify") {
                options.action = AssetsOptions::Action::Verify;
            } else if (verb == "import") {
                options.action = AssetsOptions::Action::Import;
            } else if (verb.empty() || verb == "--help" || verb == "-h" || verb == "help") {
                options.help = true;
            } else {
                error = "expected `list`, `find`, `copy`, `index`, `doctor`, `verify` or `import`, got `" + verb + "`";
                return false;
            }

            for (int i = 2; i < argc; ++i) {
                const std::string arg = argv[i];

                if (arg == "--help" || arg == "-h") {
                    options.help = true;
                } else if (arg == "--json") {
                    options.json = true;
                } else if (arg == "--overwrite") {
                    options.overwrite = true;
                } else if (arg == "--force") {
                    options.force = true;
                } else if (arg == "--deep") {
                    options.deep = true;
                } else if (arg == "--project") {
                    if (!requireValue(argc, argv, i, options.projectPath, error)) return false;
                } else if (arg == "--assets") {
                    if (!requireValue(argc, argv, i, options.assetsDir, error)) return false;
                } else if (arg == "--lua") {
                    if (!requireValue(argc, argv, i, options.luaDir, error)) return false;
                } else if (arg == "--engine-dir") {
                    if (!requireValue(argc, argv, i, options.engineDir, error)) return false;
                } else if (arg == "--root") {
                    if (!requireValue(argc, argv, i, options.rootFilter, error)) return false;
                } else if (arg == "--limit") {
                    std::string value;
                    if (!requireValue(argc, argv, i, value, error)) return false;
                    options.limit = std::atoi(value.c_str());
                } else if (!arg.empty() && arg[0] == '-') {
                    error = "unknown option: " + arg;
                    return false;
                } else if (options.action == AssetsOptions::Action::Find && options.text.empty()) {
                    options.text = arg;
                } else if (options.action == AssetsOptions::Action::Copy && options.source.empty()) {
                    options.source = arg;
                } else {
                    error = "unexpected argument: " + arg;
                    return false;
                }
            }

            if (options.help) {
                return true;
            }
            if (options.action == AssetsOptions::Action::Find && options.text.empty()) {
                error = "find needs a pattern: assets find <text>";
                return false;
            }
            if (options.action == AssetsOptions::Action::Copy) {
                if (options.source.empty()) {
                    error = "copy needs an engine path: assets copy <engine-relative path> --project <dir>";
                    return false;
                }
                if (options.projectPath.empty()) {
                    error = "copy needs --project <dir>";
                    return false;
                }
            }
            if (options.action == AssetsOptions::Action::Index || options.action == AssetsOptions::Action::Doctor ||
                options.action == AssetsOptions::Action::Import) {
                if (options.projectPath.empty()) {
                    const char* action = options.action == AssetsOptions::Action::Index ? "index" :
                        (options.action == AssetsOptions::Action::Doctor ? "doctor" : "import");
                    error = std::string(action) +
                            std::string(" needs --project <dir>");
                    return false;
                }
            }
            if (options.action == AssetsOptions::Action::Verify && options.projectPath.empty()) {
                error = "verify needs --project <dir>";
                return false;
            }
            if (options.limit < 1) {
                options.limit = 1;
            }
            return true;
        }

        void printUsage(const std::string& commandName) {
            std::cout
                << commandName << " assets: browse the content the engine ships beside the editor.\n\n"
                << "The engine folder is the copy `copy_quantum_editor_engine` puts next to this\n"
                << "executable; when it is absent the command falls back to the engine/ of the\n"
                << "checkout it was built from. The three folders are read-only here: only `copy`\n"
                << "writes, and only into the project it names.\n\n"
                << "  " << commandName << " assets list [--engine-dir <dir>] [--root <label>] [--json]\n"
                << "  " << commandName << " assets find <text> [--root <label>] [--limit N] [--json]\n"
                << "  " << commandName << " assets copy <engine path> --project <dir|project.yaml>\n"
                << "                                [--assets <dir>] [--lua <dir>] [--overwrite]\n\n"
                << "  " << commandName << " assets index --project <dir|project.yaml> [--force] [--json]\n"
                << "  " << commandName << " assets doctor --project <dir|project.yaml> [--json]\n\n"
                << "  " << commandName << " assets verify --project <dir|project.yaml> [--deep] [--json]\n\n"
                << "  " << commandName << " assets import --project <dir|project.yaml> [--assets <dir>]\n"
                << "                                [--force] [--json]\n\n"
                << "  --engine-dir <dir>   Engine folder to read instead of the resolved one.\n"
                << "  --root <label>       Restrict to one root: assets, components or shaderlib.\n"
                << "  --project <path>     Project directory or its project.yaml.\n"
                << "  --assets <dir>       Project assets root for copy/import. Copy defaults to\n"
                << "                       <project>/assets; import reads assetsDir from\n"
                << "                       project.yaml and otherwise uses <project>.\n"
                << "  --lua <dir>          Lua root (default luaDir from project.yaml, else \".\").\n"
                << "                       A .lua copy lands in <lua root>/Components, where the\n"
                << "                       New component dialog looks for ready-made scripts.\n"
                << "  --overwrite          Replace a file the project already owns.\n"
                << "  --force              Rehash project assets and reimport stale FBX files.\n"
                << "  --deep               Validate known binary headers and parse JSON/YAML assets.\n"
                << "  --limit N            Stop `find` after N rows (default 40).\n"
                << "  --json               Machine-readable output.\n\n"
                << "Engine paths are relative to the engine folder, so `copy components/Wind.lua`\n"
                << "and `copy <engine>/components/Wind.lua` are the same request.\n"
                << "Exit codes: 0 ok, 1 nothing found or a copy failed, 2 bad arguments.\n";
        }

        using Json = nlohmann::json;

        fs::path projectRootOf(const std::string& path);
        fs::path resolveProjectAssetsRoot(const fs::path& project, const std::string& assetsDir,
                                          std::string& error);

        Json databaseRecordJson(const AssetRecord& record) {
            Json value = {
                {"path", record.path},
                {"kind", record.kind},
                {"size", record.size},
                {"sha1", record.sha1},
                {"metadata", record.metadata},
                {"dependencies", record.dependencies},
                {"missing", record.missingDependencies}
            };
            if (!record.error.empty()) value["error"] = record.error;
            return value;
        }

        struct AssetVerifyIssue {
            std::string type;
            std::string path;
            std::string dependency;
            std::string expected;
            std::string actual;
            std::string detail;
        };

        bool readAndHashFile(const fs::path& path, std::string& hash, std::string& error) {
            std::ifstream stream(path, std::ios::binary);
            if (!stream) {
                error = "cannot open file";
                return false;
            }

            std::ostringstream contents;
            contents << stream.rdbuf();
            if (!stream.eof() && stream.fail()) {
                error = "cannot read file";
                return false;
            }
            hash = SHA1::hash(contents.str());
            return true;
        }

        bool readPrefix(const fs::path& path, std::vector<uint8_t>& bytes, size_t count,
                        std::string& error) {
            std::ifstream stream(path, std::ios::binary);
            if (!stream) {
                error = "cannot open file";
                return false;
            }
            bytes.resize(count);
            stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            bytes.resize(static_cast<size_t>(stream.gcount()));
            if (stream.bad()) {
                error = "cannot read file";
                return false;
            }
            return true;
        }

        bool readTextAsset(const fs::path& path, std::string& text, std::string& error) {
            constexpr size_t maxBytes = 8 * 1024 * 1024;
            std::error_code ec;
            const uintmax_t size = fs::file_size(path, ec);
            if (ec) {
                error = ec.message();
                return false;
            }
            if (size > maxBytes) {
                error = "text asset exceeds 8 MiB deep-validation limit";
                return false;
            }
            std::ifstream stream(path, std::ios::binary);
            if (!stream) {
                error = "cannot open file";
                return false;
            }
            text.resize(static_cast<size_t>(size));
            if (!text.empty() && !stream.read(text.data(), static_cast<std::streamsize>(text.size()))) {
                error = "cannot read file";
                return false;
            }
            return true;
        }

        uint32_t littleU32(const std::vector<uint8_t>& bytes, size_t offset) {
            return static_cast<uint32_t>(bytes[offset])
                | (static_cast<uint32_t>(bytes[offset + 1]) << 8)
                | (static_cast<uint32_t>(bytes[offset + 2]) << 16)
                | (static_cast<uint32_t>(bytes[offset + 3]) << 24);
        }

        uint32_t bigU32(const std::vector<uint8_t>& bytes, size_t offset) {
            return (static_cast<uint32_t>(bytes[offset]) << 24)
                | (static_cast<uint32_t>(bytes[offset + 1]) << 16)
                | (static_cast<uint32_t>(bytes[offset + 2]) << 8)
                | static_cast<uint32_t>(bytes[offset + 3]);
        }

        bool hasBytes(const std::vector<uint8_t>& bytes, std::initializer_list<uint8_t> expected) {
            if (bytes.size() < expected.size()) return false;
            size_t index = 0;
            for (const uint8_t value : expected) {
                if (bytes[index++] != value) return false;
            }
            return true;
        }

        bool validateGlb(const fs::path& path, uintmax_t fileSize, std::string& detail) {
            if (fileSize < 20) {
                detail = "GLB is shorter than its header";
                return false;
            }

            std::ifstream stream(path, std::ios::binary);
            if (!stream) {
                detail = "cannot open GLB";
                return false;
            }
            std::vector<uint8_t> header(12);
            if (!stream.read(reinterpret_cast<char*>(header.data()),
                             static_cast<std::streamsize>(header.size())) ||
                !hasBytes(header, {'g', 'l', 'T', 'F'}) || littleU32(header, 4) != 2) {
                detail = "invalid GLB header";
                return false;
            }

            const uint64_t declaredLength = littleU32(header, 8);
            if (declaredLength != fileSize) {
                detail = "GLB declared length does not match the file";
                return false;
            }

            constexpr uint32_t jsonChunkType = 0x4E4F534A;
            constexpr size_t maxJsonBytes = 8 * 1024 * 1024;
            uint64_t offset = 12;
            bool jsonSeen = false;
            while (offset < fileSize) {
                if (fileSize - offset < 8) {
                    detail = "GLB has a truncated chunk header";
                    return false;
                }
                std::vector<uint8_t> chunkHeader(8);
                stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
                if (!stream.read(reinterpret_cast<char*>(chunkHeader.data()),
                                 static_cast<std::streamsize>(chunkHeader.size()))) {
                    detail = "cannot read GLB chunk header";
                    return false;
                }
                const uint64_t chunkLength = littleU32(chunkHeader, 0);
                const uint32_t chunkType = littleU32(chunkHeader, 4);
                if (chunkLength > fileSize - offset - 8) {
                    detail = "GLB chunk exceeds the declared file length";
                    return false;
                }
                if ((chunkLength & 3u) != 0) {
                    detail = "GLB chunk length is not 4-byte aligned";
                    return false;
                }

                if (offset == 12 && chunkType != jsonChunkType) {
                    detail = "GLB first chunk is not JSON";
                    return false;
                }
                if (chunkType == jsonChunkType) {
                    if (jsonSeen || chunkLength == 0 || chunkLength > maxJsonBytes) {
                        detail = "GLB has an invalid JSON chunk";
                        return false;
                    }
                    std::string text(static_cast<size_t>(chunkLength), '\0');
                    if (!stream.read(text.data(), static_cast<std::streamsize>(text.size()))) {
                        detail = "cannot read GLB JSON chunk";
                        return false;
                    }
                    while (!text.empty() && (text.back() == '\0' || text.back() == ' ' ||
                                             text.back() == '\t' || text.back() == '\r' ||
                                             text.back() == '\n')) {
                        text.pop_back();
                    }
                    const Json document = Json::parse(text, nullptr, false);
                    if (document.is_discarded() || !document.is_object()) {
                        detail = "GLB JSON chunk is not a valid object";
                        return false;
                    }
                    jsonSeen = true;
                } else {
                    stream.seekg(static_cast<std::streamoff>(chunkLength), std::ios::cur);
                }
                if (!stream) {
                    detail = "cannot skip GLB chunk";
                    return false;
                }
                offset += 8 + chunkLength;
            }
            if (offset != fileSize || !jsonSeen) {
                detail = "GLB has no complete JSON chunk";
                return false;
            }
            return true;
        }

        bool validateFbx(const fs::path& path, const std::vector<uint8_t>& prefix,
                         uintmax_t fileSize, std::string& detail) {
            const bool binary = prefix.size() >= 21 &&
                hasBytes(prefix, {'K', 'a', 'y', 'd', 'a', 'r', 'a', ' ', 'F', 'B', 'X', ' ',
                                  'B', 'i', 'n', 'a', 'r', 'y', ' ', ' '}) &&
                prefix[20] == 0;
            if (binary) {
                if (fileSize < 27 || prefix.size() < 27) {
                    detail = "binary FBX is shorter than its version header";
                    return false;
                }
                const uint32_t version = littleU32(prefix, 23);
                if (version < 5000 || version > 10000) {
                    detail = "binary FBX has an invalid version";
                    return false;
                }
                return true;
            }

            std::string text;
            if (!readTextAsset(path, text, detail)) return false;
            const size_t marker = text.find("FBXVersion:");
            if (marker == std::string::npos) {
                detail = "ASCII FBX has no FBXVersion header";
                return false;
            }
            size_t begin = marker + std::strlen("FBXVersion:");
            while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
            size_t end = begin;
            while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end]))) ++end;
            if (end == begin) {
                detail = "ASCII FBX has an invalid FBXVersion header";
                return false;
            }
            return true;
        }

        bool validateDeepAsset(const fs::path& path, const AssetRecord& record, std::string& detail) {
            detail.clear();
            const std::string extension = lower(path.extension().string());
            std::error_code ec;
            const uintmax_t fileSize = fs::file_size(path, ec);
            if (ec || fileSize == 0) {
                detail = ec ? ec.message() : "file is empty";
                return false;
            }

            std::vector<uint8_t> prefix;
            const size_t prefixSize = extension == ".png" ? 24 : 64;
            if (!readPrefix(path, prefix, prefixSize, detail)) return false;

            if (extension == ".png") {
                if (prefix.size() < 24 || !hasBytes(prefix, {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a})
                        || bigU32(prefix, 16) == 0 || bigU32(prefix, 20) == 0) {
                    detail = "invalid PNG signature or dimensions";
                    return false;
                }
                return true;
            }
            if (extension == ".jpg" || extension == ".jpeg") {
                if (!hasBytes(prefix, {0xff, 0xd8, 0xff})) detail = "invalid JPEG signature";
                return detail.empty();
            }
            if (extension == ".bmp") {
                if (prefix.size() < 26 || !hasBytes(prefix, {'B', 'M'})) detail = "invalid BMP signature";
                return detail.empty();
            }
            if (extension == ".hdr") {
                const std::string text(prefix.begin(), prefix.end());
                if (text.find("#?RADIANCE") != 0 && text.find("#?RGBE") != 0) detail = "invalid Radiance HDR signature";
                return detail.empty();
            }
            if (extension == ".exr") {
                if (!hasBytes(prefix, {0x76, 0x2f, 0x31, 0x01})) detail = "invalid OpenEXR signature";
                return detail.empty();
            }
            if (extension == ".glb") {
                return validateGlb(path, fileSize, detail);
            }
            if (extension == ".gltf" || extension == ".json") {
                std::string text;
                if (!readTextAsset(path, text, detail)) return false;
                const Json parsed = Json::parse(text, nullptr, false);
                if (parsed.is_discarded() || !parsed.is_object()) detail = "invalid JSON document";
                return detail.empty();
            }
            if (extension == ".wav") {
                if (prefix.size() < 12 || !hasBytes(prefix, {'R', 'I', 'F', 'F'})
                        || prefix[8] != 'W' || prefix[9] != 'A' || prefix[10] != 'V' || prefix[11] != 'E') {
                    detail = "invalid RIFF/WAVE header";
                    return false;
                }
                return true;
            }
            if (extension == ".ogg") {
                if (!hasBytes(prefix, {'O', 'g', 'g', 'S'})) detail = "invalid OggS header";
                return detail.empty();
            }
            if (extension == ".flac") {
                if (!hasBytes(prefix, {'f', 'L', 'a', 'C'})) detail = "invalid FLAC header";
                return detail.empty();
            }
            if (extension == ".mp3") {
                const bool id3 = hasBytes(prefix, {'I', 'D', '3'});
                const bool frame = prefix.size() >= 2 && prefix[0] == 0xff && (prefix[1] & 0xe0) == 0xe0;
                if (!id3 && !frame) detail = "invalid MP3 frame or ID3 header";
                return detail.empty();
            }
            if (extension == ".ttf" || extension == ".otf") {
                const bool ttf = hasBytes(prefix, {0x00, 0x01, 0x00, 0x00});
                const bool otto = hasBytes(prefix, {'O', 'T', 'T', 'O'});
                const bool trueType = hasBytes(prefix, {'t', 'r', 'u', 'e'});
                const bool type1 = hasBytes(prefix, {'t', 'y', 'p', '1'});
                if (!ttf && !otto && !trueType && !type1) detail = "invalid font sfnt header";
                return detail.empty();
            }
            if (extension == ".scene" || extension == ".yaml" || extension == ".yml"
                    || extension == ".material" || extension == ".mat" || extension == ".graph") {
                std::string text;
                if (!readTextAsset(path, text, detail)) return false;
                try {
                    YAML::Load(text);
                } catch (const YAML::Exception& exception) {
                    detail = exception.msg;
                    return false;
                }
                return true;
            }
            if (extension == ".fbx") {
                return validateFbx(path, prefix, fileSize, detail);
            }
            if (extension == ".obj") {
                std::string text;
                if (!readTextAsset(path, text, detail)) return false;
                if (trimmed(text).empty()) detail = "model text is empty";
                return detail.empty();
            }

            (void)record;
            return true;
        }

        bool projectRelativePath(const fs::path& project, const std::string& value,
                                 fs::path& resolved) {
            const fs::path relative(value);
            if (value.empty() || relative.is_absolute()) {
                return false;
            }

            const fs::path normalized = relative.lexically_normal();
            const std::string normalizedText = normalized.generic_string();
            if (normalizedText.empty() || normalizedText == ".." ||
                normalizedText.rfind("../", 0) == 0) {
                return false;
            }
            resolved = (project / normalized).lexically_normal();
            return true;
        }

        Json assetVerifyIssueJson(const AssetVerifyIssue& issue) {
            Json value = {
                {"type", issue.type},
                {"path", issue.path}
            };
            if (!issue.dependency.empty()) value["dependency"] = issue.dependency;
            if (!issue.expected.empty()) value["expectedSha1"] = issue.expected;
            if (!issue.actual.empty()) value["actualSha1"] = issue.actual;
            if (!issue.detail.empty()) value["detail"] = issue.detail;
            return value;
        }

        void printAssetVerifyError(const AssetsOptions& options, const fs::path& project,
                                   const std::string& error) {
            if (options.json) {
                Json result = {
                    {"ok", false},
                    {"project", project.generic_string()},
                    {"database", AssetDatabase::databasePath(project).generic_string()},
                    {"error", error}
                };
                std::cout << result.dump(2) << "\n";
            } else {
                std::cerr << "assets verify: " << error << "\n";
            }
        }

        int runAssetVerify(const AssetsOptions& options) {
            const fs::path project = projectRootOf(options.projectPath);
            std::error_code ec;
            if (!fs::is_directory(project, ec)) {
                printAssetVerifyError(options, project,
                                      "not a project directory: " + project.generic_string());
                return 2;
            }

            const fs::path databasePath = AssetDatabase::databasePath(project);
            if (!fs::is_regular_file(databasePath, ec)) {
                printAssetVerifyError(options, project,
                                      "asset database not found: " + databasePath.generic_string());
                return 2;
            }

            AssetDatabase database(project);
            std::string error;
            if (!database.load(error)) {
                printAssetVerifyError(options, project, error);
                return 2;
            }

            std::vector<AssetVerifyIssue> issues;
            size_t checked = 0;
            size_t removed = 0;
            size_t changed = 0;
            size_t missingDependencies = 0;
            size_t deepChecked = 0;

            for (const auto& pair : database.records()) {
                const AssetRecord& record = pair.second;
                fs::path file;
                if (!projectRelativePath(project, record.path, file)) {
                    issues.push_back({"invalid-path", record.path, {}, {}, {},
                                      "record path is not project-relative"});
                    ++removed;
                    continue;
                }

                std::error_code fileEc;
                if (!fs::is_regular_file(file, fileEc)) {
                    issues.push_back({"removed", record.path, {}, record.sha1, {},
                                      "registered file is missing"});
                    ++removed;
                } else {
                    ++checked;
                    std::string actualHash;
                    std::string hashError;
                    if (!readAndHashFile(file, actualHash, hashError)) {
                        issues.push_back({"unreadable", record.path, {}, record.sha1, {}, hashError});
                        ++changed;
                    } else if (lower(actualHash) != lower(record.sha1)) {
                        issues.push_back({"changed", record.path, {}, record.sha1, actualHash,
                                          "content SHA-1 differs from the asset database"});
                        ++changed;
                    }
                    if (options.deep) {
                        ++deepChecked;
                        std::string detail;
                        if (!validateDeepAsset(file, record, detail)) {
                            issues.push_back({"invalid-format", record.path, {}, {}, {}, detail});
                            ++changed;
                        }
                    }
                }

                for (const std::string& dependency : record.dependencies) {
                    fs::path dependencyPath;
                    const bool validPath = projectRelativePath(project, dependency, dependencyPath);
                    std::error_code dependencyEc;
                    const bool present = validPath && fs::is_regular_file(dependencyPath, dependencyEc);
                    if (!present) {
                        issues.push_back({"missing-dependency", record.path, dependency, {}, {},
                                          "dependency is not present in the project"});
                        ++missingDependencies;
                    }
                }

                for (const std::string& persistedMissing : record.missingDependencies) {
                    fs::path dependencyPath;
                    const bool validPath = projectRelativePath(project, persistedMissing, dependencyPath);
                    std::error_code dependencyEc;
                    const bool present = validPath && fs::is_regular_file(dependencyPath, dependencyEc);
                    if (present) {
                        issues.push_back({"stale-missing-dependency", record.path, persistedMissing,
                                          {}, {}, "database still marks an existing dependency as missing"});
                    }
                }

                if (!record.error.empty()) {
                    issues.push_back({"database-error", record.path, {}, {}, {}, record.error});
                }
            }

            if (options.json) {
                Json serializedIssues = Json::array();
                for (const AssetVerifyIssue& issue : issues) {
                    serializedIssues.push_back(assetVerifyIssueJson(issue));
                }
                Json result = {
                    {"ok", issues.empty()},
                    {"project", project.generic_string()},
                    {"database", databasePath.generic_string()},
                    {"records", database.records().size()},
                    {"checked", checked},
                    {"removed", removed},
                    {"changed", changed},
                    {"missingDependencies", missingDependencies},
                    {"deep", options.deep},
                    {"deepChecked", deepChecked},
                    {"divergenceCount", issues.size()},
                    {"issues", serializedIssues}
                };
                std::cout << result.dump(2) << "\n";
            } else {
                std::cout << "Asset verification: " << databasePath.generic_string() << "\n"
                          << "Checked " << checked << " of " << database.records().size()
                          << " registered file(s); removed=" << removed
                          << ", changed=" << changed
                          << ", missing dependencies=" << missingDependencies;
                if (options.deep) std::cout << ", deep-checked=" << deepChecked;
                std::cout << ".\n";
                if (issues.empty()) {
                    std::cout << "Asset database matches the project.\n";
                } else {
                    std::cout << "Asset database divergence: " << issues.size() << " issue(s).\n";
                    for (const AssetVerifyIssue& issue : issues) {
                        std::cout << "  " << issue.type << ": " << issue.path;
                        if (!issue.dependency.empty()) std::cout << " -> " << issue.dependency;
                        if (!issue.expected.empty() || !issue.actual.empty()) {
                            std::cout << " expected=" << issue.expected << " actual=" << issue.actual;
                        }
                        if (!issue.detail.empty()) std::cout << " (" << issue.detail << ")";
                        std::cout << "\n";
                    }
                }
            }

            return issues.empty() ? 0 : 1;
        }

        int runAssetDatabase(const AssetsOptions& options, bool doctor) {
            const fs::path project = projectRootOf(options.projectPath);
            std::error_code ec;
            if (!fs::is_directory(project, ec)) {
                std::cerr << "assets " << (doctor ? "doctor" : "index")
                          << ": not a project directory: " << project.generic_string() << "\n";
                return 2;
            }

            AssetDatabase database(project);
            AssetDatabaseReport report;
            std::string error;
            if (!database.update(options.force, report, error)) {
                std::cerr << "assets " << (doctor ? "doctor" : "index") << ": " << error << "\n";
                return 2;
            }

            Json broken = Json::array();
            for (const auto& pair : database.records()) {
                if (pair.second.broken()) broken.push_back(databaseRecordJson(pair.second));
            }

            if (options.json) {
                Json result = {
                    {"project", project.generic_string()},
                    {"database", AssetDatabase::databasePath(project).generic_string()},
                    {"manifestSha1", report.manifestSha1},
                    {"changed", report.changed},
                    {"scanned", report.scanned},
                    {"hashed", report.hashed},
                    {"reused", report.reused},
                    {"removed", report.removed},
                    {"brokenCount", report.broken},
                    {"broken", broken}
                };
                std::cout << result.dump(2) << "\n";
            } else {
                std::cout << "Asset database: " << AssetDatabase::databasePath(project).generic_string() << "\n"
                          << "Manifest SHA-1: " << report.manifestSha1 << "\n"
                          << "Scanned " << report.scanned << " file(s), hashed " << report.hashed
                          << ", reused " << report.reused << ", removed " << report.removed << ".\n";
                if (report.broken == 0) {
                    std::cout << (doctor ? "Asset doctor: no broken dependencies.\n"
                                         : "Asset index is healthy.\n");
                } else {
                    std::cout << "Broken assets: " << report.broken << "\n";
                    for (const auto& pair : database.records()) {
                        if (!pair.second.broken()) continue;
                        std::cout << "  " << pair.second.path;
                        if (!pair.second.error.empty()) std::cout << " [" << pair.second.error << "]";
                        for (const std::string& missing : pair.second.missingDependencies) {
                            std::cout << " missing=" << missing;
                        }
                        std::cout << "\n";
                    }
                }
            }
            return doctor && report.broken > 0 ? 1 : 0;
        }

        int runAssetImport(const AssetsOptions& options) {
            const fs::path project = projectRootOf(options.projectPath);
            std::error_code ec;
            if (!fs::is_directory(project, ec)) {
                std::cerr << "assets import: not a project directory: " << project.generic_string() << "\n";
                return 2;
            }

            std::string assetsError;
            const fs::path assetsRoot = resolveProjectAssetsRoot(project, options.assetsDir, assetsError);
            if (!assetsError.empty()) {
                std::cerr << "assets import: " << assetsError << "\n";
                return 2;
            }
            if (!fs::is_directory(assetsRoot, ec)) {
                std::cerr << "assets import: assets directory not found: " << assetsRoot.generic_string() << "\n";
                return 2;
            }

            const fs::path importRoot = project / ".quantum" / "imported";
            fs::create_directories(importRoot, ec);
            if (ec) {
                std::cerr << "assets import: cannot create import cache: " << ec.message() << "\n";
                return 2;
            }

            struct ImportRow {
                std::string source;
                std::string output;
                std::string state;
                std::string error;
                std::vector<std::string> warnings;
            };
            std::vector<ImportRow> rows;
            size_t scanned = 0;
            size_t imported = 0;
            size_t current = 0;
            size_t failed = 0;

            for (fs::recursive_directory_iterator it(
                     assetsRoot, fs::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end; it.increment(ec)) {
                const std::string filename = it->path().filename().string();
                if (it->is_directory(ec) && !filename.empty() &&
                    (filename[0] == '.' || filename == "build")) {
                    it.disable_recursion_pending();
                    ec.clear();
                    continue;
                }
                if (!it->is_regular_file(ec) || ec) {
                    ec.clear();
                    continue;
                }
                if (!filename.empty() && filename[0] == '.') continue;
                if (!FbxImporter::isFbxFile(it->path())) continue;
                ++scanned;

                const fs::path relative = fs::relative(it->path(), assetsRoot, ec).lexically_normal();
                if (ec || relative.empty() || relative.generic_string().rfind("..", 0) == 0) {
                    ec.clear();
                    ++failed;
                    rows.push_back({it->path().generic_string(), std::string(), "invalid-path",
                                    "FBX path is outside the assets root", {}});
                    continue;
                }

                fs::path output = importRoot / relative;
                output += ".glb";
                const FbxImporter::State state = FbxImporter::getState(it->path(), output, assetsRoot);
                if (state == FbxImporter::State::UpToDate && !options.force) {
                    ++current;
                    rows.push_back({relative.generic_string(), output.lexically_relative(project).generic_string(),
                                    "up-to-date", std::string(), {}});
                    continue;
                }

                const FbxImporter::Result result = FbxImporter::import(
                    it->path(), output, assetsRoot, options.force);
                if (result.success) {
                    ++imported;
                    rows.push_back({relative.generic_string(), output.lexically_relative(project).generic_string(),
                                    "imported", std::string(), result.warnings});
                } else {
                    ++failed;
                    rows.push_back({relative.generic_string(), output.lexically_relative(project).generic_string(),
                                    "failed", result.error, result.warnings});
                }
            }

            if (ec) {
                std::cerr << "assets import: scan failed: " << ec.message() << "\n";
                return 2;
            }

            if (options.json) {
                Json result = {
                    {"project", project.generic_string()},
                    {"assetsRoot", assetsRoot.generic_string()},
                    {"importRoot", importRoot.generic_string()},
                    {"scanned", scanned},
                    {"imported", imported},
                    {"upToDate", current},
                    {"failed", failed},
                    {"files", Json::array()}
                };
                for (const ImportRow& row : rows) {
                    Json value = {
                        {"source", row.source},
                        {"output", row.output},
                        {"state", row.state},
                        {"warnings", row.warnings}
                    };
                    if (!row.error.empty()) value["error"] = row.error;
                    result["files"].push_back(value);
                }
                std::cout << result.dump(2) << "\n";
            } else {
                std::cout << "FBX import: scanned " << scanned << ", imported " << imported
                          << ", up-to-date " << current << ", failed " << failed << ".\n";
                for (const ImportRow& row : rows) {
                    std::cout << "  " << row.state << ": " << row.source;
                    if (!row.output.empty()) std::cout << " -> " << row.output;
                    if (!row.error.empty()) std::cout << " (" << row.error << ")";
                    std::cout << "\n";
                    for (const std::string& warning : row.warnings) {
                        std::cout << "    warning: " << warning << "\n";
                    }
                }
            }
            return failed == 0 ? 0 : 1;
        }

        // The roots the command may show: `EngineAssets::roots()` filtered by --root, or the
        // --engine-dir override mapped to the same three labels when the process resolved a
        // different folder (a build that never ran the copy).
        std::vector<EngineAssetRoot> selectedRoots(const AssetsOptions& options) {
            std::vector<EngineAssetRoot> chosen;
            for (const EngineAssetRoot& root : EngineAssets::roots()) {
                if (!options.rootFilter.empty() && lower(root.label) != lower(options.rootFilter)) {
                    continue;
                }
                chosen.push_back(root);
            }
            return chosen;
        }

        int runList(const AssetsOptions& options) {
            const std::vector<EngineAssetRoot> roots = selectedRoots(options);

            if (!options.json) {
                std::cout << "Engine content at " << EngineAssets::engineDir().string() << "\n";
                if (roots.empty()) {
                    if (options.rootFilter.empty()) {
                        std::cout << "  nothing: the engine folder holds no assets/, components/ or shaderlib/\n";
                    } else {
                        std::cout << "  nothing: no engine root is named " << options.rootFilter
                                  << " (the labels are assets, components and shaderlib)\n";
                    }
                    return 1;
                }
            } else {
                std::cout << "{\n  \"source\": \"quantum-editor-cmd assets\",\n"
                          << "  \"engineDir\": \"" << jsonPath(EngineAssets::engineDir()) << "\",\n"
                          << "  \"roots\": [\n";
            }

            unsigned long long totalFiles = 0;
            unsigned long long totalBytes = 0;
            for (size_t i = 0; i < roots.size(); ++i) {
                const RootTally tally = tallyRoot(roots[i].path);
                totalFiles += tally.files;
                totalBytes += tally.bytes;

                if (options.json) {
                    std::cout << "    {\"label\": \"" << roots[i].label << "\", \"path\": \""
                              << jsonPath(roots[i].path) << "\", \"files\": " << tally.files
                              << ", \"bytes\": " << tally.bytes << "}"
                              << (i + 1 < roots.size() ? "," : "") << "\n";
                } else {
                    std::cout << "  " << roots[i].label << "  " << tally.files << " files, "
                              << humanBytes(tally.bytes) << "\n"
                              << "    " << roots[i].description << "\n";
                }
            }

            if (options.json) {
                std::cout << "  ],\n  \"files\": " << totalFiles << ",\n  \"bytes\": " << totalBytes << "\n}\n";
            } else {
                std::cout << "\n" << totalFiles << " files, " << humanBytes(totalBytes)
                          << " available to any project - the editor lists them in Resources\n"
                          << "under the \"Engine\" origin, and `assets copy` delivers one into a project.\n";
            }
            return 0;
        }

        int runFind(const AssetsOptions& options) {
            const std::string pattern = lower(options.text);
            const std::vector<EngineAssetRoot> roots = selectedRoots(options);

            if (roots.empty()) {
                std::cerr << "assets find: no engine root" << (options.rootFilter.empty()
                          ? std::string() : " named " + options.rootFilter) << "\n";
                return 1;
            }

            struct Hit {
                std::string root;
                fs::path relative;
                unsigned long long bytes;
            };
            std::vector<Hit> hits;

            std::error_code ec;
            for (const EngineAssetRoot& root : roots) {
                if (!fs::is_directory(root.path, ec)) {
                    continue;
                }
                for (fs::recursive_directory_iterator it(root.path, fs::directory_options::skip_permission_denied, ec), end;
                     !ec && it != end; it.increment(ec)) {
                    if (!it->is_regular_file(ec)) {
                        continue;
                    }
                    const fs::path relative = fs::relative(it->path(), root.path, ec);
                    if (ec) {
                        ec.clear();
                        continue;
                    }
                    if (lower(relative.generic_string()).find(pattern) == std::string::npos) {
                        continue;
                    }
                    const unsigned long long size = static_cast<unsigned long long>(it->file_size(ec));
                    if (ec) {
                        ec.clear();
                    }
                    hits.push_back({root.label, relative, size});
                    if (static_cast<int>(hits.size()) >= options.limit) {
                        break;
                    }
                }
                if (static_cast<int>(hits.size()) >= options.limit) {
                    break;
                }
            }

            if (options.json) {
                std::cout << "{\n  \"pattern\": \"" << jsonPath(options.text) << "\", \"hits\": [\n";
                for (size_t i = 0; i < hits.size(); ++i) {
                    std::cout << "    {\"root\": \"" << hits[i].root << "\", \"path\": \""
                              << jsonPath(hits[i].relative) << "\", \"bytes\": " << hits[i].bytes << "}"
                              << (i + 1 < hits.size() ? "," : "");
                    std::cout << "\n";
                }
                std::cout << "  ],\n  \"count\": " << hits.size() << "\n}\n";
            } else if (hits.empty()) {
                std::cout << "no engine file matches \"" << options.text << "\" in "
                          << EngineAssets::engineDir().string() << "\n";
            } else {
                for (const Hit& hit : hits) {
                    std::cout << hit.root << "/" << hit.relative.generic_string() << "  "
                              << humanBytes(hit.bytes) << "\n";
                }
                std::cout << "\n" << hits.size() << " match" << (hits.size() == 1 ? "" : "es")
                          << (static_cast<int>(hits.size()) >= options.limit ? " (limit reached)" : "")
                          << " - copy one with `assets copy <root>/<path> --project <dir>`\n";
            }

            return hits.empty() ? 1 : 0;
        }

        // The same rule the components command uses: project.yaml's luaDir, relative to the
        // project, default ".", so `copy` and the Inspector point at one folder.
        fs::path resolveProjectLuaRoot(const fs::path& project, const std::string& luaDir, std::string& error) {
            std::string value = luaDir;
            const fs::path projectFile = project / "project.yaml";

            std::error_code ec;
            if (value.empty() && fs::exists(projectFile, ec)) {
                try {
                    const YAML::Node node = YAML::LoadFile(projectFile.string());
                    if (node["luaDir"]) {
                        value = trimmed(node["luaDir"].as<std::string>());
                    }
                } catch (const YAML::Exception& e) {
                    error = "cannot read project.yaml: " + e.msg;
                    return {};
                }
            }
            if (value.empty()) {
                value = ".";
            }

            const fs::path requested(value);
            return (requested.is_absolute() ? requested : project / requested).lexically_normal();
        }

        fs::path resolveProjectAssetsRoot(const fs::path& project, const std::string& assetsDir,
                                          std::string& error) {
            std::string value = assetsDir;
            const fs::path projectFile = project / "project.yaml";
            std::error_code ec;
            if (value.empty() && fs::exists(projectFile, ec)) {
                try {
                    const YAML::Node node = YAML::LoadFile(projectFile.string());
                    if (node["assetsDir"]) {
                        value = trimmed(node["assetsDir"].as<std::string>());
                    }
                } catch (const YAML::Exception& e) {
                    error = "cannot read project.yaml: " + e.msg;
                    return {};
                }
            }
            if (value.empty()) value = ".";
            const fs::path requested(value);
            return (requested.is_absolute() ? requested : project / requested).lexically_normal();
        }

        fs::path projectRootOf(const std::string& path) {
            fs::path project(path);
            if (project.filename() == "project.yaml") {
                project = project.parent_path();
            }
            return project.lexically_normal();
        }

        // One file into a folder that may already hold it: a project owns its copies, so
        // overwriting is opt-in and a skip is never an error.
        void copyOneFile(const fs::path& source, const fs::path& destinationDir,
                         const std::string& name, bool overwrite,
                         unsigned int& copied, unsigned int& skipped, unsigned int& failed) {
            std::error_code ec;
            const fs::path target = destinationDir / name;
            fs::create_directories(destinationDir, ec);

            if (fs::exists(target, ec) && !overwrite) {
                ++skipped;
                return;
            }

            ec.clear();
            if (fs::copy_file(source, target,
                              overwrite ? fs::copy_options::overwrite_existing : fs::copy_options::skip_existing,
                              ec) || (!ec && fs::exists(target))) {
                ++copied;
            } else {
                ++failed;
                std::cerr << "assets copy: " << target.string() << ": " << ec.message() << "\n";
            }
        }

        int runCopy(const AssetsOptions& options) {
            std::error_code ec;

            fs::path source(options.source);
            if (source.is_relative()) {
                source = EngineAssets::engineDir() / source;
            }
            source = source.lexically_normal();

            if (!fs::exists(source, ec)) {
                std::cerr << "assets copy: no such engine file: " << source.string() << "\n";
                return 2;
            }
            // The guard the browser enforces by drawing a read-only row: engine content is
            // a template here, never a working copy.
            if (!EngineAssets::isEnginePath(source)) {
                std::cerr << "assets copy: " << source.string()
                          << " is not engine content (under " << EngineAssets::engineDir().string() << ")\n";
                return 2;
            }

            const fs::path project = projectRootOf(options.projectPath);
            if (!fs::is_directory(project, ec)) {
                std::cerr << "assets copy: not a project directory: " << project.string() << "\n";
                return 2;
            }

            std::string error;
            const fs::path luaRoot = resolveProjectLuaRoot(project, options.luaDir, error);
            if (!error.empty()) {
                std::cerr << "assets copy: " << error << "\n";
                return 2;
            }

            const fs::path requestedAssets(options.assetsDir);
            const fs::path assetsRoot = requestedAssets.empty()
                ? project / "assets"
                : (requestedAssets.is_absolute() ? requestedAssets : project / requestedAssets);

            // The panel's own rule for where a copy lands: a ready-made script goes where the
            // component dialog looks, engine media goes under assets/engine/<root>.
            const fs::path destination = EngineAssets::projectCopyDestination(source, assetsRoot, luaRoot);
            if (destination.empty()) {
                std::cerr << "assets copy: cannot resolve a destination inside " << project.string() << "\n";
                return 2;
            }

            unsigned int copied = 0;
            unsigned int skipped = 0;
            unsigned int failed = 0;

            if (fs::is_directory(source, ec)) {
                const fs::path targetRoot = destination / source.filename();
                for (fs::recursive_directory_iterator it(source, ec), end; !ec && it != end; it.increment(ec)) {
                    if (!it->is_regular_file(ec)) {
                        continue;
                    }
                    const fs::path relative = fs::relative(it->path(), source, ec);
                    const fs::path into = targetRoot / relative.parent_path();
                    copyOneFile(it->path(), into, relative.filename().string(), options.overwrite,
                                copied, skipped, failed);
                }
            } else {
                copyOneFile(source, destination, source.filename().string(), options.overwrite,
                            copied, skipped, failed);
            }

            std::cout << copied << " copied, " << skipped << " already there, " << failed << " failed, into "
                      << destination.string() << "\n";

            const EngineAssetRoot* root = EngineAssets::rootContaining(source);
            if (root && root->label == "components" && source.extension() == ".lua") {
                std::cout << "Attach it in the editor: Inspector -> Scripts -> Add Script -> Lua Script, "
                             "then pick Components/"
                          << source.filename().string() << " as its Source File.\n";
            } else if (root && root->label == "shaderlib") {
                std::cout << "Fork it into the project's shader folder before editing it; the engine "
                             "copy is never compiled from.\n";
            }

            if (failed > 0) {
                return 1;
            }
            return copied + skipped > 0 ? 0 : 1;
        }

    }

    int runAssetsCommand(int argc, char** argv, const char* executableName) {
        const std::string commandName = commandBaseName(executableName);

        AssetsOptions options;
        std::string error;
        if (!parseAssetsArgs(argc, argv, options, error)) {
            std::cerr << commandName << " assets: " << error << "\n\n";
            printUsage(commandName);
            return 2;
        }
        if (options.help) {
            printUsage(commandName);
            return 0;
        }

        if (options.action == AssetsOptions::Action::Verify) {
            return runAssetVerify(options);
        }
        if (options.action == AssetsOptions::Action::Index || options.action == AssetsOptions::Action::Doctor) {
            return runAssetDatabase(options, options.action == AssetsOptions::Action::Doctor);
        }
        if (options.action == AssetsOptions::Action::Import) {
            return runAssetImport(options);
        }

        // The resolver caches its answer for the whole process, so an explicit --engine-dir
        // has to be handed to it before the first query instead of being a second truth.
        if (!options.engineDir.empty()) {
            EngineAssets::overrideEngineDir(options.engineDir);
        }
        if (!EngineAssets::hasContent()) {
            std::cerr << commandName << " assets: the engine ships no content at "
                      << EngineAssets::engineDir().string() << "\n"
                      << "Run the `copy_quantum_editor_engine` target, or pass --engine-dir.\n";
            return 1;
        }

        switch (options.action) {
            case AssetsOptions::Action::List: return runList(options);
            case AssetsOptions::Action::Find: return runFind(options);
            case AssetsOptions::Action::Copy: return runCopy(options);
            default: break;
        }

        printUsage(commandName);
        return 2;
    }

}
