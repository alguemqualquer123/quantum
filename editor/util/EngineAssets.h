// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace quantum::editor {

    // Which folder an asset in the browser was read from: the project the user owns,
    // or the engine copy shipped next to the editor.
    enum class AssetOrigin {
        Project,
        Engine
    };

    // One engine folder a project can consume. `label` is what the browser shows and
    // `description` the reason the folder is there, so a row never needs a guess.
    struct EngineAssetRoot {
        std::string label;
        std::string description;
        std::filesystem::path path;
    };

    // The engine ships content next to the editor executable (the CMake target
    // copy_quantum_editor_engine): stock material maps, the ready-made Lua component
    // library with its sounds and textures, and the shader sources a project forks.
    // The browser lists those beside the project's own files, read-only.
    class EngineAssets {
    public:
        // Point the resolver at another engine folder. Only `assets --engine-dir` calls it,
        // and only before the first query: the roots below cache what engineDir() answered,
        // so a later call is ignored rather than silently disagreeing with the process.
        static void overrideEngineDir(const std::filesystem::path& dir);

        // Folder that holds the engine content, resolved once per process: the copy
        // next to the executable, else the engine/ of a checkout that never ran that
        // copy. Empty only when neither exists, which is when the browser says so.
        static const std::filesystem::path& engineDir();

        // The consumable folders of engineDir() that actually exist, in display order.
        static const std::vector<EngineAssetRoot>& roots();

        static bool hasContent();

        static AssetOrigin originOf(const std::filesystem::path& path);
        static const char* originLabel(AssetOrigin origin);

        // True when path is inside one of engineDir()'s folders.
        static bool isEnginePath(const std::filesystem::path& path);

        // The engine root containing path, or nullptr when it is not engine content.
        static const EngineAssetRoot* rootContaining(const std::filesystem::path& path);

        // Where a copied engine file lands in the project: <luaDir>/Components keeps
        // the ready-made scripts where the component dialog looks for them, everything
        // the runtime references goes to the assets root. The folder is created.
        static std::filesystem::path projectCopyDestination(const std::filesystem::path& engineFile,
                                                            const std::filesystem::path& assetsPath,
                                                            const std::filesystem::path& luaPath);

    private:
        // Case-insensitive on Windows, where the same folder arrives with a different
        // spelling from the shell and from the resolver.
        static bool contains(const std::filesystem::path& parent, const std::filesystem::path& path);
    };

}
