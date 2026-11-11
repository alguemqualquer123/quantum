// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "texture/Material.h"

#include <filesystem>
#include <string>
#include <vector>

namespace quantum::editor {

    // The material a newly created primitive starts with. Without one a box or a
    // plane is the engine's flat vertex-coloured white, so the first object in a new
    // scene looks unfinished rather than like a lit surface.
    //
    // The engine ships one PBR set and every project gets its own copy of it: a scene
    // stores each texture relative to the project's assets root, and an exported build
    // can only pack files the project owns. Everything here works on that root as a
    // plain path, so a CLI reaches it with no project loaded.
    class DefaultMaterial {
    public:
        // Where the shipped set lives: the engine folder next to the executable, or the
        // source tree of a checkout that has not built yet. Empty when neither has it.
        static std::filesystem::path sourceDir();

        // The shipped map names, in the order they are copied.
        static const std::vector<std::string>& fileNames();

        // Copies every map the project does not have into its assets root. A file that
        // is already there is left alone - a project may have edited its own copy.
        // Returns false with a message only when the engine has nothing to copy.
        static bool ensureInstalled(const std::filesystem::path& assetsRoot, std::string* error);

        // True when the maps the mesh shader samples are all in the project.
        static bool isInstalled(const std::filesystem::path& assetsRoot);

        // The material for a fresh primitive, or an empty one when the project has no
        // set: an untextured primitive is still a primitive.
        static Material build(const std::filesystem::path& assetsRoot);
    };

}
