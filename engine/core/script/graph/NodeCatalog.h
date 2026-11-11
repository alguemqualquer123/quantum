// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef NODE_CATALOG_H
#define NODE_CATALOG_H

#include "Graph.h"

#include <map>
#include <string>
#include <vector>

namespace quantum {
namespace graph {

    struct QUANTUM_API GraphNodeDef {
        std::string kind;
        std::string title;
        std::string category;
        std::vector<GraphPinDef> pins;
        bool pure = false;
        // Exec outputs run in order and the chain after the node waits for all of
        // them (flow.sequence and the node-palette entries built from it).
        bool sequence = false;
    };

    // Built-in node kinds the runtime knows how to execute. Reflection-driven
    // method kinds ("method.<Class>.<method>") are added to the catalog by the
    // editor/code generation step in a later phase; until then they are reported
    // as unknown kinds by the schema.
    class QUANTUM_API NodeCatalog {
    public:
        static const NodeCatalog& core();

        const GraphNodeDef* find(const std::string& kind) const;

        // Resolve the pins of one node instance. Variable nodes look up the
        // type in the asset; unknown kinds resolve to no pins.
        std::vector<GraphPinDef> resolvePins(const GraphNode& node, const GraphAsset* asset) const;

        const std::vector<GraphNodeDef>& getDefinitions() const { return defs; }

        // Number of data inputs / data outputs of a kind. Handlers read pins by
        // name through this, so a catalog edit cannot silently shift an ordinal.
        int dataInputOrdinal(const std::string& kind, const std::string& pin) const;
        int dataOutputOrdinal(const std::string& kind, const std::string& pin) const;

    private:
        std::vector<GraphNodeDef> defs;
        std::map<std::string, size_t> index;
        std::map<std::string, std::map<std::string, int> > inputOrdinals;
        std::map<std::string, std::map<std::string, int> > outputOrdinals;

        NodeCatalog();

        void add(const std::string& kind, const std::string& title, const std::string& category,
                 bool pure, std::vector<GraphPinDef> pins, bool sequence = false);
    };

    // The runtime event id one event node binds to. Filtered events carry it in the
    // id ("keydown:87"), custom ones use the "custom:" prefix, so the engine can
    // subscribe only to the key/button/axis the graph actually asked for.
    // Returns false for a kind that is not an event entry node.
    QUANTUM_API bool graphEventIdForNode(const GraphNode& node, std::string& id);

}
}

#endif //NODE_CATALOG_H
