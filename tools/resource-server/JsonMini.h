// (c) SR VINIX
// SPDX-License-Identifier: MIT

// JsonMini.h - the minimal JSON support quantum-resource-server needs: parsing
// one flat request object per line and escaping response strings. Standard
// library only. Nested objects/arrays inside a request are skipped, not stored.

#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace quantum::rs {

struct JsonField {
    std::string text;      // string value, or the raw token for numbers/literals
    double number = 0.0;   // parsed value when isNumber
    bool isNumber = false;

    uint64_t asUInt() const;
    bool truthy() const;   // true for literals true / nonzero numbers / "true"
};

// Parses a top-level JSON object into its scalar members. Returns false with
// an error message when the text is not a well-formed object.
bool parseFlatJsonObject(const std::string& text, std::map<std::string, JsonField>& out,
                         std::string& errorOut);

// Escapes a UTF-8 string for embedding in a JSON string literal (quotes and
// backslashes included).
std::string jsonEscape(const std::string& in);

// Convenience: "key":value pairs appended to a response object body.
void jsonPutString(std::string& out, const std::string& key, const std::string& value);
void jsonPutNumber(std::string& out, const std::string& key, double value);

}
