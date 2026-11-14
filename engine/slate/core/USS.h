// (c) SR VINIX
// SPDX-License-Identifier: MIT

#ifndef SlateUSS_h
#define SlateUSS_h

#include <string>
#include <vector>

#include "slate/core/StyleSet.h"

namespace quantum::slate {

struct USSDiagnostic {
    int line = 0;
    std::string message;
};

class USSParser {
public:
    // Parses a deliberately small, deterministic USS subset. Invalid rules are
    // reported and skipped so a hot reload cannot take down the running UI.
    static bool parse(const std::string& source, StyleSet& styles,
                      std::vector<USSDiagnostic>* diagnostics = nullptr);
};

}

#endif // SlateUSS_h
