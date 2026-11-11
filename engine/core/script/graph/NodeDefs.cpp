// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "NodeDefs.h"

#include "NodeOps.h"

#include <map>
#include <string>

namespace quantum {
namespace graph {

    namespace {

        std::map<std::string, GraphOpDef>& graphOps() {
            static std::map<std::string, GraphOpDef> ops;
            return ops;
        }

    }

    void registerGraphOp(const char* kind, BoundOpKind opKind, GraphExecFn exec, GraphPureFn pure) {
        GraphOpDef def;
        def.kind = kind;
        def.opKind = opKind;
        def.exec = exec;
        def.pure = pure;
        graphOps()[kind] = def;
    }

    const GraphOpDef* findGraphOp(const std::string& kind) {
        // The handlers are plain function pointers, so the table is built on the first
        // lookup instead of at load time: that keeps the group files free of static
        // init order concerns and costs nothing after the first graph is prepared.
        static bool initialized = false;
        if (!initialized) {
            initialized = true;
            registerFlowOps();
            registerVariableOps();
            registerMathOps();
            registerLogicOps();
            registerEntityOps();
            registerAnimationOps();
            registerPhysicsOps();
            registerAudioOps();
            registerSystemOps();
            registerReflectionOps();
        }

        std::map<std::string, GraphOpDef>::const_iterator it = graphOps().find(kind);
        if (it == graphOps().end() && kind.compare(0, 7, "method.") == 0) {
            it = graphOps().find("method.call");
        }
        return it == graphOps().end() ? nullptr : &it->second;
    }

}
}
