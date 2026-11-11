// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "engine_api_suggestions.h"

#include <string>
#include <utility>
#include <vector>

namespace quantum::editor {

    // Read-only view over the API metadata quantum-reflector generates into
    // engine_api_suggestions.h. It is deliberately free of ImGui so the same filtering the
    // Engine API tab runs can be checked headlessly by `quantum-editor-cmd api ...`.
    class EngineApiIndex {
    public:
        // Every symbol the editor completes from, in generator order.
        static const std::vector<EngineAPISymbol>& symbols();

        // Kind names in display order, then any kind only the generator emits.
        static const std::vector<std::string>& kinds();

        // Owner (class/enum) with how many symbols it holds, biggest first.
        static std::vector<std::pair<std::string, int>> owners();

        // Indices into symbols() that match: empty text/kind/owner means "no constraint".
        // Case-insensitive over name, owner and signature.
        static std::vector<int> filter(const std::string& text, const std::string& kind,
                                       const std::string& owner);

        // How many symbols each kind holds, aligned with kinds().
        static std::vector<int> kindCounts();

        // The members declared under one owner, in generator order.
        static std::vector<const EngineAPISymbol*> membersOf(const std::string& owner);

        // Rows whose name equals the wanted one (a name can be a method on several owners).
        static std::vector<const EngineAPISymbol*> named(const std::string& name);

        // Generator problems that would silently empty a tab: unknown kinds, members with no
        // owner, an empty table, a kind that stopped being emitted.
        static std::vector<std::string> verify();

        static const char* kindLabel(const std::string& kind);
    };

}
