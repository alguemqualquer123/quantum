// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "ProjectDoctor.h"

#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>

namespace quantum::editor::cli {

    const char* doctorSeverityName(DoctorSeverity severity) {
        switch (severity) {
            case DoctorSeverity::Error: return "error";
            case DoctorSeverity::Warning: return "warning";
            case DoctorSeverity::Info: return "info";
        }
        return "info";
    }

    namespace {

        std::string trimmed(const std::string& value) {
            size_t begin = value.find_first_not_of(" \t\r\n");
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
            const std::string filename = fs::path(executableName).filename().string();
            return filename.empty() ? std::string("quantum-editor") : filename;
        }

        // 1-based line for a parsed node; 0 means "no mark", printed as no line.
        int nodeLine(const YAML::Node& node) {
            if (!node.IsDefined()) {
                return 0;
            }
            const YAML::Mark mark = node.Mark();
            if (mark.is_null() || mark.line < 0) {
                return 0;
            }
            return mark.line + 1;
        }

        bool scalarString(const YAML::Node& node, std::string& out) {
            if (!node || !node.IsScalar()) {
                return false;
            }
            out = node.as<std::string>();
            return true;
        }

        // Scene ids and entity ids are scalars an editor autosave can corrupt, so a
        // non-number must become an issue row instead of a YAML throw.
        bool scalarU32(const YAML::Node& node, uint32_t& out) {
            if (!node || !node.IsScalar()) {
                return false;
            }
            try {
                out = node.as<uint32_t>();
                return true;
            } catch (const YAML::Exception&) {
                return false;
            }
        }

        bool loadYaml(const fs::path& file, YAML::Node& root, std::string& error, int& errorLine) {
            errorLine = 0;
            try {
                root = YAML::LoadFile(file.string());
                return true;
            } catch (const YAML::BadFile& e) {
                error = "cannot open file";
                (void)e;
                return false;
            } catch (const YAML::Exception& e) {
                error = e.msg.empty() ? std::string("YAML parse error") : e.msg;
                if (!e.mark.is_null() && e.mark.line >= 0) {
                    errorLine = e.mark.line + 1;
                }
                return false;
            }
        }

        struct DoctorContext {
            fs::path projectDir;
            fs::path assetsRoot; // project.yaml "assetsDir" (default ".")
            fs::path luaRoot;    // project.yaml "luaDir" (default ".")
        };

        fs::path resolveRoot(const fs::path& projectDir, const std::string& ref) {
            const fs::path requested(ref.empty() ? "." : ref);
            const fs::path root = requested.is_absolute() ? requested : projectDir / requested;
            return root.lexically_normal();
        }

        // A YAML path reference counts as resolved when any root finds a regular file
        // at it; absolute paths (which survive on the machine that saved them) are
        // checked as-is. std::filesystem is used with error codes everywhere: the
        // editor build has _HAS_EXCEPTIONS=0.
        bool referenceExists(const std::string& ref, const std::vector<fs::path>& roots, std::error_code& ec) {
            if (ref.empty()) {
                return true; // an empty slot is "no texture", not a missing file
            }
            const fs::path requested(ref);
            if (requested.is_absolute()) {
                return fs::is_regular_file(requested, ec);
            }
            for (const fs::path& root : roots) {
                if (root.empty()) {
                    continue;
                }
                if (fs::is_regular_file(root / requested, ec)) {
                    return true;
                }
            }
            return false;
        }

        std::string displayPath(const fs::path& projectDir, const fs::path& path) {
            std::error_code ec;
            const fs::path relative = std::filesystem::relative(path, projectDir, ec);
            if (!ec && !relative.empty() && *relative.begin() != fs::path("..")) {
                return relative.generic_string();
            }
            return path.generic_string();
        }

        std::string displayRef(const fs::path& projectDir, const std::string& ref) {
            return displayPath(projectDir, fs::path(projectDir / ref).lexically_normal());
        }

        void addIssue(DoctorResult& result, DoctorSeverity severity, const std::string& check,
                      const std::string& file, int line, const std::string& message) {
            DoctorIssue issue;
            issue.severity = severity;
            issue.check = check;
            issue.file = file;
            issue.line = line;
            issue.message = message;
            result.issues.push_back(issue);
            switch (severity) {
                case DoctorSeverity::Error: ++result.errorCount; break;
                case DoctorSeverity::Warning: ++result.warningCount; break;
                case DoctorSeverity::Info: ++result.infoCount; break;
            }
        }

