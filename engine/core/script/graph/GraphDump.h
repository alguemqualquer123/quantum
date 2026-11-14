// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef GRAPH_DUMP_H
#define GRAPH_DUMP_H

#include "Graph.h"

#include <string>

namespace quantum {
namespace graph {

    // Human-readable dump used by headless verification ("graph --dump") and by
    // the editor's debug panel. Not a serialization format.
    class QUANTUM_API GraphDump {
    public:
        static std::string toString(const GraphAsset& asset);
    };

}
}

#endif //GRAPH_DUMP_H
