// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef RESOURCEPACK_H
#define RESOURCEPACK_H

#include "Export.h"

#include <array>
#include <string>
#include <vector>

namespace quantum {

    // Native exports can pack assets and Lua sources into a single "resources.pak"
    // (Project Settings > Build). Entries are obfuscated, not encrypted. Data::open()
    // reads through it; File keeps to the filesystem and the editor never opens a pack.
    class QUANTUM_API ResourcePack {
    public:
        // Paths are the ones Data::open() takes: asset://, lua:// or asset-relative
        static bool contains(const std::string& path);
        static bool read(const std::string& path, std::vector<unsigned char>& outData);

        // Verifies an existing DXPK1/DXPK2 pack without changing the active runtime
        // pack. DXPK2 also checks each unobfuscated body against its SHA-256 digest.
        static bool verify(const std::string& packPath);
    };

}

#endif
