// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "util/AssetDatabase.h"

#include "json.hpp"
#include "util/SHA1.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>
#include <vector>

namespace quantum::editor {

    namespace {

        using Json = nlohmann::json;

        constexpr int kFormatVersion = 2;
        constexpr int kDependencyScannerVersion = 2;
        constexpr const char* kMetadataMagic = "QAMI1";
        constexpr size_t kMaxTextDependencyBytes = 8 * 1024 * 1024;
        constexpr size_t kMaxEmbeddedJsonBytes = 8 * 1024 * 1024;

        std::string hexEncode(const std::string& value) {
            static constexpr char digits[] = "0123456789abcdef";
            std::string result;
            result.reserve(value.size() * 2);
            for (unsigned char byte : value) {
                result.push_back(digits[byte >> 4]);
                result.push_back(digits[byte & 15]);
            }
            return result;
        }

        std::string lower(std::string value) {
            std::transform(value.begin(), value.end(), value.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return value;
        }

        std::string normalizedPath(const fs::path& path) {
            std::string value = path.generic_string();
            while (!value.empty() && value.front() == '.') {
                if (value.size() == 1 || value[1] != '/') break;
                value.erase(0, 2);
            }
            return value;
        }

        std::string kindOf(const fs::path& path) {
            const std::string extension = lower(path.extension().string());
            if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
                extension == ".bmp" || extension == ".tga" || extension == ".hdr" ||
                extension == ".exr") return "texture";
            if (extension == ".obj" || extension == ".fbx" || extension == ".glb" ||
                extension == ".gltf" || extension == ".mesh") return "model";
            if (extension == ".wav" || extension == ".ogg" || extension == ".mp3" ||
                extension == ".flac") return "audio";
            if (extension == ".ttf" || extension == ".otf") return "font";
            if (extension == ".lua" || extension == ".cpp" || extension == ".h" ||
                extension == ".hpp") return "script";
            if (extension == ".scene" || extension == ".yaml" || extension == ".yml") return "scene";
            if (extension == ".material" || extension == ".mat") return "material";
            if (extension == ".vert" || extension == ".frag" || extension == ".glsl" ||
                extension == ".shader") return "shader";
            if (extension == ".graph" || extension == ".gdat") return "visual-script";
            return "asset";
        }

        bool isTextExtension(const std::string& extension) {
            static const std::set<std::string> extensions = {
                ".c", ".cc", ".cpp", ".frag", ".gdat", ".glsl", ".graph", ".h", ".hpp",
                ".json", ".lua", ".mat", ".material", ".scene", ".shader", ".txt", ".vert",
                ".yaml", ".yml"
            };
            return extensions.find(lower(extension)) != extensions.end();
        }

        bool looksLikeReference(const std::string& value) {
            if (value.empty() || value.size() > 1024 || value[0] == '#') return false;
            const std::string lowered = lower(value);
            if (lowered.find("://") != std::string::npos || lowered.rfind("http:", 0) == 0 ||
                lowered.rfind("https:", 0) == 0) return false;
            if (lowered == "true" || lowered == "false" || lowered == "null") return false;
            static const std::set<std::string> extensions = {
                ".png", ".jpg", ".jpeg", ".bmp", ".tga", ".hdr", ".exr", ".obj", ".fbx",
                ".glb", ".gltf", ".bin", ".wav", ".ogg", ".mp3", ".flac", ".lua", ".scene",
                ".yaml", ".yml", ".json", ".material", ".mat", ".mtl", ".vert", ".frag",
                ".glsl", ".shader", ".graph", ".gdat", ".ttf", ".otf"
            };
            const size_t dot = lowered.find_last_of('.');
            if (dot == std::string::npos || extensions.find(lowered.substr(dot)) == extensions.end()) {
                return false;
            }
            return value.find('/') != std::string::npos || value.find('\\') != std::string::npos ||
                   dot > 0;
        }

        void trimToken(std::string& token) {
            while (!token.empty() && (token.front() == '"' || token.front() == '\'' ||
                                      token.front() == ',' || token.front() == ';')) token.erase(0, 1);
            while (!token.empty() && (token.back() == '"' || token.back() == '\'' ||
                                      token.back() == ',' || token.back() == ';')) token.pop_back();
            std::replace(token.begin(), token.end(), '\\', '/');
        }

