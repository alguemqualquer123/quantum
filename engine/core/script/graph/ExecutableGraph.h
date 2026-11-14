// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef EXECUTABLE_GRAPH_H
#define EXECUTABLE_GRAPH_H

#include "Graph.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace quantum {
namespace graph {

    class GraphRuntime;
    struct BoundNode;

    using GraphExecFn = int (*)(GraphRuntime& runtime, const BoundNode& node);
    using GraphPureFn = void (*)(GraphRuntime& runtime, const BoundNode& node);

    enum class BoundOpKind : uint8_t {
        Exec = 0,
        Pure
    };

    // A data input resolved to the register it reads. pureNode >= 0 means the
    // register belongs to a pure node that must be evaluated on demand.
    struct BoundInput {
        int reg = -1;
        int pureNode = -1;
    };

    // Prepared node: pin ordinals follow the resolved pin order (data inputs,
    // data outputs and exec outputs are each counted separately). The names are
    // kept beside them so a handler can ask for "position" instead of a literal
    // number - variadic nodes (sequence, switch, custom events) regenerate their
    // pins per instance, so a catalog-wide ordinal would be meaningless there.
    struct BoundNode {
        uint32_t index = 0;
        uint32_t id = 0;
        const GraphNode* source = nullptr;
        BoundOpKind kind = BoundOpKind::Exec;
        GraphExecFn exec = nullptr;
        GraphPureFn pure = nullptr;
        bool sequence = false;
        int dataIndex = -1;
        std::vector<BoundInput> inputs;
        std::vector<int> outputs;
        std::vector<std::vector<uint32_t>> execTargets;
        std::vector<std::string> inputNames;
        std::vector<std::string> outputNames;
        std::vector<std::string> execNames;
        // Declared type of each data output, so a node that fills its outputs from a
        // loosely typed source (an event payload, a reflected method result) can tag
        // the register the way the pin says it should instead of guessing.
        std::vector<GraphValueType> outputTypes;

        // Ordinals of one resolved pin, or -1 when the node has no such pin.
        int findInput(const std::string& name) const { return findNamed(inputNames, name); }
        int findOutput(const std::string& name) const { return findNamed(outputNames, name); }
        int findExec(const std::string& name) const { return findNamed(execNames, name); }

    private:
        static int findNamed(const std::vector<std::string>& names, const std::string& name);
    };

    // Immutable prepared form of an asset, shared by every entity running the
    // same .gdat. Registers and event entries are resolved once in prepare().
    class QUANTUM_API ExecutableGraph {
    public:
        bool prepare(const GraphAsset& sourceAsset, std::string& error);

        const GraphAsset& getAsset() const { return asset; }
        const std::vector<BoundNode>& getNodes() const { return nodes; }
        const BoundNode* findNode(uint32_t id) const;
        const std::vector<uint32_t>* findEventEntries(const std::string& eventId) const;
        int getVariableIndex(const std::string& name) const;
        const GraphVariable* findVariable(const std::string& name) const;
        int getRegisterCount() const { return registerCount; }
        const std::vector<GraphValue>& getRegisterDefaults() const { return registerDefaults; }

    private:
        GraphAsset asset;
        std::vector<BoundNode> nodes;
        std::map<uint32_t, uint32_t> nodeIndexById;
        std::map<std::string, std::vector<uint32_t>> eventEntries;
        std::map<std::string, int> variableIndexByName;
        std::vector<GraphValue> registerDefaults;
        int registerCount = 0;

        int addRegister();
    };

}
}

#endif //EXECUTABLE_GRAPH_H
