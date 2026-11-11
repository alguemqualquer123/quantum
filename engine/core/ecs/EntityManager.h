// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef ENTITYMANAGER_H
#define ENTITYMANAGER_H

#include <vector>
#include <string>
#include <cstdint>
#include <limits>
#include <deque>
#include <algorithm>
#include "Entity.h"
#include "Signature.h"
#include "Log.h"

namespace quantum{

    struct EntityMetadata{
        std::string name;
        Signature signature;
        // Generation counter: incremented each time an entity slot is recycled.
        // Allows detection of stale entity handles without full validation.
        uint32_t generation = 0;
        // Whether the slot at index == entity id holds an entity right now. It lives in the
        // record instead of a parallel array so it shares the cache line the readers want.
        bool created = false;
    };

    class QUANTUM_API EntityManager {
    private:
        static constexpr Entity FIRST_SYSTEM_ENTITY = 1;
        static constexpr Entity LAST_SYSTEM_ENTITY  = 1000;
        static constexpr Entity FIRST_USER_ENTITY   = LAST_SYSTEM_ENTITY + 1;

        // Ceiling of the flat storage below, and so the largest id an entity can have.
        // Storage is one record per id, so any id would cost id * sizeof(EntityMetadata)
        // bytes; this bound keeps that at ~48 MB (48-byte records on MSVC) even if a corrupt
        // scene asks for a huge id. Reaching it means a million entities in one scene, which
        // the tree this replaces could not serve either.
        static constexpr Entity MAX_STORED_ENTITY = (1u << 20) - 1;

        Entity lastUserEntity = LAST_SYSTEM_ENTITY; // next user entity starts at 1001

        // Per-entity metadata indexed by the entity id itself, so a lookup is one bounds
        // check plus an array read instead of the red-black tree walk std::map cost per
        // entity per camera. Slots are never reclaimed: ids come from a monotonic counter
        // plus LIFO recycling, so they stay dense and the vector tracks the high-water id.
        std::vector<EntityMetadata> metadata;

        // Records with created == true, which is what map::size() used to report.
        size_t entryCount = 0;

        // Free-list: destroyed user entity IDs waiting to be recycled.
        // Populated in LIFO order (most recently destroyed reused first) for cache friendliness.
        std::deque<Entity> freeList;

        // Maximum number of free-list entries to keep before falling back to monotonic allocation.
        // Prevents unbounded memory growth when entities are created/destroyed in bursts.
        static constexpr size_t MAX_FREE_LIST_SIZE = 4096;

        // The record of a living entity, or null when the id is unknown or was erased.
        const EntityMetadata* find(Entity entity) const {
            if (static_cast<size_t>(entity) < metadata.size() && metadata[entity].created) {
                return &metadata[entity];
            }
            return nullptr;
        }

        EntityMetadata* find(Entity entity) {
            if (static_cast<size_t>(entity) < metadata.size() && metadata[entity].created) {
                return &metadata[entity];
            }
            return nullptr;
        }

        // Grows the storage to hold 'entity'. The size doubles once the id is past the current
        // size, so a single id far above the population cannot allocate a gigantic vector in
        // one step; the first growth already covers the whole system range.
        void growTo(Entity entity) {
            if (static_cast<size_t>(entity) < metadata.size()) {
                return;
            }

            size_t target = static_cast<size_t>(entity) + 1;
            const size_t geometric = metadata.size() * 2;
            if (geometric > target) {
                target = geometric;
            }
            const size_t minimum = static_cast<size_t>(FIRST_USER_ENTITY) + 1;
            if (target < minimum) {
                target = minimum;
            }
            const size_t maximum = static_cast<size_t>(MAX_STORED_ENTITY) + 1;
            if (target > maximum) {
                target = maximum;
            }
            metadata.resize(target);
        }

        // What map::operator[] did: create the record when the id is unknown, leave an existing
        // one as it is. Null only when the id is above MAX_STORED_ENTITY, which is logged.
        EntityMetadata* getOrCreate(Entity entity) {
            if (entity > MAX_STORED_ENTITY) {
                Log::error("Entity id is above the maximum supported entity id");
                return nullptr;
            }
            growTo(entity);

            EntityMetadata& record = metadata[entity];
            if (!record.created) {
                // A recycled slot keeps its generation, while authored metadata is rebuilt by
                // the caller as components and the name are assigned.
                record.created = true;
                ++entryCount;
            }
            return &record;
        }

    public:

