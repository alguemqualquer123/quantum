// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

// Sha1.h - streaming SHA-1 used by doriax-resource-server for content hashing.
// Self-contained (standard library only); deliberately NOT shared with the
// editor's editor/util/SHA1.h so the server target stays standalone.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

namespace doriax::rs {

class Sha1 {
public:
    Sha1();

    void update(const void* data, size_t length);

    // Finalizes the stream and returns the lowercase hex digest. The object
    // must not be updated after this call.
    std::string hexDigest();

private:
    void transform(const uint8_t* block);

    uint32_t h_[5];
    uint64_t totalBytes_;
    uint8_t buf_[64];
    size_t bufLen_;
};

// Hashes a whole file in chunks. Returns false and fills errorOut on failure
// (missing file, read error).
bool hashFile(const std::string& pathUtf8, std::string& hexOut, std::string& errorOut);

}
