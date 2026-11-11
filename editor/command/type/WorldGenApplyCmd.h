// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Project.h"
#include "util/TerrainMapUtils.h"

#include <cstdint>
#include <string>

namespace quantum::editor{

    // Applying a generated heightmap to a terrain, and going back. The whole map is replaced, so the
    // undo is the path the terrain pointed at before rather than a copy of its pixels: the previous
    // file is still on disk, and a generated map is reproducible from the parameters it records.
    class WorldGenApplyCmd: public Command{

    private:
        Project* project;
        uint32_t sceneId;
        Entity entity;
        TerrainMapRef ref;
        std::string fromPath;
        std::string toPath;

        bool wasModified = false;

        void apply(const std::string& expect, const std::string& target, bool restoreModifiedState);

    public:
        WorldGenApplyCmd(Project* project, uint32_t sceneId, Entity entity,
                         const std::string& fromPath, const std::string& toPath);

        bool execute() override;
        void undo() override;

        bool mergeWith(Command* otherCommand) override;
    };

}