        std::vector<fs::path> assetRoots(const DoctorContext& ctx) {
            return { ctx.assetsRoot, ctx.projectDir };
        }

        struct SceneInfo {
            fs::path file;       // absolute path to the .scene
            std::string display; // project-relative for output
            YAML::Node root;
            bool hasId = false;
            uint32_t id = 0;
            int idLine = 0;
        };

        struct ModelReference {
            uint32_t id = 0;
            int line = 0;
        };

        bool isUnsetModelReference(uint32_t id) {
            // NULL_ENTITY is serialized as zero. MSVC's debug fill pattern can also be
            // present in old generated scenes; report it distinctly instead of pretending
            // it is a reference to an ordinary missing entity.
            return id == 0;
        }

        // CameraComponent::yfov is the radians handed straight to Matrix4::perspectiveMatrix, while
        // the C++/Lua setters take degrees (Engine::useDegrees defaults to true). A scene written by
        // hand as "yfov: 60" therefore asks for a 60-radian frustum and renders nonsense. Ortho
        // cameras ignore the field, so only perspective ones are judged.
        void checkCameraNode(const YAML::Node& camera, const std::string& file, DoctorResult& result) {
            if (!camera || !camera.IsMap()) {
                return;
            }
            std::string type;
            if (!scalarString(camera["type"], type) || type != "camera_perspective") {
                return;
            }
            const YAML::Node yfov = camera["yfov"];
            if (!yfov || !yfov.IsScalar()) {
                return;
            }
            double value = 0.0;
            try {
                value = yfov.as<double>();
            } catch (const YAML::Exception&) {
                return;
            }
            if (value > 0.05 && value <= 3.14) {
                return;
            }
            std::ostringstream message;
            message << "camera yfov " << value << " is not a usable vertical field of view in radians "
                    << "(0.75 is the engine default, 45 degrees is 0.79)";
            addIssue(result, DoctorSeverity::Warning, "camera-yfov-radians", file, nodeLine(yfov), message.str());
        }

