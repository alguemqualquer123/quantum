// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Project.h"

namespace quantum::editor{

    class SetMainCameraCmd: public Command{

    private:
        Project* project;
        uint32_t sceneId;
        Entity newMainCamera;
        Entity oldMainCamera;
        bool wasModified;

    public:
        SetMainCameraCmd(Project* project, uint32_t sceneId, Entity newMainCamera);

        bool execute() override;
        void undo() override;

        bool mergeWith(Command* otherCommand) override;
    };

}