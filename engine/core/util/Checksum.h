// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef QUANTUM_CHECKSUM_H
#define QUANTUM_CHECKSUM_H

#include "Export.h"

#include <cstddef>
#include <string>

namespace quantum{

    // Integrity, not authenticity: these two are what lets a package manager notice a truncated
    // download or a file that changed under it. Neither says who wrote the bytes.
    // Hand-rolled on purpose - no zlib/openssl is vendored here and the plugin system must not pull
    // a new dependency in (AGENTS.md rule 6). SHA-256 is FIPS 180-4 and CRC-32 is the PKZIP polynomial.

    // Lowercase hex, 64 characters.
    QUANTUM_API std::string sha256Hex(const unsigned char* data, size_t size);
    QUANTUM_API std::string sha256Hex(const std::string& text);

    // Reads in chunks, so hashing a 40 MB package costs 64 KB, not 40 MB.
    // Returns an empty string and fills errorOut when the file cannot be opened or read.
    QUANTUM_API std::string sha256HexOfFile(const std::string& path, std::string& errorOut);

    // CRC-32 as PKZIP stores it (reflected 0xEDB88320, initial 0xFFFFFFFF, final xor).
    QUANTUM_API unsigned int crc32Of(const unsigned char* data, size_t size);

}

#endif //QUANTUM_CHECKSUM_H