        // One entity row, and its children. Ids 1..1000 are system ids and user
        // entities start at 1001, but the same id must still appear only once per
        // scene: a repeated id is what the editor draws as a blank outliner row and
        // then writes back as a nameless entity (the corruption that multiplies).
        void walkEntity(const YAML::Node& entityNode, const DoctorContext& ctx, const std::string& sceneDisplay,
                        std::map<uint32_t, int>& seenIds, DoctorResult& result,
                        std::vector<ModelReference>& modelRefs) {
            if (!entityNode || !entityNode.IsMap()) {
                return;
            }

            uint32_t id = 0;
            if (scalarU32(entityNode["entity"], id)) {
                const int line = nodeLine(entityNode["entity"]);
                const auto found = seenIds.find(id);
                if (found != seenIds.end()) {
                    std::ostringstream message;
                    message << "duplicate entity id " << id << " (first used at line " << found->second << ")";
                    addIssue(result, DoctorSeverity::Error, "duplicate-entity-id", sceneDisplay, line, message.str());
                } else {
                    seenIds[id] = line;
                    ++result.entitiesAudited;
                }
            }

            const YAML::Node components = entityNode["components"];
            if (components && components.IsMap()) {
                for (YAML::const_iterator it = components.begin(); it != components.end(); ++it) {
                    std::string key;
                    if (!scalarString(it->first, key)) {
                        continue;
                    }
                    const YAML::Node comp = it->second;
                    if (!comp || !comp.IsMap()) {
                        continue;
                    }

                    if (key == "camera") {
                        checkCameraNode(comp, sceneDisplay, result);
                    }

                    if (key == "model") {
                        std::string filename;
                        if (scalarString(comp["filename"], filename) && !filename.empty()) {
                            std::error_code ec;
                            if (!referenceExists(filename, assetRoots(ctx), ec)) {
                                addIssue(result, DoctorSeverity::Error, "missing-model", sceneDisplay,
                                         nodeLine(comp["filename"]),
                                         "ModelComponent filename not found: " + displayRef(ctx.projectDir, filename));
                            }
                        }
                        // The node maps store raw entity ids; a recycled id makes the
                        // model claim an unrelated row, so verify they exist in-scene.
                        uint32_t skeleton = 0;
                        if (scalarU32(comp["skeleton"], skeleton)) {
                            modelRefs.push_back({ skeleton, nodeLine(comp["skeleton"]) });
                        }
                        if (comp["animations"] && comp["animations"].IsSequence()) {
                            for (const auto& anim : comp["animations"]) {
                                uint32_t animId = 0;
                                if (scalarU32(anim, animId)) {
                                    modelRefs.push_back({ animId, nodeLine(anim) });
                                }
                            }
                        }
                        for (const char* mapKey : { "bonesIdMapping", "meshNodesMapping", "nodesIdMapping" }) {
                            if (comp[mapKey] && comp[mapKey].IsSequence()) {
                                for (const auto& entry : comp[mapKey]) {
                                    uint32_t entityId = 0;
                                    if (entry && entry.IsMap() && scalarU32(entry["entity"], entityId)) {
                                        modelRefs.push_back({ entityId, nodeLine(entry["entity"]) });
                                    }
                                }
                            }
                        }
                    } else if (key == "mesh") {
                        std::string path;
                        if (scalarString(comp["path"], path) && !path.empty()) {
                            std::error_code ec;
                            if (!referenceExists(path, assetRoots(ctx), ec)) {
                                addIssue(result, DoctorSeverity::Error, "missing-mesh-file", sceneDisplay,
                                         nodeLine(comp["path"]),
                                         "MeshComponent path not found: " + displayRef(ctx.projectDir, path));
                            }
                        }
                    } else if (key == "script") {
                        const YAML::Node scripts = comp["scripts"];
                        if (scripts && scripts.IsSequence()) {
                            for (const auto& entry : scripts) {
                                if (!entry || !entry.IsMap()) {
                                    continue;
                                }
                                std::string type = "cpp";
                                scalarString(entry["type"], type);
                                std::string path;
                                if (!scalarString(entry["path"], path) || path.empty()) {
                                    continue;
                                }
                                // A Lua ScriptEntry path is relative to the project's
                                // Lua root (what the Inspector resolves against);
                                // cpp/graph entries are project-relative.
                                const std::vector<fs::path> roots =
                                    (type == "lua") ? std::vector<fs::path>{ ctx.luaRoot, ctx.projectDir }
                                                    : std::vector<fs::path>{ ctx.projectDir };
                                std::error_code ec;
                                if (!referenceExists(path, roots, ec)) {
                                    addIssue(result, DoctorSeverity::Error, "missing-script", sceneDisplay,
                                             nodeLine(entry["path"]),
                                             "ScriptComponent (" + type + ") not found: " +
                                                 displayRef(ctx.projectDir, path));
                                }
                            }
                        }
                    }
                }
            }

            if (entityNode["children"] && entityNode["children"].IsSequence()) {
                for (const auto& child : entityNode["children"]) {
                    walkEntity(child, ctx, sceneDisplay, seenIds, result, modelRefs);
                }
            }
        }

