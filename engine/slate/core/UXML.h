// (c) SR VINIX
// SPDX-License-Identifier: MIT

#ifndef SlateUXML_h
#define SlateUXML_h

#include <memory>
#include <string>
#include <vector>

namespace quantum::slate {

class Widget;

struct UXMLDiagnostic {
    int line = 0;
    std::string message;
};

class UXMLParser {
public:
    // Builds a retained widget tree from a small, deterministic UXML subset.
    // Unknown tags and malformed attributes are reported without exceptions.
    static std::unique_ptr<Widget> parse(const std::string& source,
                                         std::vector<UXMLDiagnostic>* diagnostics = nullptr);
};

}

#endif // SlateUXML_h
