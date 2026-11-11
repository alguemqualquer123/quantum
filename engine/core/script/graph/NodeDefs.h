// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef NODE_DEFS_H
#define NODE_DEFS_H

#include "ExecutableGraph.h"

#include <string>

namespace quantum {
namespace graph {

    struct GraphOpDef {
        std::string kind;
        BoundOpKind opKind = BoundOpKind::Exec;
        GraphExecFn exec = nullptr;
        GraphPureFn pure = nullptr;
    };

    // Runtime operation table. Each kind also exists in NodeCatalog (pins) —
    // both must stay in sync; prepare() refuses a kind without an operation.
    const GraphOpDef* findGraphOp(const std::string& kind);

}
}

#endif //NODE_DEFS_H