        // Texture nodes (encodeTexture) always say how they resolve: source "path"
        // with one file, "paths" with six faces, "id"/"data"/"camera" with none.
        // Scanning the whole tree catches every component that embeds a material
        // (mesh submeshes, sprite, ui, sky, particles, reflection probes) without
        // duplicating the component list here.
        void scanTree(const YAML::Node& node, const DoctorContext& ctx, const std::string& file,
                      int depth, DoctorResult& result) {
            if (!node || depth > 64) {
                return;
            }

            if (node.IsSequence()) {
                for (const auto& child : node) {
                    scanTree(child, ctx, file, depth + 1, result);
                }
                return;
            }

            if (!node.IsMap()) {
                return;
            }

            std::string source;
            if (scalarString(node["source"], source)) {
                if (source == "path") {
                    std::string path;
                    if (scalarString(node["path"], path) && !path.empty()) {
                        std::error_code ec;
                        if (!referenceExists(path, assetRoots(ctx), ec)) {
                            addIssue(result, DoctorSeverity::Error, "missing-texture", file,
                                     nodeLine(node["path"]),
                                     "Texture path not found: " + displayRef(ctx.projectDir, path));
                        }
                    }
                } else if (source == "paths") {
                    if (node["paths"] && node["paths"].IsSequence()) {
                        int face = 0;
                        for (const auto& faceNode : node["paths"]) {
                            std::string path;
                            if (scalarString(faceNode, path) && !path.empty()) {
                                std::error_code ec;
                                if (!referenceExists(path, assetRoots(ctx), ec)) {
                                    std::ostringstream label;
                                    label << "Cube map face " << face << " not found: "
                                          << displayRef(ctx.projectDir, path);
                                    addIssue(result, DoctorSeverity::Error, "missing-texture", file,
                                             nodeLine(node["paths"]), label.str());
                                }
                            }
                            ++face;
                        }
                    }
                }
            }

            // A forked shader is named by its project-relative path in a key
            // (encodeShaderKeys note); without the file the load fails for the
            // whole scene, so this is an error, not a hint.
            for (YAML::const_iterator it = node.begin(); it != node.end(); ++it) {
                std::string key;
                if (!scalarString(it->first, key)) {
                    continue;
                }
                if (key == "customShader") {
                    std::string ref;
                    if (scalarString(it->second, ref) && !ref.empty()) {
                        // A fork is stored as a stem, or as "vert|frag" when the entry points are
                        // separately named - never as one file - so the old exact-path test flagged
                        // every valid fork as missing. Mirrors Util::resolveCustomShaderPaths.
                        const size_t sep = ref.find('|');
                        const std::string vert = sep != std::string::npos ? ref.substr(0, sep) : ref + ".vert";
                        const std::string frag = sep != std::string::npos ? ref.substr(sep + 1) : ref + ".frag";
                        const std::vector<fs::path> roots = { ctx.projectDir, ctx.assetsRoot };
                        std::error_code ec;
                        for (const std::string& entry : { vert, frag }) {
                            if (referenceExists(entry, roots, ec)) {
                                continue;
                            }
                            addIssue(result, DoctorSeverity::Error, "missing-shader-file", file,
                                     nodeLine(it->second),
                                     "Custom shader entry not found: " + displayRef(ctx.projectDir, entry));
                        }
                    }
                }
                scanTree(it->second, ctx, file, depth + 1, result);
            }
        }

        fs::file_time_type mtimeOf(const fs::path& file, bool& ok) {
            std::error_code ec;
            const fs::file_time_type when = fs::last_write_time(file, ec);
            ok = !ec;
            return when;
        }

        bool isDoctorSourceExt(const fs::path& path) {
            std::string ext = path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            static const std::set<std::string> exts = {
                ".scene", ".bundle", ".h", ".hpp", ".hh", ".hxx", ".inl",
                ".cpp", ".cc", ".cxx", ".c++", ".c"
            };
            return exts.count(ext) > 0;
        }

        // Mirrors quantum-build-generator status (BuildGeneratorCli.cpp): the stamp
        // stands in for the outputs' time because a generate run rewrites
        // project.yaml after the sources it just read.
        void checkGeneration(const DoctorContext& ctx, DoctorResult& result) {
            const fs::path internalDir = ctx.projectDir / ".quantum";
            std::error_code ec;
            const bool haveInternal = fs::is_directory(internalDir, ec);
            const bool haveOutputs =
                fs::is_regular_file(ctx.projectDir / "CMakeLists.txt", ec) &&
                fs::is_regular_file(internalDir / "scene_scripts.cpp", ec) &&
                fs::is_regular_file(internalDir / "generated" / "main.cpp", ec);

            if (!haveInternal || !haveOutputs) {
                addIssue(result, DoctorSeverity::Info, "not-generated", "project.yaml", 0,
                         "generated sources are missing; run "
                         "\"quantum-build-generator generate --project <dir>\"");
                return;
            }

            bool haveSource = false;
            fs::file_time_type newestSource{};
            auto consider = [&](const fs::path& file) {
                bool ok = false;
                const fs::file_time_type when = mtimeOf(file, ok);
                if (!ok) {
                    return;
                }
                if (!haveSource || when > newestSource) {
                    newestSource = when;
                }
                haveSource = true;
            };

            consider(ctx.projectDir / "project.yaml");
            {
                fs::recursive_directory_iterator it(ctx.projectDir, fs::directory_options::skip_permission_denied, ec);
                fs::recursive_directory_iterator end;
                for (; !ec && it != end; it.increment(ec)) {
                    const std::string stem = it->path().filename().string();
                    if (it->is_directory(ec)) {
                        // Everything hidden is machine state (.quantum, .vscode, git),
                        // never a generator input.
                        if (!stem.empty() && stem[0] == '.') {
                            it.disable_recursion_pending();
                        }
                        continue;
                    }
                    std::error_code fec;
                    if (it->is_regular_file(fec) && !fec && isDoctorSourceExt(it->path())) {
                        consider(it->path());
                    }
                }
            }

            bool haveOutput = false;
            fs::file_time_type newestOutput{};
            for (const fs::path& file : { ctx.projectDir / "CMakeLists.txt",
                                          internalDir / "scene_scripts.cpp",
                                          internalDir / "generated" / "main.cpp",
                                          internalDir / "generation.stamp" }) {
                std::error_code fec;
                if (!fs::is_regular_file(file, fec)) {
                    continue;
                }
                bool ok = false;
                const fs::file_time_type when = mtimeOf(file, ok);
                if (!ok) {
                    continue;
                }
                if (!haveOutput || when > newestOutput) {
                    newestOutput = when;
                }
                haveOutput = true;
            }

            if (haveOutput && haveSource && newestSource > newestOutput) {
                addIssue(result, DoctorSeverity::Warning, "stale-generated", "project.yaml", 0,
                         "a .scene/.bundle/script is newer than the generated output; run "
                         "\"quantum-build-generator generate --project <dir>\"");
            }
        }

