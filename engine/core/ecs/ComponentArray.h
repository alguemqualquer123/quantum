// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPONENTARRAY_H
#define COMPONENTARRAY_H
    
#include <array>
#include <cassert>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <functional>
#include <stdexcept>
#include <numeric>
#include <memory>
#include <utility>
#include "Entity.h"
#include "Log.h"

namespace quantum {

    class QUANTUM_API ComponentArrayBase{
    public:
        virtual ~ComponentArrayBase() = default;
        virtual void entityDestroyed(Entity entity) = 0;
        virtual size_t size() const = 0;
    };


    template<typename T>
    class ComponentArray : public ComponentArrayBase{
    private:
        // Marks a slot of indexById that holds no entity. Entity ids reach 2^32 in type but
        // EntityManager caps them well below this, so no real id collides with it.
        static constexpr uint32_t NO_INDEX = 0xFFFFFFFFu;

        static constexpr size_t FIRST_CAPACITY = 256;
        static constexpr size_t GROWTH_FACTOR = 8;

        std::vector<T> componentArray{};
        // Indexes are dense, so the reverse map is a vector parallel to componentArray:
        // getEntity() is the hottest accessor in every draw loop and hashing it cost two
        // lookups per entity per camera per frame.
        std::vector<Entity> entityArray{};
        // Entity id -> index in componentArray, or NO_INDEX. Ids come from a monotonic
        // counter with LIFO recycling, so they stay dense enough for this to track the
        // high-water id rather than the address space; findComponent() sits in every render
        // loop, where the unordered_map it replaces cost a hash and a cache miss per call.
        std::vector<uint32_t> indexById{};

        // Grows indexById so it can address 'entity', doubling past the request the same way
        // a vector would so a burst of inserts does not resize once per entity.
        void reserveSlot(Entity entity){
            const size_t needed = static_cast<size_t>(entity) + 1;
            if (needed <= indexById.size()){
                return;
            }
            size_t target = indexById.empty() ? needed : indexById.size() * 2;
            if (target < needed){
                target = needed;
            }
            indexById.resize(target, NO_INDEX);
        }

        // The index of 'entity', or NO_INDEX when this array holds no component for it.
        uint32_t findIndex(Entity entity) const {
            return static_cast<size_t>(entity) < indexById.size() ? indexById[entity] : NO_INDEX;
        }

        // The vector's own 1.5x policy copies every element it displaces, and a MeshComponent is
        // 48 KB, so appending to a growing scene re-commits the whole array a couple of times per
        // decade of entities: measured at 0.049 ms per insert against 0.012 ms for the same bytes
        // in a buffer that never moved. Growing by eight from a block the usual scene never leaves
        // keeps that to one copy pass, and the pages it reserves are only paid for when touched.
        void growOnce(){
            if (componentArray.size() != componentArray.capacity())
                return;
            componentArray.reserve(componentArray.empty() ? FIRST_CAPACITY :
                componentArray.capacity() * GROWTH_FACTOR);
        }

        template<typename... Args>
        void emplace(Entity entity, Args&&... args) {
            if (findIndex(entity) != NO_INDEX) {
                Log::error("Component added to same entity more than once");
                return;
            }

            growOnce();
            size_t newIndex = componentArray.size();
            componentArray.emplace_back(std::forward<Args>(args)...);
            entityArray.push_back(entity);
            reserveSlot(entity);
            indexById[entity] = static_cast<uint32_t>(newIndex);
        }

        void moveRange(size_t start, size_t length, size_t dst){
            typename std::vector<T>::iterator first, middle, last;
            if (start < dst){
                first  = componentArray.begin() + start;
                middle = first + length;
                last   = componentArray.begin() + dst + length;
            }else{
                first  = componentArray.begin() + dst;
                middle = componentArray.begin() + start;
                last   = middle + length;
            }
            std::rotate(first, middle, last);
        }

    public:
        void insert(Entity entity) {
            emplace(entity);
        }

        void insert(Entity entity, const T& component) {
            emplace(entity, component);
        }

        void insert(Entity entity, T&& component) {
            emplace(entity, std::move(component));
        }

        void remove(Entity entity) {

            const uint32_t removedIndex = findIndex(entity);
            if (removedIndex == NO_INDEX){
                Log::error("Removing non-existent component");
                return;
            }

            size_t indexOfRemovedEntity = removedIndex;
            componentArray.erase(componentArray.begin() + indexOfRemovedEntity);
            entityArray.erase(entityArray.begin() + indexOfRemovedEntity);
            indexById[entity] = NO_INDEX;

            // Erasing shifted both arrays down over the hole, so only the forward map still
            // needs renumbering.
            for (size_t i = indexOfRemovedEntity; i < entityArray.size(); i++){
                indexById[entityArray[i]] = static_cast<uint32_t>(i);
            }
        }

