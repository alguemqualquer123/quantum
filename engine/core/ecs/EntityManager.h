// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ENTITYMANAGER_H
#define ENTITYMANAGER_H

#include <map>
#include <vector>
#include <string>
#include <cstdint>
#include <limits>
#include <deque>
#include "Entity.h"
#include "Signature.h"
#include "Log.h"

namespace doriax{

    struct EntityMetadata{
        std::string name;
        Signature signature;
        // Generation counter: incremented each time an entity slot is recycled.
        // Allows detection of stale entity handles without full validation.
        uint32_t generation = 0;
    };

    class DORIAX_API EntityManager {
    private:
        static constexpr Entity FIRST_SYSTEM_ENTITY = 1;
        static constexpr Entity LAST_SYSTEM_ENTITY  = 1000;
        static constexpr Entity FIRST_USER_ENTITY   = LAST_SYSTEM_ENTITY + 1;

        Entity lastUserEntity = LAST_SYSTEM_ENTITY; // next user entity starts at 1001
        std::map<Entity, EntityMetadata> metadata;

        // Free-list: destroyed user entity IDs waiting to be recycled.
        // Populated in LIFO order (most recently destroyed reused first) for cache friendliness.
        std::deque<Entity> freeList;

        // Maximum number of free-list entries to keep before falling back to monotonic allocation.
        // Prevents unbounded memory growth when entities are created/destroyed in bursts.
        static constexpr size_t MAX_FREE_LIST_SIZE = 4096;

    public:

        static constexpr Entity firstSystemEntity() { return FIRST_SYSTEM_ENTITY; }
        static constexpr Entity lastSystemEntity()  { return LAST_SYSTEM_ENTITY; }
        static constexpr Entity firstUserEntity()   { return FIRST_USER_ENTITY; }

        bool recreateEntity(Entity entity) { // for internal editor use only
            if (!isCreated(entity)){
                metadata[entity];
                return true;
            }
            return false;
        }

        void setLastUserEntity(Entity entity){
            if (entity < LAST_SYSTEM_ENTITY) {
                this->lastUserEntity = LAST_SYSTEM_ENTITY;
            } else {
                this->lastUserEntity = entity;
            }
        }

        Entity getLastUserEntity() const{
            return this->lastUserEntity;
        }

        // Returns the generation counter for an entity. Useful for stale handle detection.
        uint32_t getGeneration(Entity entity) const {
            auto it = metadata.find(entity);
            if (it == metadata.end()) return 0;
            return it->second.generation;
        }

        // Checks whether a handle is still valid by comparing stored generation.
        // Usage: store {entity, generation} at handle creation time, then call this each access.
        bool isHandleValid(Entity entity, uint32_t storedGeneration) const {
            auto it = metadata.find(entity);
            if (it == metadata.end()) return false;
            return it->second.generation == storedGeneration;
        }

        // System entity in range [1,100], first free slot
        Entity createSystemEntity() {
            for (Entity e = FIRST_SYSTEM_ENTITY; e <= LAST_SYSTEM_ENTITY; ++e){
                if (metadata.count(e) == 0){
                    metadata[e];
                    return e;
                }
            }
            Log::error("No free system entity slot available in range [1..100]");
            return NULL_ENTITY;
        }

        // User entity in range [101..max], with free-list recycling.
        // Destroyed user entity IDs are pushed onto a free-list and reused
        // before allocating new monotonic IDs.
        Entity createUserEntity() {
            // Try recycling from free-list first
            if (!freeList.empty()){
                Entity recycled = freeList.back();
                freeList.pop_back();
                // Generation is already incremented at destroy time
                metadata[recycled];
                return recycled;
            }

            // Fall back to monotonic allocation
            if (lastUserEntity < LAST_SYSTEM_ENTITY){
                lastUserEntity = LAST_SYSTEM_ENTITY;
            }

            Entity candidate = lastUserEntity;
            while (true){
                if (candidate == std::numeric_limits<Entity>::max()){
                    Log::error("User entity counter overflow");
                    return NULL_ENTITY;
                }
                candidate++;

                // Safety: if wrapped into system range by some means, jump to first user id
                if (candidate <= LAST_SYSTEM_ENTITY){
                    candidate = FIRST_USER_ENTITY;
                }

                if (metadata.count(candidate) == 0){
                    break;
                }
            }

            lastUserEntity = candidate;
            metadata[candidate];
            return candidate;
        }

        bool isCreated(Entity entity) const {
            return metadata.count(entity) > 0;
        }

        void destroy(Entity entity) {
            auto it = metadata.find(entity);
            if (it == metadata.end()) return;

            // Only recycle user entities (system entities don't go through free-list)
            if (entity >= FIRST_USER_ENTITY && freeList.size() < MAX_FREE_LIST_SIZE){
                // Increment generation before recycling so stale handles are detected
                it->second.generation++;
                it->second.name.clear();
                it->second.signature.reset();
                freeList.push_back(entity);
                // Keep the metadata entry alive with incremented generation
                // so isHandleValid() correctly rejects old handles
            } else {
                metadata.erase(it);
            }
        }

        // Returns the current number of recycled entity slots in the free-list.
        size_t getFreeListSize() const {
            return freeList.size();
        }

        // Returns an estimate of live (active) user entities.
        size_t getLivingEntityCount() const {
            // Total metadata entries minus system entities minus free-list slots
            size_t systemCount = 0;
            for (Entity e = FIRST_SYSTEM_ENTITY; e <= LAST_SYSTEM_ENTITY; ++e){
                if (metadata.count(e) > 0) systemCount++;
            }
            size_t total = metadata.size();
            if (total < systemCount + freeList.size()) return 0;
            return total - systemCount - freeList.size();
        }

        std::vector<Entity> getEntityList() const{
            std::vector<Entity> list;
            list.reserve(metadata.size());
            for (auto const& [key, val] : metadata){
                list.push_back(key);
            }
            return list;
        }

        void setSignature(Entity entity, Signature signature) {
            if (metadata.count(entity)==0){
                Log::error("Entity does not exist to set signature");
            }
            metadata[entity].signature = signature;
        }

        Signature getSignature(Entity entity) const{
            if (metadata.count(entity)==0){
                Log::error("Entity does not exist to get signature");
                return Signature();
            }
            return metadata.at(entity).signature;
        }

        void setName(Entity entity, const std::string& name) {
            if (metadata.count(entity)==0){
                Log::error("Entity does not exist to set name");
            }
            metadata[entity].name = name;
        }

        std::string getName(Entity entity) const{
            if (metadata.count(entity)==0){
                Log::error("Entity does not exist to get name");
                return std::string();
            }
            return metadata.at(entity).name;
        }
    };

}

#endif //ENTITYMANAGER_H