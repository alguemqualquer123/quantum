// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "CreateFileCmd.h"

#include <fstream>

using namespace quantum;

editor::CreateFileCmd::CreateFileCmd(Project* project, const fs::path& file, const std::string& content)
    : file(file), content(content), deleteCmd(project, {file}, project->getProjectPath()){
}

bool editor::CreateFileCmd::execute(){
    if (created){
        deleteCmd.undo();
        return true;
    }

    std::ofstream out(file, std::ios::binary);
    if (!out.is_open()){
        return false;
    }

    out << content;
    out.close();

    created = true;
    return true;
}

void editor::CreateFileCmd::undo(){
    deleteCmd.execute();
}

bool editor::CreateFileCmd::mergeWith(editor::Command* otherCommand){
    return false;
}