        int countSdat(const fs::path& dir) {
            std::error_code ec;
            if (!fs::is_directory(dir, ec)) {
                return 0;
            }
            int count = 0;
            fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
            fs::recursive_directory_iterator end;
            for (; !ec && it != end && count < 4096; it.increment(ec)) {
                std::error_code fec;
                if (it->is_regular_file(fec) && !fec && it->path().extension() == ".sdat") {
                    ++count;
                }
            }
            return count;
        }

        // LocalPaths.cmake bakes in the machine's shader cache (Generator.cpp writes
        // QUANTUM_SHADER_PATH from App::getUserShaderCacheDir). A generated project
        // whose cache holds no .sdat makes the standalone engine abort with "Quantum is
        // missing precompiled shaders" long before the editor would tell why.
        void checkSdatCache(const DoctorContext& ctx, DoctorResult& result) {
            if (countSdat(ctx.projectDir / "assets" / "shaders") > 0) {
                return;
            }

            fs::path cacheDir;
            std::error_code ec;
            const fs::path localPaths = ctx.projectDir / ".quantum" / "LocalPaths.cmake";
            if (fs::is_regular_file(localPaths, ec)) {
                std::ifstream file(localPaths, std::ios::in | std::ios::binary);
                std::stringstream buffer;
                buffer << file.rdbuf();
                const std::string text = buffer.str();
                const size_t marker = text.find("QUANTUM_SHADER_PATH");
                if (marker != std::string::npos) {
                    const size_t open = text.find('"', marker);
                    const size_t close = open == std::string::npos ? open : text.find('"', open + 1);
                    if (open != std::string::npos && close != std::string::npos && close > open) {
                        cacheDir = fs::path(text.substr(open + 1, close - open - 1));
                    }
                }
            }

            if (!cacheDir.empty() && countSdat(cacheDir) > 0) {
                return;
            }

            addIssue(result, DoctorSeverity::Warning, "missing-sdat", ".quantum/LocalPaths.cmake", 0,
                     "no .sdat in assets/shaders nor in the shader cache (" +
                     (cacheDir.empty() ? std::string("unknown") : cacheDir.generic_string()) +
                     "); a standalone run aborts with 'Quantum is missing precompiled shaders'");
        }

