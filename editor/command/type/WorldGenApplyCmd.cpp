// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "WorldGenApplyCmd.h"

#include <utility>

using namespace quantum;

editor::WorldGenApplyCmd::WorldGenApplyCmd(Project* project, uint32_t sceneId, Entity entity,
                                           const std::string& fromPath, const std::string& toPath){
    this->project = project;
    this->sceneId = sceneId;
    this->entity = entity;
    this->ref = TerrainMapRef(TerrainMapTarget::HeightMap);
    this->fromPath = fromPath;
    this->toPath = toPath;
}

void editor::WorldGenApplyCmd::apply(const std::string& expect, const std::string& target,
                                     bool restoreModifiedState){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene->isEntityCreated(entity)){
        return;
    }
    TerrainComponent* terrain = sceneProject->scene->findComponent<TerrainComponent>(entity);
    if (!terrain){
        return;
    }

    Texture* texture = TerrainMapUtils::findTexture(*terrain, ref);
    // Another command, a brush stroke or an undo may have moved the map on since; pointing it at a
    // different file from here would silently throw that work away.
    if (!texture || texture->getPath(0) != expect){
        return;
    }

    texture->setPath(target);
    // The terrain editor keeps a file-backed map's pixels after upload so a brush stroke can read
    // them; setPath arms the release, so undo the same way applySnapshotToTexture does.
    texture->setReleaseDataAfterLoad(false);
    TerrainMapUtils::refresh(sceneProject, entity, ref);

    if (project->isEntityInBundle(sceneId, entity)){
        project->bundlePropertyChanged(sceneId, entity, ComponentType::TerrainComponent,
                                       {TerrainMapUtils::getPropertyName(ref)});
    }

    if (restoreModifiedState){
        sceneProject->isModified = wasModified;
    }else{
        sceneProject->isModified = true;
    }
}

bool editor::WorldGenApplyCmd::execute(){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject){
        return false;
    }

    wasModified = sceneProject->isModified;
    apply(fromPath, toPath, false);
    return true;
}

void editor::WorldGenApplyCmd::undo(){
    apply(toPath, fromPath, true);
}

bool editor::WorldGenApplyCmd::mergeWith(Command* otherCommand){
    return false;
}
