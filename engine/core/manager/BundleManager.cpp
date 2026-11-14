// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "BundleManager.h"
#include "Scene.h"
#include "EntityHandle.h"
#include "Log.h"
#include "LuaBinding.h"

#include <algorithm>
#include <exception>
#include <unordered_set>

using namespace quantum;

std::vector<BundleManager::BundleEntry> BundleManager::entries;
std::function<void(Scene*)> BundleManager::scriptStarter = LuaBinding::initializeLuaScripts;
std::function<void(Scene*, Entity)> BundleManager::scriptStopper = [](Scene* scene, Entity entity) {
    LuaBinding::cleanupLuaScripts(scene, entity);
};
std::vector<std::pair<Scene*, Entity>> BundleManager::stoppingEntities;

namespace {

void rollbackSpawnedEntities(Scene* scene, const std::unordered_set<Entity>& beforeSet, Entity root) {
    if (!scene)
        return;

    std::vector<Entity> afterEntities = scene->getEntityList();
    for (auto it = afterEntities.rbegin(); it != afterEntities.rend(); ++it) {
        Entity e = *it;
        if (e != root && beforeSet.find(e) == beforeSet.end() && scene->isEntityCreated(e))
            scene->destroyEntity(e);
    }

    if (root != NULL_ENTITY && scene->isEntityCreated(root))
        scene->destroyEntity(root);
}

// a branch is contiguous in the Transform array, up to findBranchLastIndex
std::vector<Entity> withDescendants(Scene* scene, const std::vector<Entity>& entities) {
    std::vector<Entity> result;
    std::unordered_set<Entity> added;
    auto transforms = scene->getComponentArray<Transform>();

    for (Entity entity : entities) {
        if (!added.insert(entity).second)
            continue;
        result.push_back(entity);

        if (!scene->isEntityCreated(entity) || !scene->findComponent<Transform>(entity))
            continue;
        const size_t last = scene->findBranchLastIndex(entity);
        for (size_t i = transforms->getIndex(entity) + 1; i <= last; i++) {
            Entity descendant = transforms->getEntity(i);
            if (added.insert(descendant).second)
                result.push_back(descendant);
        }
    }

    return result;
}

}

std::vector<BundleManager::BundleInstance>& BundleManager::getInstances() {
    // never freed, a static Scene can be destroyed after it
    static std::vector<BundleInstance>* instances = new std::vector<BundleInstance>();
    return *instances;
}

BundleManager::BundleEntry* BundleManager::findEntry(uint32_t id) {
    for (auto& entry : entries) {
        if (entry.id == id)
            return &entry;
    }
    return nullptr;
}

BundleManager::BundleEntry* BundleManager::findEntry(const std::string& name) {
    for (auto& entry : entries) {
        if (entry.name == name)
            return &entry;
    }
    return nullptr;
}

std::vector<BundleManager::BundleInstance>::iterator BundleManager::findInstance(Scene* scene, Entity rootEntity) {
    auto& instances = getInstances();
    return std::find_if(instances.begin(), instances.end(), [&](const BundleInstance& instance) {
        return instance.scene == scene && instance.rootEntity == rootEntity;
    });
}

void BundleManager::registerBundle(uint32_t id, const std::string& name, std::function<bool(Scene*, Entity)> factory, std::function<bool(Scene*, Entity)> destroyer) {
    if (BundleEntry* entry = findEntry(id)) {
        entry->name = name;
        entry->factory = std::move(factory);
        entry->destroyer = std::move(destroyer);
        return;
    }
    entries.push_back({id, name, std::move(factory), std::move(destroyer)});
}

Entity BundleManager::createBundle(const std::string& name, Scene* scene) {
    return createBundle(name, scene, NULL_ENTITY);
}

Entity BundleManager::createBundle(uint32_t id, Scene* scene) {
    return instantiate(id, scene, NULL_ENTITY);
}

Entity BundleManager::createBundle(const std::string& name, Scene* scene, const std::string& parentName) {
    BundleEntry* entry = findEntry(name);
    if (!entry) {
        Log::error("BundleManager: bundle '%s' not found", name.c_str());
        return NULL_ENTITY;
    }
    return createBundle(entry->id, scene, parentName);
}

Entity BundleManager::createBundle(uint32_t id, Scene* scene, const std::string& parentName) {
    if (!scene) {
        Log::error("BundleManager: scene is null");
        return NULL_ENTITY;
    }
    Entity parent = scene->findEntity(parentName);
    if (parent == NULL_ENTITY) {
        Log::error("BundleManager: parent entity '%s' not found in the given scene", parentName.c_str());
        return NULL_ENTITY;
    }
    return instantiate(id, scene, parent);
}

Entity BundleManager::createBundle(const std::string& name, Scene* scene, Entity parent) {
    BundleEntry* entry = findEntry(name);
    if (!entry) {
        Log::error("BundleManager: bundle '%s' not found", name.c_str());
        return NULL_ENTITY;
    }
    return instantiate(entry->id, scene, parent);
}

Entity BundleManager::createBundle(uint32_t id, Scene* scene, Entity parent) {
    return instantiate(id, scene, parent);
}

Entity BundleManager::createBundle(const std::string& name, const EntityHandle& parent) {
    return createBundle(name, parent.getScene(), parent.getEntity());
}

Entity BundleManager::createBundle(uint32_t id, const EntityHandle& parent) {
    return instantiate(id, parent.getScene(), parent.getEntity());
}

