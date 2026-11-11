// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef GRAPH_SCHEMA_H
#define GRAPH_SCHEMA_H

#include "Graph.h"
#include "NodeCatalog.h"

#include <string>
#include <vector>

namespace quantum {
namespace graph {

    enum class GraphDiagnosticSeverity : uint8_t {
        Warning = 0,
        Error
    };

    struct QUANTUM_API GraphDiagnostic {
        GraphDiagnosticSeverity severity = GraphDiagnosticSeverity::Error;
        uint32_t nodeId = 0;
        std::string message;
    };

    // Authoring-time validation: unique ids, known kinds, edges that connect an
    // output to an input of compatible kind/type, one edge per data input, and
    // variable references that exist. Runs before compilation so the .gdat is
    // never written from an inconsistent asset.
    class QUANTUM_API GraphSchema {
    public:
        static void validate(const GraphAsset& asset, std::vector<GraphDiagnostic>& diagnostics);

        static bool hasErrors(const std::vector<GraphDiagnostic>& diagnostics);
        static std::string format(const std::vector<GraphDiagnostic>& diagnostics);

        static bool typesCompatible(GraphValueType from, GraphValueType to);
    };

}
}

#endif //GRAPH_SCHEMA_H