        void auditScene(const SceneInfo& scene, const DoctorContext& ctx,
                        const std::set<uint32_t>& allSceneIds, DoctorResult& result) {
            const YAML::Node root = scene.root;

            scanTree(root, ctx, scene.display, 0, result);

            std::map<uint32_t, int> seenIds;
            std::vector<ModelReference> modelRefs;
            if (root["entities"] && root["entities"].IsSequence()) {
                for (const auto& entityNode : root["entities"]) {
                    walkEntity(entityNode, ctx, scene.display, seenIds, result, modelRefs);
                }
            }

            uint32_t mainCamera = 0;
            if (scalarU32(root["mainCamera"], mainCamera) && mainCamera != 0) {
                if (seenIds.find(mainCamera) == seenIds.end()) {
                    std::ostringstream message;
                    message << "mainCamera " << mainCamera << " is not an entity in this scene";
                    addIssue(result, DoctorSeverity::Warning, "unknown-main-camera", scene.display,
                             nodeLine(root["mainCamera"]), message.str());
                }
            }

            for (const ModelReference& ref : modelRefs) {
                if (isUnsetModelReference(ref.id)) continue;
                if (ref.id == 0xCCCCCCCCu) {
                    addIssue(result, DoctorSeverity::Error, "uninitialized-model-reference", scene.display,
                             ref.line, "ModelComponent contains the MSVC uninitialized entity sentinel 0xCCCCCCCC");
                    continue;
                }
                if (seenIds.find(ref.id) == seenIds.end()) {
                    std::ostringstream message;
                    message << "ModelComponent references entity id " << ref.id
                            << " which does not exist in this scene";
                    addIssue(result, DoctorSeverity::Warning, "unknown-model-reference", scene.display,
                             ref.line, message.str());
                }
            }

            if (root["bundles"] && root["bundles"].IsSequence()) {
                for (const auto& bundle : root["bundles"]) {
                    std::string path;
                    if (scalarString(bundle, path) && !path.empty()) {
                        std::error_code ec;
                        const std::vector<fs::path> roots = { ctx.projectDir, ctx.assetsRoot };
                        if (!referenceExists(path, roots, ec)) {
                            addIssue(result, DoctorSeverity::Error, "missing-bundle", scene.display,
                                     nodeLine(bundle), "Bundle not found: " + displayRef(ctx.projectDir, path));
                        }
                    }
                }
            }

            if (root["imports"] && root["imports"].IsMap()) {
                for (YAML::const_iterator it = root["imports"].begin(); it != root["imports"].end(); ++it) {
                    std::string path;
                    if (scalarString(it->first, path) && !path.empty()) {
                        std::error_code ec;
                        const std::vector<fs::path> roots = { ctx.projectDir, ctx.assetsRoot };
                        if (!referenceExists(path, roots, ec)) {
                            addIssue(result, DoctorSeverity::Error, "missing-import", scene.display,
                                     nodeLine(it->first), "Imported scene not found: " + displayRef(ctx.projectDir, path));
                        }
                    }
                }
            }

            if (root["childScenes"] && root["childScenes"].IsSequence()) {
                for (const auto& child : root["childScenes"]) {
                    uint32_t childId = 0;
                    if (child && child.IsMap() && scalarU32(child["id"], childId)) {
                        if (allSceneIds.find(childId) == allSceneIds.end()) {
                            std::ostringstream message;
                            message << "childScenes references scene id " << childId << " which no listed scene has";
                            addIssue(result, DoctorSeverity::Warning, "unknown-child-scene", scene.display,
                                     nodeLine(child["id"]), message.str());
                        }
                    }
                }
            }
        }

    } // anonymous namespace

