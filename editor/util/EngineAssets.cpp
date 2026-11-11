// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "util/EngineAssets.h"

#include "util/FileUtils.h"
#include "util/Util.h"

#include <algorithm>
#include <cctype>
#include <system_error>

using namespace quantum::editor;

namespace fs = std::filesystem;

namespace {

    fs::path engineDirOverride;
    bool engineDirQueried = false;

    bool isDirectoryHere(const fs::path& path) {
        std::error_code ec;
        return fs::is_directory(path, ec) && !ec;
    }

    // Windows hands back the same folder with a different spelling depending on who
    // resolved it (shell drop, cmake copy, std::filesystem canonicalization).
    std::string foldCase(const std::string& text) {
        std::string out = text;
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    }

    fs::path resolveEngineDir() {
        engineDirQueried = true;
        if (!engineDirOverride.empty()) {
            return engineDirOverride.lexically_normal();
        }

        // copy_quantum_editor_engine puts the whole engine/ next to the executable, and
        // FileUtils::getEngineDir() is where every other engine-content reader looks.
        const fs::path shipped = FileUtils::getEngineDir();
        if (isDirectoryHere(shipped)) {
            return shipped;
        }

        // A checkout that never ran that copy still has the content in the source tree,
        // which sits above the build folder the editor runs from.
        fs::path dir = FileUtils::getExecutableDir();
        for (int depth = 0; depth <= 5 && !dir.empty(); ++depth) {
            const fs::path candidate = dir / "engine";
            if (isDirectoryHere(candidate / "assets") || isDirectoryHere(candidate / "components")) {
                return candidate;
            }
            dir = dir.parent_path();
        }

        // Nothing on disk: still answer with the canonical location, so callers can
        // report where they looked instead of an empty path.
        return shipped;
    }

    std::vector<EngineAssetRoot> buildRoots() {
        const fs::path& engine = EngineAssets::engineDir();

        const std::vector<EngineAssetRoot> candidates = {
            { "assets", "Stock material maps and textures that ship with the engine", engine / "assets" },
            { "components", "Ready-made Lua gameplay scripts, their demos and their sounds and textures", engine / "components" },
            { "shaderlib", "GLSL sources a project forks to customize rendering", engine / "shaderlib" },
        };

        std::vector<EngineAssetRoot> found;
        for (const EngineAssetRoot& candidate : candidates) {
            if (isDirectoryHere(candidate.path)) {
                found.push_back(candidate);
            }
        }
        return found;
    }

}

void EngineAssets::overrideEngineDir(const fs::path& dir) {
    if (engineDirQueried || dir.empty()) {
        return;
    }
    engineDirOverride = dir;
}

const fs::path& EngineAssets::engineDir() {
    static const fs::path dir = resolveEngineDir();
    return dir;
}

const std::vector<EngineAssetRoot>& EngineAssets::roots() {
    static const std::vector<EngineAssetRoot> found = buildRoots();
    return found;
}

bool EngineAssets::hasContent() {
    return !roots().empty();
}

bool EngineAssets::contains(const fs::path& parent, const fs::path& path) {
    std::error_code ec;
    fs::path head = fs::weakly_canonical(parent, ec);
    if (ec || head.empty()) {
        head = parent.lexically_normal();
    }

    ec.clear();
    fs::path tail = fs::weakly_canonical(path, ec);
    if (ec || tail.empty()) {
        tail = path.lexically_normal();
    }

    std::string prefix = foldCase(head.generic_string());
    const std::string value = foldCase(tail.generic_string());
    if (prefix.empty() || value.empty()) {
        return false;
    }
    if (prefix.back() != '/') {
        prefix += '/';
    }

    // Only a real folder boundary counts: C:/engine2 is not inside C:/engine.
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

bool EngineAssets::isEnginePath(const fs::path& path) {
    if (path.empty() || engineDir().empty()) {
        return false;
    }
    return contains(engineDir(), path);
}

AssetOrigin EngineAssets::originOf(const fs::path& path) {
    return isEnginePath(path) ? AssetOrigin::Engine : AssetOrigin::Project;
}

const char* EngineAssets::originLabel(AssetOrigin origin) {
    return origin == AssetOrigin::Engine ? "Engine" : "Project";
}

const EngineAssetRoot* EngineAssets::rootContaining(const fs::path& path) {
    for (const EngineAssetRoot& root : roots()) {
        if (contains(root.path, path)) {
            return &root;
        }
    }
    return nullptr;
}

fs::path EngineAssets::projectCopyDestination(const fs::path& engineFile,
                                              const fs::path& assetsPath,
                                              const fs::path& luaPath) {
    std::error_code ec;
    const bool directory = fs::is_directory(engineFile, ec) && !ec;

    fs::path destination;

    // A ready-made script is only useful where the component dialog and the ScriptEntry
    // paths look for it; the engine's own media is referenced from the assets root.
    if (!directory && Util::isLuaFile(engineFile.string())) {
        destination = luaPath / "Components";
    } else {
        const EngineAssetRoot* root = rootContaining(engineFile);
        destination = root ? assetsPath / "engine" / root->label : assetsPath / "engine";
        // The asset pack is sorted into subfolders (assets/trees, assets/materials/ground), and
        // flattening a copy collides two of them and loses the sort the browser showed, so a
        // single file keeps the part of its path that sits below the root. A directory already
        // carries its own name: the caller appends it, and its tree comes with it.
        if (root) {
            std::error_code belowError;
            const fs::path below = fs::relative(engineFile.parent_path(), root->path, belowError);
            if (!belowError && !below.empty() && below != ".") {
                destination /= below;
            }
        }
    }

    fs::create_directories(destination, ec);
    return destination;
}
