// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "util/ReadyMadeComponents.h"

#include "Catalog.h"
#include "Out.h"
#include "Project.h"
#include "cli/ComponentLibrary.h"
#include "command/CommandHandle.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/PropertyCmd.h"
#include "component/ScriptComponent.h"

#include <algorithm>
#include <filesystem>
#include <memory>

namespace quantum::editor {

    namespace fs = std::filesystem;

    // Library components land in this folder of the project's Lua root, which is where
    // Project::installReadyMadeComponents and `components install` put them.
    static const char* const LIBRARY_SUBDIR = "Components";

    std::vector<ReadyMadeCategory> libraryComponentCategories() {
        const std::vector<cli::ReadyMadeComponent> library =
            cli::listReadyMadeComponents(cli::findComponentsDir());

        std::vector<ReadyMadeCategory> categories;
        for (const cli::ReadyMadeComponent& component : library) {
            const std::string group = component.category.empty() ? std::string("Scripts") : component.category;

            auto category = std::find_if(categories.begin(), categories.end(),
                [&group](const ReadyMadeCategory& candidate) { return candidate.name == group; });
            if (category == categories.end()) {
                category = categories.insert(categories.end(), ReadyMadeCategory{ group, {} });
            }
            category->entries.push_back(ReadyMadeEntry{ component.name, component.summary });
        }
        return categories;
    }

    bool runsLibraryComponent(Scene* scene, Entity entity, const std::string& className) {
        const ScriptComponent* script = scene->findComponent<ScriptComponent>(entity);
        if (!script) {
            return false;
        }
        for (const ScriptEntry& entry : script->scripts) {
            if (entry.className == className) {
                return true;
            }
        }
        return false;
    }

    void attachLibraryComponent(Project* project, SceneProject* sceneProject,
                                const std::vector<Entity>& entities, const std::string& name) {
        if (!project || !sceneProject || entities.empty()) {
            return;
        }

        const fs::path source = cli::findComponentsDir() / (name + ".lua");
        std::error_code ec;
        if (!fs::is_regular_file(source, ec)) {
            Out::warning("The engine ships no component named \"%s\" at %s",
                         name.c_str(), source.string().c_str());
            return;
        }

        // Once the file is in the project it belongs to the project: never overwrite it here.
        const fs::path destination = project->getLuaPath() / LIBRARY_SUBDIR / (name + ".lua");
        ec.clear();
        if (!fs::is_regular_file(destination, ec)) {
            fs::create_directories(destination.parent_path(), ec);
            fs::copy_file(source, destination, fs::copy_options::skip_existing, ec);
            if (ec) {
                Out::error("Could not install %s: %s", name.c_str(), ec.message().c_str());
                return;
            }
            Out::success("Installed component %s in %s", name.c_str(),
                         destination.parent_path().string().c_str());
        }

        // A Lua script path is stored relative to the project's Lua root, like "New Script" stores it.
        const std::string scriptPath = (fs::path(LIBRARY_SUBDIR) / (name + ".lua")).string();

        MultiPropertyCmd* multiCmd = new MultiPropertyCmd();

        for (const Entity& entity : entities) {
            std::vector<ScriptEntry> newScripts;

            if (sceneProject->scene && sceneProject->scene->findComponent<ScriptComponent>(entity)) {
                newScripts = sceneProject->scene->getComponent<ScriptComponent>(entity).scripts;
            } else {
                multiCmd->addCommand(std::make_unique<AddComponentCmd>(
                    project, sceneProject->id, entity, ComponentType::ScriptComponent));
            }

            ScriptEntry entry;
            entry.type = ScriptType::LUA;
            entry.enabled = true;
            entry.path = scriptPath;
            entry.className = name;
            newScripts.push_back(entry);

            project->updateScriptProperties(sceneProject, entity, newScripts);

            multiCmd->addCommand(std::make_unique<PropertyCmd<std::vector<ScriptEntry>>>(
                project, sceneProject->id, entity, ComponentType::ScriptComponent, "scripts", newScripts));
        }

        multiCmd->setNoMerge();
        CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
        sceneProject->isModified = true;
    }

}