    DoctorResult auditProject(const fs::path& projectDirArg) {
        DoctorResult result;
        const fs::path projectDir = projectDirArg.lexically_normal();
        std::error_code ec;

        if (!fs::is_directory(projectDir, ec)) {
            result.fatal = "not a directory: " + projectDir.string();
            return result;
        }

        DoctorContext ctx;
        ctx.projectDir = projectDir;
        ctx.assetsRoot = projectDir;
        ctx.luaRoot = projectDir;

        const fs::path projectFile = projectDir / "project.yaml";
        if (!fs::is_regular_file(projectFile, ec)) {
            result.fatal = "no project.yaml in " + projectDir.generic_string();
            return result;
        }

        YAML::Node projectNode;
        std::string parseError;
        int parseLine = 0;
        if (!loadYaml(projectFile, projectNode, parseError, parseLine)) {
            addIssue(result, DoctorSeverity::Error, "invalid-project-yaml", "project.yaml", parseLine,
                     "cannot parse: " + parseError);
            result.completed = true;
            return result;
        }
        if (!projectNode.IsMap()) {
            addIssue(result, DoctorSeverity::Error, "invalid-project-yaml", "project.yaml", nodeLine(projectNode),
                     "root must be a YAML map");
            result.completed = true;
            return result;
        }

        std::string assetsRef;
        if (scalarString(projectNode["assetsDir"], assetsRef) && !assetsRef.empty()) {
            ctx.assetsRoot = resolveRoot(projectDir, assetsRef);
        }
        std::string luaRef;
        if (scalarString(projectNode["luaDir"], luaRef) && !luaRef.empty()) {
            ctx.luaRoot = resolveRoot(projectDir, luaRef);
        }
        if (!fs::is_directory(ctx.luaRoot, ec)) {
            addIssue(result, DoctorSeverity::Warning, "missing-lua-dir", "project.yaml",
                     nodeLine(projectNode["luaDir"]),
                     "luaDir does not exist: " + displayPath(projectDir, ctx.luaRoot));
        }
        if (!fs::is_directory(ctx.assetsRoot, ec)) {
            addIssue(result, DoctorSeverity::Warning, "missing-assets-dir", "project.yaml",
                     nodeLine(projectNode["assetsDir"]),
                     "assetsDir does not exist: " + displayPath(projectDir, ctx.assetsRoot));
        }

        // Phase 1: read the scene files so the project-wide id set exists before any
        // startSceneId / childScenes reference is judged.
        std::vector<SceneInfo> scenes;
        std::set<uint32_t> sceneIds;
        if (projectNode["scenes"] && projectNode["scenes"].IsSequence()) {
            for (const auto& sceneNode : projectNode["scenes"]) {
                if (!sceneNode || !sceneNode.IsMap()) {
                    addIssue(result, DoctorSeverity::Error, "invalid-scene-entry", "project.yaml",
                             nodeLine(sceneNode), "scenes[] entry must be a map with a filepath");
                    continue;
                }
                std::string filepath;
                if (!scalarString(sceneNode["filepath"], filepath) || filepath.empty()) {
                    addIssue(result, DoctorSeverity::Error, "invalid-scene-entry", "project.yaml",
                             nodeLine(sceneNode), "scenes[] entry has no filepath");
                    continue;
                }

                checkCameraNode(sceneNode["editorCamera"], "project.yaml", result);

                const fs::path sceneFile = (projectDir / filepath).lexically_normal();
                SceneInfo scene;
                scene.file = sceneFile;
                scene.display = displayPath(projectDir, sceneFile);

                if (!fs::is_regular_file(sceneFile, ec)) {
                    addIssue(result, DoctorSeverity::Error, "missing-scene", "project.yaml",
                             nodeLine(sceneNode["filepath"]),
                             "scene file listed in project.yaml not found: " + scene.display);
                    continue;
                }

                std::string error;
                int errorLine = 0;
                if (!loadYaml(sceneFile, scene.root, error, errorLine)) {
                    addIssue(result, DoctorSeverity::Error, "invalid-scene-yaml", scene.display, errorLine,
                             "cannot parse: " + error);
                    continue;
                }
                if (!scene.root.IsMap()) {
                    addIssue(result, DoctorSeverity::Error, "invalid-scene-yaml", scene.display,
                             nodeLine(scene.root), "root must be a YAML map");
                    continue;
                }

                if (scalarU32(scene.root["id"], scene.id)) {
                    scene.hasId = true;
                    scene.idLine = nodeLine(scene.root["id"]);
                    if (sceneIds.insert(scene.id).second) {
                        // first scene with this id
                    } else {
                        std::ostringstream message;
                        message << "scene id " << scene.id << " is used by more than one scene file";
                        addIssue(result, DoctorSeverity::Warning, "duplicate-scene-id", scene.display,
                                 scene.idLine, message.str());
                    }
                } else {
                    addIssue(result, DoctorSeverity::Warning, "missing-scene-id", scene.display,
                             nodeLine(scene.root["id"]), "scene has no numeric id; startSceneId cannot reach it");
                }
                scenes.push_back(scene);
            }
        } else {
            addIssue(result, DoctorSeverity::Warning, "no-scenes", "project.yaml", nodeLine(projectNode["scenes"]),
                     "project.yaml lists no scenes[]");
        }

        uint32_t startSceneId = 0;
        if (scalarU32(projectNode["startSceneId"], startSceneId)) {
            if (sceneIds.find(startSceneId) == sceneIds.end()) {
                std::ostringstream message;
                message << "startSceneId " << startSceneId << " matches no scene id in the project";
                addIssue(result, DoctorSeverity::Error, "start-scene-missing", "project.yaml",
                         nodeLine(projectNode["startSceneId"]), message.str());
            }
        }

        // Phase 2: per-scene content audit.
        for (const SceneInfo& scene : scenes) {
            auditScene(scene, ctx, sceneIds, result);
        }
        result.scenesAudited = static_cast<int>(scenes.size());

        // The project file itself may reference forks inside its export section.
        scanTree(projectNode, ctx, "project.yaml", 0, result);

        checkGeneration(ctx, result);
        checkSdatCache(ctx, result);

        result.completed = true;
        return result;
    }