        void moveEntityRangeToIndex(Entity start, Entity end, size_t newIndex){

            const uint32_t startIndexById = findIndex(start);
            const uint32_t endIndexById = findIndex(end);
            if (startIndexById == NO_INDEX || endIndexById == NO_INDEX){
                Log::error("Cannot move entity range absent from the array");
                return;
            }

            size_t startIndex = startIndexById;
            size_t endIndex = endIndexById;
            size_t length = endIndex - startIndex + 1;
            size_t oldIndex = startIndex;

            if (newIndex < 0 || (newIndex + length) > componentArray.size()){
                Log::error("Cannot move entity range out of array");
                return;
            }

            std::vector<Entity> tmp(length);
            for (int i = 0; i < length; i++){
                tmp[i] = entityArray[oldIndex+i];
            }

            if (oldIndex > newIndex){
                for (size_t i = oldIndex+length-1; i > newIndex+length-1; i--){
                    Entity moved = entityArray[i-length];
                    indexById[moved] = static_cast<uint32_t>(i);
                    entityArray[i] = moved;
                }
            }else{
                for (size_t i = oldIndex; i < newIndex; i++){
                    Entity moved = entityArray[i+length];
                    indexById[moved] = static_cast<uint32_t>(i);
                    entityArray[i] = moved;
                }
            }

            for (int i = 0; i < tmp.size(); i++){
                indexById[tmp[i]] = static_cast<uint32_t>(newIndex+i);
                entityArray[newIndex+i] = tmp[i];
            }

            moveRange(oldIndex, length, newIndex);
        }

        void moveEntityToIndex(Entity entity, size_t newIndex){

            const uint32_t found = findIndex(entity);
            if (found == NO_INDEX){
                Log::error("Moving a component the entity does not have");
                return;
            }

            size_t oldIndex = found;

            if (oldIndex > newIndex){
                for (size_t i = oldIndex; i > newIndex; i--){
                    Entity moved = entityArray[i-1];
                    indexById[moved] = static_cast<uint32_t>(i);
                    entityArray[i] = moved;
                }
            }else{
                for (size_t i = oldIndex; i < newIndex; i++){
                    Entity moved = entityArray[i+1];
                    indexById[moved] = static_cast<uint32_t>(i);
                    entityArray[i] = moved;
                }
            }

            indexById[entity] = static_cast<uint32_t>(newIndex);
            entityArray[newIndex] = entity;

            moveRange(oldIndex, 1, newIndex);
        }

        template<typename C>
        void sortByComponent(ComponentArray<C>* otherComponent) {
            // Create a vector of indices [0, 1, ..., N-1]
            std::vector<size_t> indices(componentArray.size());
            std::iota(indices.begin(), indices.end(), 0);

            // Sort indices based on the key in otherComponent
            std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
                Entity ea = entityArray[a];
                Entity eb = entityArray[b];

                bool hasA = otherComponent->hasEntity(ea);
                bool hasB = otherComponent->hasEntity(eb);

                if (!hasA && !hasB) return false;
                if (!hasA) return false;
                if (!hasB) return true;

                size_t indexA = otherComponent->getIndex(ea);
                size_t indexB = otherComponent->getIndex(eb);
                return indexA < indexB;
            });

            // Create new arrays/maps
            std::vector<T> newComponentArray(componentArray.size());
            std::vector<Entity> newEntityArray(componentArray.size());

            for (size_t newIndex = 0; newIndex < indices.size(); ++newIndex) {
                size_t oldIndex = indices[newIndex];
                Entity entity = entityArray[oldIndex];
                newComponentArray[newIndex] = componentArray[oldIndex];
                newEntityArray[newIndex] = entity;
                indexById[entity] = static_cast<uint32_t>(newIndex);
            }

            // Assign new arrays/maps back
            componentArray = std::move(newComponentArray);
            entityArray = std::move(newEntityArray);
        }

        bool hasEntity(Entity entity) {
            return findIndex(entity) != NO_INDEX;
        }

        T* findComponent(Entity entity) {
            const uint32_t index = findIndex(entity);

            if (index == NO_INDEX) {
                 return NULL;
            }

            return &componentArray[index];
        }

        T& getComponent(Entity entity) {
            const uint32_t index = findIndex(entity);

            if (index == NO_INDEX){
                Log::error("Retrieving non-existent component");
                throw std::out_of_range("Retrieving non-existent component");
            }

            return componentArray[index];
        }

        T* findComponentFromIndex(size_t index) {
            if (index >= componentArray.size()){
                return NULL;
            }

            return &componentArray[index];
        }

        T& getComponentFromIndex(size_t index) {
            try{

                return componentArray.at(index);

            }catch (const std::out_of_range& e){
                Log::error("Retrieving non-existent component: %s", e.what());
                throw;
            }
        }

        size_t getIndex(Entity entity){
            const uint32_t index = findIndex(entity);

            if (index == NO_INDEX){
                Log::error("Retrieving non-existent component");
                throw std::out_of_range("Retrieving non-existent component");
            }

            return index;
        }

        Entity getEntity(size_t index){
            if (index >= entityArray.size()){
                Log::error("Entity not found");
                return NULL_ENTITY;
            }

            return entityArray[index];
        }

        size_t size() const override {
            return componentArray.size();
        }

        void entityDestroyed(Entity entity) override {
            if (findIndex(entity) != NO_INDEX) {
                remove(entity);
            }
        }

        void iterateAll(std::function<void(const T& component)> lambda){
              for(size_t i=0;i<componentArray.size();i++){
                lambda(componentArray[i]);
              }
        }
    };

}

#endif //COMPONENTARRAY_H
