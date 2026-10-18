// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#ifndef OBJECTPOOL_H
#define OBJECTPOOL_H

#include <vector>
#include <queue>
#include <functional>
#include <cstdint>
#include <cassert>
#include "Export.h"

namespace doriax {

    // Generic object pool with free-list recycling.
    // Objects are pre-allocated and reused to avoid runtime allocations.
    //
    // Usage:
    //   ObjectPool<Particle> pool(1024);
    //   auto* p = pool.acquire();
    //   p->position = {0, 0, 0};
    //   pool.release(p);
    //
    // The pool supports:
    //   - Pre-allocation with configurable initial capacity
    //   - Auto-grow when pool is exhausted
    //   - O(1) acquire/release via free-list
    //   - Iterator access for batch processing
    //   - Custom reset callbacks on release
    //
    template<typename T>
    class ObjectPool {
    private:
        std::vector<T> objects;
        std::vector<uint32_t> generations;   // Per-slot generation for stale handle detection
        std::vector<bool> active;            // Per-slot active state
        std::queue<uint32_t> freeSlots;      // Free-list of available slot indices
        std::function<void(T&)> resetFunc;   // Optional reset callback
        uint32_t totalAcquired = 0;
        uint32_t totalReleased = 0;

        void grow(uint32_t newCapacity) {
            uint32_t oldSize = static_cast<uint32_t>(objects.size());
            objects.resize(newCapacity);
            generations.resize(newCapacity, 0);
            active.resize(newCapacity, false);

            // Add new slots to free-list
            for (uint32_t i = oldSize; i < newCapacity; ++i){
                freeSlots.push(i);
            }
    }

    public:
        // Handle: lightweight reference to a pooled object.
        // Contains slot index + generation for stale detection.
        struct Handle {
            uint32_t index = UINT32_MAX;
            uint32_t generation = 0;

            bool isValid() const { return index != UINT32_MAX; }
            void invalidate() { index = UINT32_MAX; generation = 0; }
        };

        explicit ObjectPool(uint32_t initialCapacity = 256)
            : resetFunc(nullptr)
        {
            grow(initialCapacity);
        }

        explicit ObjectPool(uint32_t initialCapacity, std::function<void(T&)> resetCallback)
            : resetFunc(resetCallback)
        {
            grow(initialCapacity);
        }

        ~ObjectPool() = default;

        // Non-copyable, movable
        ObjectPool(const ObjectPool&) = delete;
        ObjectPool& operator=(const ObjectPool&) = delete;
        ObjectPool(ObjectPool&&) = default;
        ObjectPool& operator=(ObjectPool&&) = default;

        // Acquire an object from the pool. Returns a pointer to the object.
        // If the pool is exhausted, it auto-grows by 50%.
        T* acquire() {
            if (freeSlots.empty()){
                grow(static_cast<uint32_t>(objects.size() * 1.5f + 64));
            }

            uint32_t slot = freeSlots.front();
            freeSlots.pop();

            active[slot] = true;
            totalAcquired++;

            if (resetFunc){
                resetFunc(objects[slot]);
            }

            return &objects[slot];
        }

        // Acquire an object and return a Handle for later release/validation.
        Handle acquireHandle() {
            T* obj = acquire();
            uint32_t slot = static_cast<uint32_t>(obj - &objects[0]);
            return Handle{slot, generations[slot]};
        }

        // Release an object back to the pool.
        void release(T* obj) {
            if (!obj) return;

            uint32_t slot = static_cast<uint32_t>(obj - &objects[0]);
            assert(slot < objects.size());
            assert(active[slot]);

            active[slot] = false;
            generations[slot]++;
            freeSlots.push(slot);
            totalReleased++;
        }

        // Release via Handle. Returns true if the handle was still valid.
        bool releaseHandle(Handle handle) {
            if (!handle.isValid()) return false;
            if (handle.index >= objects.size()) return false;
            if (generations[handle.index] != handle.generation) return false;
            if (!active[handle.index]) return false;

            active[handle.index] = false;
            generations[handle.index]++;
            freeSlots.push(handle.index);
            totalReleased++;
            return true;
        }

        // Check if a Handle is still valid (object hasn't been released).
        bool isHandleValid(Handle handle) const {
            if (!handle.isValid()) return false;
            if (handle.index >= objects.size()) return false;
            return active[handle.index] && generations[handle.index] == handle.generation;
        }

        // Access object by slot index (unchecked).
        T* getByIndex(uint32_t index) {
            assert(index < objects.size());
            return &objects[index];
        }

        // Access object by Handle (returns nullptr if stale).
        T* getByHandle(Handle handle) {
            if (!isHandleValid(handle)) return nullptr;
            return &objects[handle.index];
        }

        // Iterate all active objects. Lambda receives (T& object, uint32_t index).
        template<typename Func>
        void forEach(Func&& func) {
            for (uint32_t i = 0; i < static_cast<uint32_t>(objects.size()); ++i){
                if (active[i]){
                    func(objects[i], i);
                }
            }
        }

        // Iterate all active objects (const version).
        template<typename Func>
        void forEach(Func&& func) const {
            for (uint32_t i = 0; i < static_cast<uint32_t>(objects.size()); ++i){
                if (active[i]){
                    func(objects[i], i);
                }
            }
        }

        // Release all active objects.
        void releaseAll() {
            for (uint32_t i = 0; i < static_cast<uint32_t>(objects.size()); ++i){
                if (active[i]){
                    active[i] = false;
                    generations[i]++;
                    freeSlots.push(i);
                    totalReleased++;
                }
            }
        }

        // Statistics
        uint32_t capacity() const { return static_cast<uint32_t>(objects.size()); }
        uint32_t activeCount() const { return totalAcquired - totalReleased; }
        uint32_t freeCount() const { return static_cast<uint32_t>(freeSlots.size()); }
        uint32_t getTotalAcquired() const { return totalAcquired; }
        uint32_t getTotalReleased() const { return totalReleased; }

        // Set the reset callback function.
        void setResetFunction(std::function<void(T&)> func) {
            resetFunc = func;
        }
    };

}

#endif //OBJECTPOOL_H
