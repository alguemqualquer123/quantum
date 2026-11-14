// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPONENTMANAGER_H
#define COMPONENTMANAGER_H

#include <any>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
#include <string>
#include <cstring>
#include "ComponentArray.h"
#include "Log.h"

namespace quantum {

	using ComponentId = unsigned char;

	struct ComponentDescriptor {
		std::string name;
		ComponentId id = 0;
	};

	namespace detail {

		template<typename T>
		std::string componentTypeName() {
#if defined(_MSC_VER)
			std::string signature = __FUNCSIG__;
			const std::string marker = "componentTypeName<";
			const size_t begin = signature.find(marker);
			const size_t end = begin == std::string::npos ? std::string::npos
				: signature.find(">", begin + marker.size());
			std::string name = begin == std::string::npos || end == std::string::npos
				? typeid(T).name() : signature.substr(begin + marker.size(), end - begin - marker.size());
#else
			std::string signature = __PRETTY_FUNCTION__;
			const std::string marker = "T = ";
			const size_t begin = signature.find(marker);
			const size_t end = begin == std::string::npos ? std::string::npos
				: signature.find_first_of("];", begin + marker.size());
			std::string name = begin == std::string::npos || end == std::string::npos
				? typeid(T).name() : signature.substr(begin + marker.size(), end - begin - marker.size());
#endif
			while (name.rfind("class ", 0) == 0) name.erase(0, 6);
			while (name.rfind("struct ", 0) == 0) name.erase(0, 7);
			const size_t namespaceSeparator = name.rfind("::");
			if (namespaceSeparator != std::string::npos) name.erase(0, namespaceSeparator + 2);
			return name;
		}

	}


	// Dense index per component type, so an array lookup is a vector subscript instead
	// of hashing typeid().name() on every call.
	class QUANTUM_API ComponentTypeRegistry {
	public:
		static size_t indexOf(const char* mangledName);
	};

	template<typename T>
	inline size_t componentTypeIndex(){
		// Cached per module; the shared registry keeps the value equal across them.
		static const size_t index = ComponentTypeRegistry::indexOf(typeid(T).name());
		return index;
	}


	class QUANTUM_API ComponentManager {
	private:
		struct ComponentEntry {
			std::shared_ptr<ComponentArrayBase> array;
			ComponentId id = 0;
			std::string name;
		};

		// Indexed by componentTypeIndex<T>(); unregistered types keep an empty slot.
		std::vector<ComponentEntry> entries{};
		ComponentId nextComponentIdId{};

		const ComponentEntry* findEntry(size_t index) const {
			if (index >= entries.size() || !entries[index].array){
				return nullptr;
			}
			return &entries[index];
		}

	public:
		template<typename T>
		void registerComponent() {
			const size_t index = componentTypeIndex<T>();

			if (index >= entries.size()){
				entries.resize(index + 1);
			}

			if (entries[index].array){
				Log::error("Registering component type more than once");
				return;
			}

			// Ids stay sequential: they are signature bit positions.
			entries[index].array = std::make_shared<ComponentArray<T>>();
			entries[index].id = nextComponentIdId;
			entries[index].name = detail::componentTypeName<T>();

			++nextComponentIdId;
		}

		template<typename T>
		ComponentId getComponentId() const{
			if (const ComponentEntry* entry = findEntry(componentTypeIndex<T>())){
				return entry->id;
			}

			// Not a default: 0 is the first component's signature bit.
			Log::error("Component not registered before use");
			throw std::out_of_range("Component not registered before use");
		}

		// Raw pointer: copying the shared_ptr cost an atomic pair per access. Never
		// null, since every caller dereferences it right away.
		template<typename T>
		ComponentArray<T>* getComponentArray() const {
			if (const ComponentEntry* entry = findEntry(componentTypeIndex<T>())){
				return static_cast<ComponentArray<T>*>(entry->array.get());
			}

			Log::error("Component not registered before use");
			throw std::out_of_range("Component not registered before use");
		}

		template<typename T>
		void addComponent(Entity entity) {
			getComponentArray<T>()->insert(entity);
		}

		template<typename T>
		void addComponent(Entity entity, const T& component) {
			getComponentArray<T>()->insert(entity, component);
		}

		template<typename T>
		void addComponent(Entity entity, T&& component) {
			using Component = std::remove_cv_t<std::remove_reference_t<T>>;
			getComponentArray<Component>()->insert(entity, std::forward<T>(component));
		}

		template<typename T>
		void removeComponent(Entity entity) {
			getComponentArray<T>()->remove(entity);
		}

		template<typename T>
		T* findComponent(Entity entity) const {
			return getComponentArray<T>()->findComponent(entity);
		}

		template<typename T>
		T& getComponent(Entity entity) const {
			return getComponentArray<T>()->getComponent(entity);
		}

		template<typename T>
	    T* findComponentFromIndex(size_t index) const{
		    return getComponentArray<T>()->findComponentFromIndex(index);
	    }

		template<typename T>
	    T& getComponentFromIndex(size_t index) const {
		    return getComponentArray<T>()->getComponentFromIndex(index);
	    }

		void entityDestroyed(Entity entity) {
			for (auto const& entry : entries) {
				if (entry.array){
					entry.array->entityDestroyed(entity);
				}
			}
		}

		size_t getComponentInstanceCount() const {
			size_t count = 0;
			for (const ComponentEntry& entry : entries) {
				if (entry.array) {
					count += entry.array->size();
				}
			}
			return count;
		}

		size_t getRegisteredComponentCount() const {
			size_t count = 0;
			for (const ComponentEntry& entry : entries) {
				if (entry.array) {
					++count;
				}
			}
			return count;
		}

		size_t getRegistrySlotCount() const { return entries.size(); }
		size_t getRegistryCapacity() const { return entries.capacity(); }

		std::vector<ComponentDescriptor> getRegisteredComponentCatalog() const {
			std::vector<ComponentDescriptor> result;
			result.reserve(nextComponentIdId);
			for (const ComponentEntry& entry : entries) {
				if (entry.array) result.push_back({entry.name, entry.id});
			}
			return result;
		}
	};

}

#endif //COMPONENTMANAGER_H