        static constexpr Entity firstSystemEntity() { return FIRST_SYSTEM_ENTITY; }
        static constexpr Entity lastSystemEntity()  { return LAST_SYSTEM_ENTITY; }
        static constexpr Entity firstUserEntity()   { return FIRST_USER_ENTITY; }
        static constexpr Entity maxStoredEntity()   { return MAX_STORED_ENTITY; }

        bool recreateEntity(Entity entity) { // for internal editor use only
            if (!isCreated(entity)){
                auto free = std::find(freeList.begin(), freeList.end(), entity);
                if (free != freeList.end()) {
                    freeList.erase(free);
                }
                return getOrCreate(entity) != nullptr;
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

        size_t getStorageSlotCount() const { return metadata.size(); }
        size_t getStorageCapacity() const { return metadata.capacity(); }

        // Returns the generation counter for an entity. Useful for stale handle detection.
        uint32_t getGeneration(Entity entity) const {
            return static_cast<size_t>(entity) < metadata.size() ? metadata[entity].generation : 0;
        }

        // Checks whether a handle is still valid by comparing stored generation.
        // Usage: store {entity, generation} at handle creation time, then call this each access.
        bool isHandleValid(Entity entity, uint32_t storedGeneration) const {
            const EntityMetadata* record = find(entity);
            return record && record->generation == storedGeneration;
        }

        // System entity in range [1,100], first free slot
        Entity createSystemEntity() {
            for (Entity e = FIRST_SYSTEM_ENTITY; e <= LAST_SYSTEM_ENTITY; ++e){
                if (!isCreated(e)){
                    getOrCreate(e);
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
                if (getOrCreate(recycled) == nullptr){
                    return NULL_ENTITY;
                }
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

                if (!isCreated(candidate)){
                    break;
                }
            }

            if (getOrCreate(candidate) == nullptr){
                // The ceiling is out of reach for this allocator; the counter stays put.
                return NULL_ENTITY;
            }

            lastUserEntity = candidate;
            return candidate;
        }

        bool isCreated(Entity entity) const {
            return find(entity) != nullptr;
        }

        void destroy(Entity entity) {
            EntityMetadata* record = find(entity);
            if (record == nullptr) return;

            // Only recycle user entities (system entities don't go through free-list)
            // Every destruction invalidates the current handle, including IDs that cannot be
            // retained in the bounded free-list. The slot is marked dead so entity enumeration
            // and isCreated() describe live ECS state while the generation survives recycling.
            ++record->generation;
            record->name.clear();
            record->signature.reset();
            record->created = false;
            --entryCount;
            if (entity >= FIRST_USER_ENTITY && freeList.size() < MAX_FREE_LIST_SIZE){
                freeList.push_back(entity);
            }
        }

        // Returns the current number of recycled entity slots in the free-list.
        size_t getFreeListSize() const {
            return freeList.size();
        }

        // Live entity ids. Recycled slots are marked dead until createUserEntity() claims them.
        size_t getLivingEntityCount() const {
            return entryCount;
        }

        std::vector<Entity> getEntityList() const{
            std::vector<Entity> list;
            list.reserve(entryCount);
            // Ascending like the map it replaces, and only the slots that hold an entity:
            // the created flag skips both the never-used slots geometric growth leaves and
            // the ones destroy() erased.
            for (size_t i = 0; i < metadata.size(); ++i){
                if (metadata[i].created){
                    list.push_back(static_cast<Entity>(i));
                }
            }
            return list;
        }

        void setSignature(Entity entity, Signature signature) {
            EntityMetadata* record = find(entity);
            if (record == nullptr){
                Log::error("Entity does not exist to set signature");
                record = getOrCreate(entity);
                if (record != nullptr){
                    record->signature = signature;
                }
                return;
            }
            record->signature = signature;
        }

        // getSignature() sits in the per-entity render loop, so this is the lookup the flat
        // storage was paid for: one bounds check and one array read, no tree walk.
        Signature getSignature(Entity entity) const{
            const EntityMetadata* record = find(entity);
            if (record == nullptr){
                Log::error("Entity does not exist to get signature");
                return Signature();
            }
            return record->signature;
        }

        void setName(Entity entity, const std::string& name) {
            EntityMetadata* record = find(entity);
            if (record == nullptr){
                Log::error("Entity does not exist to set name");
                record = getOrCreate(entity);
                if (record != nullptr){
                    record->name = name;
                }
                return;
            }
            record->name = name;
        }

        std::string getName(Entity entity) const{
            const EntityMetadata* record = find(entity);
            if (record == nullptr){
                Log::error("Entity does not exist to get name");
                return std::string();
            }
            return record->name;
        }
    };

}

#endif //ENTITYMANAGER_H