        std::vector<std::string> dependencyCandidates(const std::string& text) {
            std::set<std::string> unique;
            auto add = [&](std::string token) {
                trimToken(token);
                if (looksLikeReference(token)) unique.insert(token);
            };

            for (size_t i = 0; i < text.size();) {
                if (text[i] == '"' || text[i] == '\'') {
                    const char quote = text[i++];
                    const size_t begin = i;
                    while (i < text.size() && text[i] != quote) ++i;
                    add(text.substr(begin, i - begin));
                    if (i < text.size()) ++i;
                    continue;
                }
                const size_t begin = i;
                while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i])) &&
                       std::string(",;()[]{}<>:=\"'\r\n").find(text[i]) == std::string::npos) ++i;
                if (i > begin) add(text.substr(begin, i - begin));
                else ++i;
            }
            return { unique.begin(), unique.end() };
        }

        bool makeRelative(const fs::path& projectDir, const fs::path& path, std::string& out) {
            std::error_code ec;
            const fs::path absoluteProject = fs::weakly_canonical(projectDir, ec);
            if (ec) return false;
            const fs::path absolutePath = fs::weakly_canonical(path, ec);
            if (ec) return false;
            const fs::path relative = fs::relative(absolutePath, absoluteProject, ec);
            if (ec || relative.empty() || relative.is_absolute()) return false;
            const std::string value = normalizedPath(relative);
            if (value.empty() || value == ".." || value.rfind("../", 0) == 0) return false;
            out = value;
            return true;
        }

        std::string resolveDependency(const fs::path& projectDir, const fs::path& source,
                                      const std::string& raw, const std::set<std::string>& files) {
            std::string candidate = raw;
            if (candidate.rfind("assets/", 0) == 0 || candidate.rfind("lua/", 0) == 0 ||
                candidate.rfind("scenes/", 0) == 0 || candidate.rfind("scripts/", 0) == 0) {
                const std::string normalized = normalizedPath(fs::path(candidate));
                if (files.find(normalized) != files.end()) return normalized;
            }

            std::vector<fs::path> attempts = {
                source.parent_path() / fs::path(candidate),
                projectDir / "assets" / fs::path(candidate),
                projectDir / "lua" / fs::path(candidate),
                projectDir / fs::path(candidate)
            };
            for (const fs::path& attempt : attempts) {
                std::string relative;
                if (makeRelative(projectDir, attempt, relative) && files.find(relative) != files.end()) {
                    return relative;
                }
            }
            return std::string();
        }

        bool readFile(const fs::path& path, std::string& out, std::string& error) {
            std::error_code ec;
            const uintmax_t size = fs::file_size(path, ec);
            if (ec) {
                error = ec.message();
                return false;
            }
            if (size > kMaxTextDependencyBytes) return false;
            std::ifstream stream(path, std::ios::binary);
            if (!stream) {
                error = "cannot open file";
                return false;
            }
            out.resize(static_cast<size_t>(size));
            if (!out.empty() && !stream.read(out.data(), static_cast<std::streamsize>(out.size()))) {
                error = "cannot read file";
                return false;
            }
            return true;
        }

        bool hashFile(const fs::path& path, std::string& out, std::string& error) {
            std::ifstream stream(path, std::ios::binary);
            if (!stream) {
                error = "cannot open file";
                return false;
            }

            SHA1::Context context;
            std::vector<char> chunk(1 << 20);
            while (stream) {
                stream.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
                const std::streamsize count = stream.gcount();
                if (count > 0) context.update(chunk.data(), static_cast<size_t>(count));
            }
            if (stream.bad()) {
                error = "cannot read file";
                return false;
            }
            out = context.final();
            return true;
        }

        std::string computeManifest(const std::map<std::string, AssetRecord>& records) {
            std::ostringstream input;
            for (const auto& pair : records) {
                const AssetRecord& record = pair.second;
                input << record.path << ' ' << record.sha1 << '\n';
                for (const auto& metadata : record.metadata) {
                    input << "  @ " << metadata.first << '=' << metadata.second << '\n';
                }
                for (const std::string& dependency : record.dependencies) {
                    input << "  -> " << dependency << '\n';
                }
                for (const std::string& missing : record.missingDependencies) {
                    input << "  ! " << missing << '\n';
                }
            }
            return SHA1::hash(input.str());
        }

        AssetRecord recordFromJson(const Json& value) {
            AssetRecord record;
            record.path = value.value("path", "");
            record.kind = value.value("kind", "asset");
            record.size = value.value("size", uint64_t(0));
            record.mtimeRaw = value.value("mtime", int64_t(0));
            record.sha1 = value.value("sha1", "");
            if (value.contains("metadata") && value.at("metadata").is_object()) {
                for (const auto& item : value.at("metadata").items()) {
                    if (item.value().is_string()) record.metadata[item.key()] = item.value().get<std::string>();
                }
            }
            record.dependencies = value.value("dependencies", std::vector<std::string>());
            record.missingDependencies = value.value("missing", std::vector<std::string>());
            record.error = value.value("error", "");
            return record;
        }

        Json recordToJson(const AssetRecord& record) {
            Json value = {
                {"path", record.path},
                {"kind", record.kind},
                {"size", record.size},
                {"mtime", record.mtimeRaw},
                {"sha1", record.sha1},
                {"dependencies", record.dependencies}
            };
            if (!record.metadata.empty()) value["metadata"] = record.metadata;
            if (!record.missingDependencies.empty()) value["missing"] = record.missingDependencies;
            if (!record.error.empty()) value["error"] = record.error;
            return value;
        }

        bool writeMetadataSidecar(const fs::path& path,
                                  const std::map<std::string, AssetRecord>& records,
                                  std::string& error) {
            const fs::path temporary = path.parent_path() / "asset-metadata.v1.tmp";
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            if (!stream) {
                error = "cannot write " + temporary.generic_string();
                return false;
            }
            stream << kMetadataMagic << '\n';
            for (const auto& pair : records) {
                for (const auto& metadata : pair.second.metadata) {
                    if (metadata.first == "dependencyScanner") continue;
                    stream << "entry\t" << hexEncode(pair.first) << '\t'
                           << hexEncode(metadata.first) << '\t'
                           << hexEncode(metadata.second) << '\n';
                }
            }
            stream.close();
            if (!stream) {
                error = "cannot flush " + temporary.generic_string();
                std::error_code ec;
                fs::remove(temporary, ec);
                return false;
            }

            std::error_code ec;
            fs::rename(temporary, path, ec);
            if (ec) {
                ec.clear();
                fs::remove(path, ec);
                ec.clear();
                fs::rename(temporary, path, ec);
            }
            if (ec) {
                error = "cannot replace " + path.generic_string() + ": " + ec.message();
                fs::remove(temporary, ec);
                return false;
            }
            return true;
        }

        uint32_t readLittleU32(const unsigned char* bytes) {
            return static_cast<uint32_t>(bytes[0]) |
                   (static_cast<uint32_t>(bytes[1]) << 8) |
                   (static_cast<uint32_t>(bytes[2]) << 16) |
                   (static_cast<uint32_t>(bytes[3]) << 24);
        }

        std::string decodeGltfUri(std::string value) {
            if (value.empty() || lower(value).rfind("data:", 0) == 0 ||
                value.find("://") != std::string::npos) return {};

            const size_t fragment = value.find('#');
            if (fragment != std::string::npos) value.erase(fragment);

            std::string decoded;
            decoded.reserve(value.size());
            auto hex = [](char character) -> int {
                if (character >= '0' && character <= '9') return character - '0';
                if (character >= 'a' && character <= 'f') return character - 'a' + 10;
                if (character >= 'A' && character <= 'F') return character - 'A' + 10;
                return -1;
            };
            for (size_t index = 0; index < value.size(); ++index) {
                if (value[index] == '%' && index + 2 < value.size()) {
                    const int high = hex(value[index + 1]);
                    const int low = hex(value[index + 2]);
                    if (high >= 0 && low >= 0) {
                        decoded.push_back(static_cast<char>((high << 4) | low));
                        index += 2;
                        continue;
                    }
                }
                decoded.push_back(value[index]);
            }
            std::replace(decoded.begin(), decoded.end(), '\\', '/');
            return decoded;
        }

        bool readGltfDocument(const fs::path& path, Json& document) {
            document = Json();
            const std::string extension = lower(path.extension().string());
            std::string jsonText;
            if (extension == ".gltf") {
                std::string error;
                if (!readFile(path, jsonText, error)) return false;
            } else if (extension == ".glb") {
                std::ifstream file(path, std::ios::binary);
                unsigned char header[20] = {};
                if (!file.read(reinterpret_cast<char*>(header), sizeof(header)) ||
                    header[0] != 'g' || header[1] != 'l' || header[2] != 'T' || header[3] != 'F' ||
                    readLittleU32(header + 4) != 2 || readLittleU32(header + 16) != 0x4E4F534A) {
                    return false;
                }
                const uint32_t jsonSize = readLittleU32(header + 12);
                if (jsonSize == 0 || jsonSize > kMaxEmbeddedJsonBytes) return false;
                jsonText.resize(jsonSize);
                if (!file.read(jsonText.data(), static_cast<std::streamsize>(jsonText.size()))) return false;
            } else {
                return false;
            }

            while (!jsonText.empty() && (jsonText.back() == '\0' || jsonText.back() == ' ' ||
                                         jsonText.back() == '\t' || jsonText.back() == '\r' ||
                                         jsonText.back() == '\n')) {
                jsonText.pop_back();
            }
            document = Json::parse(jsonText, nullptr, false);
            return document.is_object();
        }

        void collectGltfUris(const Json& value, std::set<std::string>& references) {
            if (value.is_object()) {
                for (const auto& item : value.items()) {
                    if (item.key() == "uri" && item.value().is_string()) {
                        const std::string decoded = decodeGltfUri(item.value().get<std::string>());
                        if (!decoded.empty()) references.insert(decoded);
                    } else {
                        collectGltfUris(item.value(), references);
                    }
                }
            } else if (value.is_array()) {
                for (const Json& item : value) collectGltfUris(item, references);
            }
        }

        void collectFbxReferences(const fs::path& path, std::set<std::string>& references) {
            std::string text;
            std::string error;
            if (!readFile(path, text, error)) return;

            std::ifstream headerFile(path, std::ios::binary);
            unsigned char header[21] = {};
            const bool binary = headerFile.read(reinterpret_cast<char*>(header), sizeof(header)) &&
                std::memcmp(header, "Kaydara FBX Binary  ", 20) == 0 && header[20] == 0;
            if (!binary) {
                for (const std::string& candidate : dependencyCandidates(text)) references.insert(candidate);
                return;
            }

            // Binary FBX stores texture filenames in printable property strings. Scanning
            // those bounded runs is conservative and avoids treating arbitrary binary data
            // as a project dependency.
            size_t begin = 0;
            while (begin < text.size()) {
                while (begin < text.size() &&
                       (static_cast<unsigned char>(text[begin]) < 32 ||
                        static_cast<unsigned char>(text[begin]) > 126)) ++begin;
                const size_t end = begin;
                while (begin < text.size() && static_cast<unsigned char>(text[begin]) >= 32 &&
                       static_cast<unsigned char>(text[begin]) <= 126) ++begin;
                if (begin > end) {
                    const std::string token = text.substr(end, begin - end);
                    if (looksLikeReference(token)) references.insert(token);
                }
            }
        }

        std::vector<std::string> dependencyCandidatesForAsset(const fs::path& path) {
            std::set<std::string> unique;
            const std::string extension = lower(path.extension().string());
            if (extension == ".gltf" || extension == ".glb") {
                Json document;
                if (readGltfDocument(path, document)) collectGltfUris(document, unique);
            } else if (extension == ".fbx") {
                collectFbxReferences(path, unique);
            } else if (isTextExtension(path.extension().string())) {
                std::string text;
                std::string error;
                if (readFile(path, text, error)) {
                    for (const std::string& candidate : dependencyCandidates(text)) unique.insert(candidate);
                }
            }
            return {unique.begin(), unique.end()};
        }

        uint16_t readLittleU16(const unsigned char* bytes) {
            return static_cast<uint16_t>(bytes[0]) |
                   static_cast<uint16_t>(bytes[1] << 8);
        }

        uint32_t readBigU32(const unsigned char* bytes) {
            return (static_cast<uint32_t>(bytes[0]) << 24) |
                   (static_cast<uint32_t>(bytes[1]) << 16) |
                   (static_cast<uint32_t>(bytes[2]) << 8) |
                   static_cast<uint32_t>(bytes[3]);
        }

        void addGltfMetadata(const Json& document, std::map<std::string, std::string>& metadata) {
            if (!document.is_object()) return;
            const auto addArrayCount = [&document, &metadata](const char* key, const char* metadataKey) {
                if (document.contains(key) && document.at(key).is_array()) {
                    metadata[metadataKey] = std::to_string(document.at(key).size());
                }
            };
            addArrayCount("accessors", "accessorCount");
            addArrayCount("animations", "animationCount");
            addArrayCount("images", "imageCount");
            addArrayCount("materials", "materialCount");
            addArrayCount("meshes", "meshCount");
            addArrayCount("nodes", "nodeCount");
            addArrayCount("scenes", "sceneCount");
            addArrayCount("skins", "skinCount");
            addArrayCount("textures", "textureCount");
            addArrayCount("buffers", "bufferCount");

            size_t primitiveCount = 0;
            size_t morphTargetCount = 0;
            const Json meshes = document.value("meshes", Json::array());
            if (meshes.is_array()) {
                for (const Json& mesh : meshes) {
                    if (!mesh.is_object()) continue;
                    const Json primitives = mesh.value("primitives", Json::array());
                    if (!primitives.is_array()) continue;
                    primitiveCount += primitives.size();
                    for (const Json& primitive : primitives) {
                        if (!primitive.is_object()) continue;
                        const Json targets = primitive.value("targets", Json::array());
                        if (targets.is_array()) morphTargetCount += targets.size();
                    }
                }
            }
            metadata["primitiveCount"] = std::to_string(primitiveCount);
            metadata["morphTargetCount"] = std::to_string(morphTargetCount);

            const Json skins = document.value("skins", Json::array());
            metadata["hasSkin"] = skins.is_array() && !skins.empty() ? "true" : "false";

            size_t materialTextureCount = 0;
            const Json materials = document.value("materials", Json::array());
            if (materials.is_array()) {
                const auto countTexture = [&materialTextureCount](const Json& value,
                                                                    const char* key) {
                    if (!value.is_object()) return;
                    const Json texture = value.value(key, Json());
                    if (texture.is_object() && texture.contains("index")
                            && texture.at("index").is_number_integer()) {
                        ++materialTextureCount;
                    }
                };
                for (const Json& material : materials) {
                    if (!material.is_object()) continue;
                    countTexture(material, "normalTexture");
                    countTexture(material, "occlusionTexture");
                    countTexture(material, "emissiveTexture");
                    const Json pbr = material.value("pbrMetallicRoughness", Json());
                    countTexture(pbr, "baseColorTexture");
                    countTexture(pbr, "metallicRoughnessTexture");
                }
            }
            metadata["materialTextureCount"] = std::to_string(materialTextureCount);

            size_t extensionCount = 0;
            const Json usedExtensions = document.value("extensionsUsed", Json::array());
            const Json requiredExtensions = document.value("extensionsRequired", Json::array());
            if (usedExtensions.is_array()) extensionCount += usedExtensions.size();
            if (requiredExtensions.is_array()) extensionCount += requiredExtensions.size();
            metadata["extensionCount"] = std::to_string(extensionCount);

            const Json asset = document.value("asset", Json::object());
            if (!asset.is_object()) return;
            if (asset.contains("generator") && asset.at("generator").is_string()) {
                metadata["generator"] = asset.at("generator").get<std::string>();
            }
            const Json extras = asset.value("extras", Json::object());
            const Json import = extras.is_object() ? extras.value("quantumFbxImport", Json()) : Json();
            if (!import.is_object()) return;
            metadata["importer"] = "fbx";
            if (import.contains("version") && import.at("version").is_number_integer()) {
                metadata["importerVersion"] = std::to_string(import.at("version").get<int>());
            }
            const Json files = import.value("files", Json::array());
            if (files.is_array()) {
                metadata["sourceCount"] = std::to_string(files.size());
                size_t linkedSourceCount = 0;
                for (const Json& file : files) {
                    if (file.is_object() && file.value("linked", false)) ++linkedSourceCount;
                }
                metadata["linkedSourceCount"] = std::to_string(linkedSourceCount);
                if (!files.empty() && files.front().is_object()) {
                    const Json& source = files.front();
                    if (source.contains("path") && source.at("path").is_string()) {
                        metadata["sourcePath"] = source.at("path").get<std::string>();
                    }
                    if (source.contains("hash") && source.at("hash").is_string()) {
                        metadata["sourceHash"] = source.at("hash").get<std::string>();
                    }
                }
            }
            const Json textures = import.value("textures", Json::array());
            if (textures.is_array()) {
                metadata["textureCount"] = std::to_string(textures.size());
                size_t resolvedTextureCount = 0;
                size_t missingTextureCount = 0;
                for (const Json& texture : textures) {
                    const Json path = texture.is_object() ? texture.value("path", Json()) : Json();
                    if (!path.is_string() || path.get<std::string>().empty()) {
                        ++missingTextureCount;
                    } else {
                        ++resolvedTextureCount;
                    }
                }
                metadata["resolvedTextureCount"] = std::to_string(resolvedTextureCount);
                metadata["missingTextureCount"] = std::to_string(missingTextureCount);
            }
        }

        void addFbxMetadata(const fs::path& path, std::map<std::string, std::string>& metadata) {
            std::vector<unsigned char> prefix(27, 0);
            std::ifstream file(path, std::ios::binary);
            file.read(reinterpret_cast<char*>(prefix.data()), static_cast<std::streamsize>(prefix.size()));
            const bool binary = file.gcount() >= 21
                && std::memcmp(prefix.data(), "Kaydara FBX Binary  ", 20) == 0
                && prefix[20] == 0;
            metadata["format"] = "fbx";
            metadata["encoding"] = binary ? "binary" : "ascii";
            if (binary && file.gcount() >= 27) {
                metadata["version"] = std::to_string(readLittleU32(prefix.data() + 23));
                return;
            }

            std::string text;
            std::string error;
            if (!readFile(path, text, error)) return;
            const std::string marker = "FBXVersion:";
            const size_t markerPosition = text.find(marker);
            if (markerPosition == std::string::npos) return;
            size_t begin = markerPosition + marker.size();
            while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
            size_t end = begin;
            while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end]))) ++end;
            if (end > begin) metadata["version"] = text.substr(begin, end - begin);
        }

        bool readJpegDimensions(const fs::path& path, uint32_t& width, uint32_t& height) {
            std::ifstream file(path, std::ios::binary);
            unsigned char signature[2] = {};
            if (!file.read(reinterpret_cast<char*>(signature), sizeof(signature)) ||
                signature[0] != 0xff || signature[1] != 0xd8) return false;

            for (size_t markerCount = 0; markerCount < 256 && file; ++markerCount) {
                unsigned char byte = 0;
                do {
                    if (!file.read(reinterpret_cast<char*>(&byte), 1)) return false;
                } while (byte != 0xff);
                do {
                    if (!file.read(reinterpret_cast<char*>(&byte), 1)) return false;
                } while (byte == 0xff);
                if (byte == 0xd8 || byte == 0xd9) continue;
                if (byte == 0x01 || (byte >= 0xd0 && byte <= 0xd7)) continue;

                unsigned char lengthBytes[2] = {};
                if (!file.read(reinterpret_cast<char*>(lengthBytes), sizeof(lengthBytes))) return false;
                const uint16_t length = static_cast<uint16_t>((lengthBytes[0] << 8) | lengthBytes[1]);
                if (length < 2) return false;

                const bool hasDimensions = (byte >= 0xc0 && byte <= 0xc3) ||
                    (byte >= 0xc5 && byte <= 0xc7) || (byte >= 0xc9 && byte <= 0xcb) ||
                    (byte >= 0xcd && byte <= 0xcf);
                if (hasDimensions) {
                    unsigned char dimensions[5] = {};
                    if (length < 7 || !file.read(reinterpret_cast<char*>(dimensions), sizeof(dimensions))) return false;
                    height = static_cast<uint32_t>((dimensions[1] << 8) | dimensions[2]);
                    width = static_cast<uint32_t>((dimensions[3] << 8) | dimensions[4]);
                    return width != 0 && height != 0;
                }
                file.seekg(static_cast<std::streamoff>(length - 2), std::ios::cur);
            }
            return false;
        }

        void addWavMetadata(const fs::path& path, std::map<std::string, std::string>& metadata) {
            std::ifstream file(path, std::ios::binary);
            unsigned char header[12] = {};
            if (!file.read(reinterpret_cast<char*>(header), sizeof(header)) ||
                std::memcmp(header, "RIFF", 4) != 0 || std::memcmp(header + 8, "WAVE", 4) != 0) return;

            metadata["format"] = "wav";
            uint32_t byteRate = 0;
            uint32_t dataBytes = 0;
            for (size_t chunkCount = 0; chunkCount < 64 && file; ++chunkCount) {
                unsigned char chunkHeader[8] = {};
                if (!file.read(reinterpret_cast<char*>(chunkHeader), sizeof(chunkHeader))) break;
                const uint32_t chunkSize = readLittleU32(chunkHeader + 4);
                if (std::memcmp(chunkHeader, "fmt ", 4) == 0 && chunkSize >= 16 && chunkSize <= 4096) {
                    std::vector<unsigned char> chunk(chunkSize);
                    if (!file.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()))) break;
                    metadata["channels"] = std::to_string(readLittleU16(chunk.data() + 2));
                    metadata["sampleRate"] = std::to_string(readLittleU32(chunk.data() + 4));
                    byteRate = readLittleU32(chunk.data() + 8);
                    metadata["bitsPerSample"] = std::to_string(readLittleU16(chunk.data() + 14));
                } else if (std::memcmp(chunkHeader, "data", 4) == 0) {
                    dataBytes = chunkSize;
                    file.seekg(static_cast<std::streamoff>(chunkSize), std::ios::cur);
                } else {
                    file.seekg(static_cast<std::streamoff>(chunkSize), std::ios::cur);
                }
                if ((chunkSize & 1u) != 0) file.seekg(1, std::ios::cur);
            }
            if (byteRate != 0 && dataBytes != 0) {
                metadata["durationSeconds"] = std::to_string(static_cast<double>(dataBytes) /
                                                               static_cast<double>(byteRate));
            }
        }

        void addObjMetadata(const fs::path& path, std::map<std::string, std::string>& metadata) {
            std::string text;
            std::string error;
            if (!readFile(path, text, error)) return;
            uint64_t vertices = 0;
            uint64_t texcoords = 0;
            uint64_t normals = 0;
            uint64_t faces = 0;
            size_t begin = 0;
            while (begin < text.size()) {
                const size_t end = text.find('\n', begin);
                const size_t lineEnd = end == std::string::npos ? text.size() : end;
                size_t cursor = begin;
                while (cursor < lineEnd && (text[cursor] == ' ' || text[cursor] == '\t')) ++cursor;
                const size_t foundTokenEnd = text.find_first_of(" \t\r", cursor);
                const size_t tokenEnd = foundTokenEnd == std::string::npos || foundTokenEnd > lineEnd
                    ? lineEnd : foundTokenEnd;
                const std::string token = text.substr(cursor, tokenEnd - cursor);
                if (token == "v") ++vertices;
                else if (token == "vt") ++texcoords;
                else if (token == "vn") ++normals;
                else if (token == "f") ++faces;
                begin = end == std::string::npos ? text.size() : end + 1;
            }
            metadata["format"] = "obj";
            metadata["vertexCount"] = std::to_string(vertices);
            metadata["texcoordCount"] = std::to_string(texcoords);
            metadata["normalCount"] = std::to_string(normals);
            metadata["faceCount"] = std::to_string(faces);
        }

        std::map<std::string, std::string> metadataFor(const fs::path& path) {
            std::map<std::string, std::string> metadata;
            const std::string extension = lower(path.extension().string());
            if (extension == ".png") {
                std::ifstream file(path, std::ios::binary);
                unsigned char header[24] = {};
                if (file.read(reinterpret_cast<char*>(header), sizeof(header)) &&
                    header[0] == 0x89 && header[1] == 'P' && header[2] == 'N' && header[3] == 'G' &&
                    header[12] == 'I' && header[13] == 'H' && header[14] == 'D' && header[15] == 'R') {
                    metadata["format"] = "png";
                    metadata["width"] = std::to_string(readBigU32(header + 16));
                    metadata["height"] = std::to_string(readBigU32(header + 20));
                }
            } else if (extension == ".jpg" || extension == ".jpeg") {
                uint32_t width = 0;
                uint32_t height = 0;
                if (readJpegDimensions(path, width, height)) {
                    metadata["format"] = "jpeg";
                    metadata["width"] = std::to_string(width);
                    metadata["height"] = std::to_string(height);
                }
            } else if (extension == ".wav") {
                addWavMetadata(path, metadata);
            } else if (extension == ".obj") {
                addObjMetadata(path, metadata);
            } else if (extension == ".glb") {
                std::ifstream file(path, std::ios::binary);
                unsigned char header[20] = {};
                if (file.read(reinterpret_cast<char*>(header), sizeof(header)) &&
                    header[0] == 'g' && header[1] == 'l' && header[2] == 'T' && header[3] == 'F' &&
                    readLittleU32(header + 4) == 2 && readLittleU32(header + 16) == 0x4E4F534A) {
                    const uint32_t jsonSize = readLittleU32(header + 12);
                    if (jsonSize > 0 && jsonSize <= kMaxEmbeddedJsonBytes) {
                        std::string jsonText(jsonSize, '\0');
                        if (file.read(jsonText.data(), static_cast<std::streamsize>(jsonText.size()))) {
                            const Json document = Json::parse(jsonText, nullptr, false);
                            if (!document.is_discarded()) addGltfMetadata(document, metadata);
                        }
                    }
                }
                if (metadata.find("format") == metadata.end()) metadata["format"] = "glb";
            } else if (extension == ".gltf") {
                std::string text;
                std::string ignored;
                if (readFile(path, text, ignored) && text.size() <= kMaxEmbeddedJsonBytes) {
                    const Json document = Json::parse(text, nullptr, false);
                    if (!document.is_discarded()) addGltfMetadata(document, metadata);
                }
                metadata["format"] = "gltf";
            } else if (extension == ".fbx") {
                addFbxMetadata(path, metadata);
            }
            return metadata;
        }

    }

    AssetDatabase::AssetDatabase(fs::path projectDir)
        : projectDir_(fs::absolute(std::move(projectDir)).lexically_normal()),
          databasePath_(databasePath(projectDir_)) {}

    fs::path AssetDatabase::databasePath(const fs::path& projectDir) {
        return projectDir / ".quantum" / "assetdb.json";
    }

    bool AssetDatabase::readExisting(std::map<std::string, AssetRecord>& out, std::string& error) const {
        out.clear();
        std::ifstream stream(databasePath_, std::ios::binary);
        if (!stream) return true;
        std::stringstream buffer;
        buffer << stream.rdbuf();
        Json document = Json::parse(buffer.str(), nullptr, false);
        const int format = document.is_object() ? document.value("format", 0) : 0;
        if (document.is_discarded() || !document.is_object() || (format != 1 && format != kFormatVersion)) {
            error = "invalid or unsupported asset database format";
            return false;
        }
        const Json files = document.value("files", Json::array());
        if (!files.is_array()) {
            error = "asset database files is not an array";
            return false;
        }
        for (const Json& value : files) {
            AssetRecord record = recordFromJson(value);
            if (!record.path.empty()) out[record.path] = std::move(record);
        }
        return true;
    }

    bool AssetDatabase::load(std::string& error) {
        std::map<std::string, AssetRecord> loaded;
        if (!readExisting(loaded, error)) return false;
        records_ = std::move(loaded);
        std::ifstream stream(databasePath_, std::ios::binary);
        if (stream) {
            std::stringstream buffer;
            buffer << stream.rdbuf();
            Json document = Json::parse(buffer.str(), nullptr, false);
            if (!document.is_discarded()) manifestSha1_ = document.value("manifestSha1", "");
        }
        return true;
    }

    bool AssetDatabase::write(std::string& error) const {
        std::error_code ec;
        fs::create_directories(databasePath_.parent_path(), ec);
        if (ec) {
            error = "cannot create .quantum: " + ec.message();
            return false;
        }
        Json document = {
            {"format", kFormatVersion},
            {"engine", "quantum"},
            {"manifestSha1", manifestSha1_},
            {"files", Json::array()}
        };
        for (const auto& pair : records_) document["files"].push_back(recordToJson(pair.second));

        const fs::path temporary = databasePath_.parent_path() / "assetdb.json.tmp";
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "cannot write " + temporary.generic_string();
            return false;
        }
        stream << document.dump(2) << '\n';
        stream.close();
        if (!stream) {
            error = "cannot flush " + temporary.generic_string();
            fs::remove(temporary, ec);
            return false;
        }
        fs::rename(temporary, databasePath_, ec);
        if (ec) {
            ec.clear();
            fs::remove(databasePath_, ec);
            ec.clear();
            fs::rename(temporary, databasePath_, ec);
        }
        if (ec) {
            error = "cannot replace " + databasePath_.generic_string() + ": " + ec.message();
            fs::remove(temporary, ec);
            return false;
        }
        return writeMetadataSidecar(databasePath_.parent_path() / "asset-metadata.v1",
                                    records_, error);
    }

    bool AssetDatabase::update(bool force, AssetDatabaseReport& report, std::string& error) {
        report = AssetDatabaseReport();
        std::map<std::string, AssetRecord> previous;
        if (!readExisting(previous, error)) {
            previous.clear();
            error.clear();
        }
        std::string previousManifest;
        {
            std::ifstream stream(databasePath_, std::ios::binary);
            if (stream) {
                std::stringstream buffer;
                buffer << stream.rdbuf();
                Json document = Json::parse(buffer.str(), nullptr, false);
                if (!document.is_discarded()) previousManifest = document.value("manifestSha1", "");
            }
        }

        std::map<std::string, AssetRecord> fresh;
        std::set<std::string> paths;
        std::error_code ec;
        for (const char* rootName : {"assets", "lua", "scenes", "scripts"}) {
            const fs::path root = projectDir_ / rootName;
            if (!fs::is_directory(root, ec)) continue;
            fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
            for (fs::recursive_directory_iterator end; !ec && it != end; it.increment(ec)) {
                std::error_code fec;
                if (!it->is_regular_file(fec) || fec) continue;
                std::string relative;
                if (!makeRelative(projectDir_, it->path(), relative)) continue;
                paths.insert(relative);
            }
        }

        for (const std::string& relative : paths) {
            const fs::path absolute = projectDir_ / fs::path(relative);
            AssetRecord record;
            record.path = relative;
            record.kind = kindOf(absolute);
            record.size = static_cast<uint64_t>(fs::file_size(absolute, ec));
            if (ec) continue;
            const fs::file_time_type time = fs::last_write_time(absolute, ec);
            if (ec) continue;
            record.mtimeRaw = static_cast<int64_t>(time.time_since_epoch().count());

            const auto old = previous.find(relative);
            bool scanDependencies = true;
            if (!force && old != previous.end() && old->second.size == record.size &&
                old->second.mtimeRaw == record.mtimeRaw && !old->second.sha1.empty()) {
                record.sha1 = old->second.sha1;
                record.metadata = old->second.metadata;
                record.dependencies = old->second.dependencies;
                record.error = old->second.error;
                const auto scanner = record.metadata.find("dependencyScanner");
                scanDependencies = scanner == record.metadata.end() ||
                                   scanner->second != std::to_string(kDependencyScannerVersion);
                ++report.reused;
            } else {
                std::string hashError;
                if (!hashFile(absolute, record.sha1, hashError)) {
                    record.error = hashError;
                } else {
                    ++report.hashed;
                }
            }
            if (record.error.empty() && scanDependencies) {
                record.dependencies.clear();
                for (const std::string& candidate : dependencyCandidatesForAsset(absolute)) {
                    const std::string resolved = resolveDependency(projectDir_, absolute, candidate, paths);
                    if (!resolved.empty() && resolved != relative) record.dependencies.push_back(resolved);
                }
                std::sort(record.dependencies.begin(), record.dependencies.end());
                record.dependencies.erase(std::unique(record.dependencies.begin(), record.dependencies.end()),
                                         record.dependencies.end());
            }
            if (record.metadata.empty() || scanDependencies) record.metadata = metadataFor(absolute);
            record.metadata["dependencyScanner"] = std::to_string(kDependencyScannerVersion);
            fresh[relative] = std::move(record);
        }

        report.scanned = fresh.size();
        for (const auto& pair : previous) {
            if (fresh.find(pair.first) == fresh.end()) ++report.removed;
        }
        for (auto& pair : fresh) {
            AssetRecord& record = pair.second;
            record.missingDependencies.clear();
            for (const std::string& dependency : record.dependencies) {
                if (paths.find(dependency) == paths.end()) record.missingDependencies.push_back(dependency);
            }
            if (record.broken()) ++report.broken;
        }

        const std::string newManifest = computeManifest(fresh);
        report.changed = newManifest != previousManifest || fresh.size() != previous.size();
        records_ = std::move(fresh);
        manifestSha1_ = newManifest;
        report.manifestSha1 = manifestSha1_;
        if (!write(error)) return false;
        return true;
    }

}
