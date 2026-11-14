// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/type/DeleteFileCmd.h"
#include "Project.h"
#include <string>
#include <filesystem>

namespace fs = std::filesystem;

namespace quantum::editor{

    class CreateFileCmd: public Command{

    private:

        fs::path file;
        std::string content;
        // Undo trashes the file, so redo keeps its edits
        DeleteFileCmd deleteCmd;
        bool created = false;

    public:
        CreateFileCmd(Project* project, const fs::path& file, const std::string& content);

        bool execute() override;
        void undo() override;

        bool mergeWith(Command* otherCommand) override;
    };

}
