// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef GRAPH_READER_H
#define GRAPH_READER_H

#include "Graph.h"

#include <cstdint>
#include <string>
#include <vector>

namespace quantum {
namespace graph {

    struct QUANTUM_API GraphBlobInfo {
        uint16_t version = 0;
        uint16_t flags = 0;
        uint32_t blobSize = 0;
        uint64_t sourceHash = 0;
    };

    // Decodes a DGR1 blob. Every read is bounds-checked, so a truncated or
    // hand-edited .gdat fails with an error instead of reading out of bounds.
    class QUANTUM_API GraphReader {
    public:
        static bool read(const std::vector<uint8_t>& data, GraphAsset& out,
                         GraphBlobInfo& info, std::string& error);
        static bool readFile(const std::string& path, GraphAsset& out,
                             GraphBlobInfo& info, std::string& error);
    };

}
}

#endif //GRAPH_READER_H