Entity BundleManager::instantiate(uint32_t id, Scene* scene, Entity parent) {
    if (!scene) {
        Log::error("BundleManager: scene is null");
        return NULL_ENTITY;
    }
    if (parent != NULL_ENTITY && !scene->isEntityCreated(parent)) {
        Log::error("BundleManager: parent entity %u does not exist in the given scene", parent);
        return NULL_ENTITY;
    }

    BundleEntry* entry = findEntry(id);
    if (!entry) {
        Log::error("BundleManager: bundle id %u not found", id);
        return NULL_ENTITY;
    }

    std::vector<Entity> beforeEntities = scene->getEntityList();
    std::unordered_set<Entity> beforeSet(beforeEntities.begin(), beforeEntities.end());

    Entity root = scene->createEntity();

    bool ok = false;
    try {
        ok = entry->factory && entry->factory(scene, root);
    } catch (const std::exception& e) {
        Log::error("BundleManager: factory for bundle id %u threw an exception: %s", id, e.what());
    } catch (...) {
        Log::error("BundleManager: factory for bundle id %u threw an exception", id);
    }

    if (!ok) {
        Log::error("BundleManager: factory failed for bundle id %u", id);
        rollbackSpawnedEntities(scene, beforeSet, root);
        return NULL_ENTITY;
    }

    // after the factory, which is what gives the root its Transform
    if (parent != NULL_ENTITY)
        scene->addEntityChild(parent, root, false);

    BundleInstance instance;
    instance.rootEntity = root;
    instance.scene = scene;
    instance.bundleId = id;
    instance.entities.push_back(root);
    for (Entity e : scene->getEntityList()) {
        if (e != root && beforeSet.find(e) == beforeSet.end())
            instance.entities.push_back(e);
    }

    getInstances().push_back(std::move(instance));

    // tracked first, init() can destroy it
    scriptStarter(scene);

    if (!scene->isEntityCreated(root))
        return NULL_ENTITY;
    return root;
}

bool BundleManager::destroyBundle(Scene* scene, Entity rootEntity) {
    auto& instances = getInstances();
    auto it = findInstance(scene, rootEntity);
    if (it == instances.end()) {
        Log::error("BundleManager: bundle instance with root %u not found in scene", rootEntity);
        return false;
    }

    // removed before any script runs, it can spawn or destroy too
    BundleInstance instance = std::move(*it);
    instances.erase(it);

    // what was parented under it goes too, a spawned bundle through its own destroy
    std::vector<Entity> entities = withDescendants(scene, instance.entities);
    while (entities.size() > instance.entities.size()) {
        auto child = std::find_if(instances.begin(), instances.end(), [&](const BundleInstance& other) {
            return other.scene == scene && std::find(entities.begin(), entities.end(), other.rootEntity) != entities.end();
        });
        if (child == instances.end())
            break;
        destroyBundle(scene, child->rootEntity);
        entities = withDescendants(scene, instance.entities);
    }

    const size_t stoppingCount = stoppingEntities.size();
    for (Entity entity : entities) {
        if (scene->isEntityCreated(entity)) {
            stoppingEntities.push_back({scene, entity});
            scriptStopper(scene, entity);
        }
    }

    bool result = true;
    BundleEntry* entry = findEntry(instance.bundleId);
    if (entry && entry->destroyer) {
        result = entry->destroyer(scene, rootEntity);
    } else {
        for (auto eit = entities.rbegin(); eit != entities.rend(); ++eit) {
            if (scene->isEntityCreated(*eit))
                scene->destroyEntity(*eit);
        }
    }

    stoppingEntities.resize(std::min(stoppingEntities.size(), stoppingCount));
    return result;
}

bool BundleManager::isStopping(Scene* scene, Entity entity) {
    return std::find(stoppingEntities.begin(), stoppingEntities.end(), std::make_pair(scene, entity)) != stoppingEntities.end();
}

void BundleManager::setScriptCallbacks(std::function<void(Scene*)> start, std::function<void(Scene*, Entity)> stop) {
    scriptStarter = std::move(start);
    scriptStopper = std::move(stop);
}

uint32_t BundleManager::getBundleId(const std::string& name) {
    BundleEntry* entry = findEntry(name);
    return entry ? entry->id : 0;
}

std::string BundleManager::getBundleName(uint32_t id) {
    BundleEntry* entry = findEntry(id);
    return entry ? entry->name : "";
}

std::vector<std::string> BundleManager::getBundleNames() {
    std::vector<std::string> names;
    names.reserve(entries.size());
    for (const auto& entry : entries)
        names.push_back(entry.name);
    return names;
}

int BundleManager::getBundleCount() {
    return (int)entries.size();
}

void BundleManager::destroyAllInstances(Scene* scene) {
    auto& instances = getInstances();
    std::vector<Entity> roots;
    for (const auto& inst : instances) {
        if (inst.scene == scene)
            roots.push_back(inst.rootEntity);
    }
    for (Entity root : roots) {
        // a child is gone with its parent
        if (findInstance(scene, root) != instances.end())
            destroyBundle(scene, root);
    }
}

void BundleManager::forgetInstances(Scene* scene) {
    auto& instances = getInstances();
    instances.erase(std::remove_if(instances.begin(), instances.end(), [scene](const BundleInstance& instance) {
        return instance.scene == scene;
    }), instances.end());
}

void BundleManager::clearAll() {
    entries.clear();
    getInstances().clear();
    stoppingEntities.clear();
}
