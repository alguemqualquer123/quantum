// Validation.h - field validation shared by the Inspector, dialogs and the
// "field validation" demo page.
#pragma once

#include <string>

namespace uilab {

// Rules understood by ValidateText():
//   none        - always valid
//   nonempty    - at least one non-space character
//   integer     - optional sign + digits
//   float       - optional sign, digits, optional single decimal separator
//   minlength   - at least 4 characters
//   maxlength   - at most 24 characters
//   pattern     - matches the wildcard pattern passed in 'pattern' (see MatchPattern)
//   identifier  - [A-Za-z_][A-Za-z0-9_]* (blueprint style asset names)
//   range       - value must be inside [minValue, maxValue]
bool ValidateText(const std::string& value, const std::string& rule, const std::string& pattern,
                  std::string& errorOut);
// Overload used when no pattern is involved (rule "pattern" then accepts everything).
bool ValidateText(const std::string& value, const std::string& rule, std::string& errorOut);

// Wildcard pattern matching: supports '*', '?' and character classes [a-z0-9_]
// as well as a plain pattern without wildcards (exact match). Case sensitive.
bool MatchPattern(const std::string& pattern, const std::string& text);

// Convenience: human readable description of a rule, used for tooltips/hints.
const char* ValidationRuleHint(const std::string& rule);

// List of rule identifiers for combos.
const char* const* ValidationRules(int& countOut);

bool ValidateFloatRange(float value, float minValue, float maxValue, std::string& errorOut);
bool ValidateIntRange(int value, int minValue, int maxValue, std::string& errorOut);

} // namespace uilab
