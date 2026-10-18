//
// (c) Eduardo Doria
// SPDX-License-Identifier: MIT
//

#include "BuiltinModel.h"

#include "builtin_model_bananaman.h"

#include <cstring>
#include <mutex>
#include <unordered_map>
#include <utility>

using namespace doriax;

namespace {

    struct BuiltinModelEntry {
        const char* name;
        const unsigned char* data;
        unsigned long long size;
    };

    const BuiltinModelEntry kBuiltinModels[] = {
        { "BananaMan.glb", doriax_builtin_bananaman_glb_data, doriax_builtin_bananaman_glb_size },
    };

    const BuiltinModelEntry* entryFor(const std::string& path) {
        const size_t prefixLength = std::strlen(BuiltinModels::pathPrefix());
        if (path.compare(0, prefixLength, BuiltinModels::pathPrefix()) != 0) {
            return nullptr;
        }

        const std::string name = path.substr(prefixLength);
        for (const BuiltinModelEntry& entry : kBuiltinModels) {
            if (name == entry.name) {
                return &entry;
            }
        }

        return nullptr;
    }

}

const char* BuiltinModels::pathPrefix() {
    return "builtin://";
}

const char* BuiltinModels::defaultPath() {
    return "builtin://BananaMan.glb";
}

bool BuiltinModels::isBuiltinPath(const std::string& path) {
    const size_t prefixLength = std::strlen(pathPrefix());
    return path.size() > prefixLength && path.compare(0, prefixLength, pathPrefix()) == 0;
}

const std::vector<unsigned char>* BuiltinModels::find(const std::string& path) {
    const BuiltinModelEntry* entry = entryFor(path);
    if (!entry) {
        return nullptr;
    }

    // unordered_map keeps element references stable across rehash, so the pointer
    // handed back here stays valid even when another model is decoded later.
    static std::unordered_map<std::string, std::vector<unsigned char>> decoded;
    static std::mutex decodedMutex;

    std::lock_guard<std::mutex> lock(decodedMutex);
    const auto found = decoded.find(entry->name);
    if (found != decoded.end()) {
        return &found->second;
    }

    std::vector<unsigned char> bytes(entry->data, entry->data + entry->size);
    return &decoded.emplace(entry->name, std::move(bytes)).first->second;
}
