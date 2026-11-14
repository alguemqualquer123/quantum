// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <vector>

#include "Scene.h"   // Entity, Scene

namespace quantum::editor {

    class Project;
    struct SceneProject;

    // One ready-made Lua component of the engine library, as the "Add new component" dialog
    // draws it. It is not a ComponentType - attaching one adds a script entry - so a row only
    // carries the text the list shows.
    struct ReadyMadeEntry {
        std::string name;
        std::string description;
    };

    struct ReadyMadeCategory {
        std::string name;
        std::vector<ReadyMadeEntry> entries;
    };

    // The library grouped by the "-- category:" line each file declares. Read from disk when
    // the dialog opens, so a component installed since the project loaded shows up without
    // a restart.
    std::vector<ReadyMadeCategory> libraryComponentCategories();

    // True when the entity already runs the named library component.
    bool runsLibraryComponent(Scene* scene, Entity entity, const std::string& className);

    // Installs the component into <luaRoot>/Components when the project does not have it and
    // attaches it to every selected entity as one undoable command - the same ScriptEntry
    // recipe "New Script" uses.
    void attachLibraryComponent(Project* project, SceneProject* sceneProject,
                                const std::vector<Entity>& entities, const std::string& name);

}
