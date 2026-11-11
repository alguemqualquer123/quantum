// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef GRAPH_COMPILER_H
#define GRAPH_COMPILER_H

#include "Graph.h"

#include <cstdint>
#include <string>
#include <vector>

namespace quantum {
namespace graph {

    // Serializes a validated asset into the binary DGR1 blob consumed by the
    // runtime. Layout is deterministic: the same asset always produces the same
    // bytes, which keeps exported projects reproducible.
    class QUANTUM_API GraphCompiler {
    public:
        static bool compile(const GraphAsset& asset, uint64_t sourceHash,
                            std::vector<uint8_t>& out, std::string& error);

        static bool writeFile(const std::string& path, const GraphAsset& asset,
                              uint64_t sourceHash, std::string& error);
    };

}
}

#endif //GRAPH_COMPILER_H