    namespace {

        void printUsage(const std::string& commandName) {
            std::cout
                << "Usage:\n"
                << "  " << commandName << " doctor --project <dir> [--help]\n\n"
                << "Audits a quantum project folder by reading project.yaml and its scenes\n"
                << "straight from disk (no project load, no window), so it works even on\n"
                << "projects the editor cannot open. One line per issue:\n"
                << "  <severity> <check-id> <file>:<line> <message>\n\n"
                << "Checks: invalid-project-yaml, invalid-scene-yaml, invalid-scene-entry,\n"
                << "missing-scene, no-scenes, missing-scene-id, duplicate-scene-id,\n"
                << "start-scene-missing, duplicate-entity-id, missing-model, missing-mesh-file,\n"
                << "missing-texture, missing-script, missing-bundle, missing-import,\n"
                << "missing-shader-file, missing-lua-dir, missing-assets-dir, unknown-main-camera,\n"
                << "unknown-model-reference, uninitialized-model-reference, unknown-child-scene, camera-yfov-radians,\n"
                << "not-generated, stale-generated.\n\n"
                << "Exit code: 0 clean (info rows only), 1 warnings, 2 errors or unreadable\n"
                << "project. Re-run quantum-build-generator generate after fixing scenes.\n";
        }

    } // anonymous namespace

    int runProjectDoctorCommand(int argc, char** argv, const char* executableName) {
        const std::string commandName = commandBaseName(executableName);

        std::string projectPath;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help" || arg == "-h") {
                printUsage(commandName);
                return 0;
            } else if (arg == "--project") {
                if (i + 1 >= argc) {
                    std::cerr << commandName << " doctor: --project needs a value\n";
                    return 2;
                }
                projectPath = argv[++i];
            } else {
                std::cerr << commandName << " doctor: unexpected argument: " << arg << "\n\n";
                printUsage(commandName);
                return 2;
            }
        }

        if (projectPath.empty()) {
            std::cerr << commandName << " doctor: missing --project <dir>\n\n";
            printUsage(commandName);
            return 2;
        }

        DoctorResult result;
        try {
            result = auditProject(fs::path(projectPath));
        } catch (const std::exception& e) {
            // Nothing here is supposed to throw, but the MSVC STL still can under
            // allocation pressure; a doctor must never be the crash it reports.
            std::cerr << commandName << " doctor: unexpected error: " << e.what() << "\n";
            return 2;
        } catch (...) {
            std::cerr << commandName << " doctor: unexpected error.\n";
            return 2;
        }

        if (!result.completed) {
            std::cerr << commandName << " doctor: " << result.fatal << "\n";
            return 2;
        }

        std::cout << commandName << " doctor: " << fs::path(projectPath).lexically_normal().generic_string() << "\n";
        for (const DoctorIssue& issue : result.issues) {
            std::string location = issue.file;
            if (issue.line > 0) {
                location += ":" + std::to_string(issue.line);
            }
            std::string severity = doctorSeverityName(issue.severity);
            if (severity.size() < 7) {
                severity.append(7 - severity.size(), ' ');
            }
            std::cout << severity << "   ";
            std::string check = issue.check;
            if (check.size() < 23) {
                check.append(23 - check.size(), ' ');
            }
            std::cout << check << ' ';
            if (location.size() < 33) {
                location.append(33 - location.size(), ' ');
            }
            std::cout << location << ' ' << issue.message << "\n";
        }

        std::cout << "\nSummary: " << result.errorCount << " error(s), " << result.warningCount
                  << " warning(s), " << result.infoCount << " info(s); " << result.scenesAudited
                  << " scene(s), " << result.entitiesAudited << " entity id(s) audited.\n";
        if (result.errorCount == 0 && result.warningCount == 0 && result.infoCount == 0) {
            std::cout << "Project looks clean.\n";
        }

        if (result.errorCount > 0) {
            return 2;
        }
        if (result.warningCount > 0) {
            return 1;
        }
        return 0;
    }

}
