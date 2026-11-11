// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "GraphDump.h"
#include "NodeCatalog.h"

#include <cstdio>

namespace quantum {
namespace graph {

    std::string GraphDump::toString(const GraphAsset& asset) {
        std::string out;
        char buffer[256];

        std::snprintf(buffer, sizeof(buffer), "graph '%s' (schema %u): %u variables, %u nodes, %u edges, %u events, %u comments\n",
                      asset.name.c_str(), asset.version,
                      static_cast<unsigned>(asset.variables.size()),
                      static_cast<unsigned>(asset.nodes.size()),
                      static_cast<unsigned>(asset.edges.size()),
                      static_cast<unsigned>(asset.events.size()),
                      static_cast<unsigned>(asset.comments.size()));
        out += buffer;

        for (const GraphVariable& variable : asset.variables) {
            std::snprintf(buffer, sizeof(buffer), "  var %s : %s = %s%s\n",
                          variable.name.c_str(),
                          graphValueTypeName(variable.type),
                          variable.defaultValue.toDisplayString().c_str(),
                          variable.exposed ? " (exposed)" : "");
            out += buffer;
        }

        const NodeCatalog& catalog = NodeCatalog::core();
        for (const GraphNode& node : asset.nodes) {
            const GraphNodeDef* def = catalog.find(node.kind);
            std::snprintf(buffer, sizeof(buffer), "  node %u %s '%s' at (%.1f, %.1f)%s\n",
                          node.id,
                          node.kind.c_str(),
                          node.title.empty() && def ? def->title.c_str() : node.title.c_str(),
                          node.x, node.y,
                          def ? "" : " [unknown kind]");
            out += buffer;
            for (const GraphParam& param : node.params) {
                std::snprintf(buffer, sizeof(buffer), "      %s = %s\n",
                              param.name.c_str(), param.value.toDisplayString().c_str());
                out += buffer;
            }
        }

        for (const GraphEdge& edge : asset.edges) {
            std::snprintf(buffer, sizeof(buffer), "  edge %u.%s -> %u.%s\n",
                          edge.fromNode, edge.fromPin.c_str(),
                          edge.toNode, edge.toPin.c_str());
            out += buffer;
        }

        for (const GraphEventBinding& binding : asset.events) {
            std::snprintf(buffer, sizeof(buffer), "  event '%s' -> node %u\n",
                          binding.id.c_str(), binding.entryNode);
            out += buffer;
        }

        for (const GraphComment& comment : asset.comments) {
            std::snprintf(buffer, sizeof(buffer), "  comment at (%.1f, %.1f) %.0fx%.0f: %s\n",
                          comment.x, comment.y, comment.width, comment.height,
                          comment.text.c_str());
            out += buffer;
        }

        return out;
    }

}
}
