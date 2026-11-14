// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "NodeOps.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace quantum {
namespace graph {

    namespace {

        // A graph builds a string per frame in some HUDs, so a repeat/replace must
        // never be able to ask for more than a screenful.
        const size_t MAX_STRING_LENGTH = 65536;

        inline bool eitherIsText(const GraphValue& a, const GraphValue& b) {
            return a.type == GraphValueType::String || b.type == GraphValueType::String;
        }

        std::string compareText(GraphRuntime& rt, const BoundNode& n, size_t pin) {
            const GraphValue value = op::in(rt, n, pin);
            return value.type == GraphValueType::String ? value.asString() : value.toDisplayString();
        }

        std::string toUpper(std::string text) {
            std::transform(text.begin(), text.end(), text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            return text;
        }

        std::string toLower(std::string text) {
            std::transform(text.begin(), text.end(), text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        std::string trimText(std::string text) {
            const auto notSpace = [](unsigned char c) { return std::isspace(c) == 0; };
            text.erase(0, std::find_if(text.begin(), text.end(), notSpace) - text.begin());
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
                text.pop_back();
            }
            return text;
        }

        // ---- Logic ----
        void logicAnd(GraphRuntime& rt, const BoundNode& n) { op::outBool(rt, n, 0, op::flag(rt, n, 0) && op::flag(rt, n, 1)); }
        void logicOr(GraphRuntime& rt, const BoundNode& n) { op::outBool(rt, n, 0, op::flag(rt, n, 0) || op::flag(rt, n, 1)); }
        void logicXor(GraphRuntime& rt, const BoundNode& n) { op::outBool(rt, n, 0, op::flag(rt, n, 0) != op::flag(rt, n, 1)); }
        void logicNot(GraphRuntime& rt, const BoundNode& n) { op::outBool(rt, n, 0, !op::flag(rt, n, 0)); }

        enum class Order { Greater, Less, GreaterEqual, LessEqual };

        // A comparison where either side is text orders texts; otherwise it is numeric.
        void compareOrder(GraphRuntime& rt, const BoundNode& n, Order order) {
            const GraphValue a = op::in(rt, n, 0);
            const GraphValue b = op::in(rt, n, 1);
            bool result = false;
            if (eitherIsText(a, b)) {
                const std::string lhs = a.type == GraphValueType::String ? a.asString() : a.toDisplayString();
                const std::string rhs = b.type == GraphValueType::String ? b.asString() : b.toDisplayString();
                switch (order) {
                    case Order::Greater: result = lhs > rhs; break;
                    case Order::Less: result = lhs < rhs; break;
                    case Order::GreaterEqual: result = lhs >= rhs; break;
                    case Order::LessEqual: result = lhs <= rhs; break;
                }
            } else {
                const double lhs = a.asFloat();
                const double rhs = b.asFloat();
                switch (order) {
                    case Order::Greater: result = lhs > rhs; break;
                    case Order::Less: result = lhs < rhs; break;
                    case Order::GreaterEqual: result = lhs >= rhs; break;
                    case Order::LessEqual: result = lhs <= rhs; break;
                }
            }
            op::outBool(rt, n, 0, result);
        }

        void logicGreater(GraphRuntime& rt, const BoundNode& n) { compareOrder(rt, n, Order::Greater); }
        void logicLess(GraphRuntime& rt, const BoundNode& n) { compareOrder(rt, n, Order::Less); }
        void logicGreaterEqual(GraphRuntime& rt, const BoundNode& n) { compareOrder(rt, n, Order::GreaterEqual); }
        void logicLessEqual(GraphRuntime& rt, const BoundNode& n) { compareOrder(rt, n, Order::LessEqual); }

        // Equal on two texts compares texts; anything else compares numbers, which is
        // what a health-bar or a score check wants.
        void logicEqual(GraphRuntime& rt, const BoundNode& n) {
            const GraphValue a = op::in(rt, n, 0);
            const GraphValue b = op::in(rt, n, 1);
            if (eitherIsText(a, b)) {
                op::outBool(rt, n, 0, compareText(rt, n, 0) == compareText(rt, n, 1));
                return;
            }
            op::outBool(rt, n, 0, graphValueEquals(a, b));
        }
        void logicNotEqual(GraphRuntime& rt, const BoundNode& n) {
            const GraphValue a = op::in(rt, n, 0);
            const GraphValue b = op::in(rt, n, 1);
            if (eitherIsText(a, b)) {
                op::outBool(rt, n, 0, compareText(rt, n, 0) != compareText(rt, n, 1));
                return;
            }
            op::outBool(rt, n, 0, !graphValueEquals(a, b));
        }

        void logicBetween(GraphRuntime& rt, const BoundNode& n) {
            const double v = op::number(rt, n, 0);
            const double lo = op::number(rt, n, 1);
            const double hi = op::number(rt, n, 2);
            op::outBool(rt, n, 0, lo <= hi ? (v >= lo && v <= hi) : (v >= hi && v <= lo));
        }

        void logicApproximately(GraphRuntime& rt, const BoundNode& n) {
            const double a = op::number(rt, n, 0);
            const double b = op::number(rt, n, 1);
            double tolerance = std::fabs(op::number(rt, n, 2));
            if (tolerance <= 0.0) {
                tolerance = 0.0001;
            }
            op::outBool(rt, n, 0, std::fabs(a - b) <= tolerance);
        }

        void logicIsNil(GraphRuntime& rt, const BoundNode& n) {
            const GraphValue value = op::in(rt, n, 0);
            op::outBool(rt, n, 0, value.isNone() || (value.type == GraphValueType::Entity && value.asEntity() == NULL_ENTITY));
        }
        void logicIsNotNil(GraphRuntime& rt, const BoundNode& n) {
            const GraphValue value = op::in(rt, n, 0);
            op::outBool(rt, n, 0, !value.isNone() && !(value.type == GraphValueType::Entity && value.asEntity() == NULL_ENTITY));
        }

        void logicSelect(GraphRuntime& rt, const BoundNode& n) {
            op::out(rt, n, 0, op::flag(rt, n, 0) ? op::in(rt, n, 1) : op::in(rt, n, 2));
        }

        void logicTypeOf(GraphRuntime& rt, const BoundNode& n) {
            op::outString(rt, n, 0, graphValueTypeName(op::in(rt, n, 0).type));
        }

        void logicValidate(GraphRuntime& rt, const BoundNode& n) {
            const double v = op::number(rt, n, 0);
            const double lo = op::number(rt, n, 1);
            const double hi = op::number(rt, n, 2);
            const bool valid = std::isfinite(v) && v >= lo && v <= hi;
            op::outFloat(rt, n, 0, valid ? v : (std::isfinite(v) ? (v < lo ? lo : hi) : lo));
            op::outBool(rt, n, 1, !valid);
        }

        // ---- String ----
        void stringConcat(GraphRuntime& rt, const BoundNode& n) {
            std::string out = compareText(rt, n, 0) + compareText(rt, n, 1);
            if (out.size() > MAX_STRING_LENGTH) out.resize(MAX_STRING_LENGTH);
            op::outString(rt, n, 0, out);
        }
        void stringLength(GraphRuntime& rt, const BoundNode& n) { op::outInt(rt, n, 0, static_cast<int64_t>(op::text(rt, n, 0).size())); }
        void stringUpper(GraphRuntime& rt, const BoundNode& n) { op::outString(rt, n, 0, toUpper(op::text(rt, n, 0))); }
        void stringLower(GraphRuntime& rt, const BoundNode& n) { op::outString(rt, n, 0, toLower(op::text(rt, n, 0))); }
        void stringTrim(GraphRuntime& rt, const BoundNode& n) { op::outString(rt, n, 0, trimText(op::text(rt, n, 0))); }

        void stringContains(GraphRuntime& rt, const BoundNode& n) {
            const std::string value = op::text(rt, n, 0);
            const std::string needle = op::text(rt, n, 1);
            op::outBool(rt, n, 0, !needle.empty() && value.find(needle) != std::string::npos);
        }
        void stringStartsWith(GraphRuntime& rt, const BoundNode& n) {
            const std::string value = op::text(rt, n, 0);
            const std::string prefix = op::text(rt, n, 1);
            op::outBool(rt, n, 0, value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0);
        }
        void stringEndsWith(GraphRuntime& rt, const BoundNode& n) {
            const std::string value = op::text(rt, n, 0);
            const std::string suffix = op::text(rt, n, 1);
            op::outBool(rt, n, 0, value.size() >= suffix.size() &&
                                 value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0);
        }
        void stringEquals(GraphRuntime& rt, const BoundNode& n) { op::outBool(rt, n, 0, op::text(rt, n, 0) == op::text(rt, n, 1)); }
        void stringIsEmpty(GraphRuntime& rt, const BoundNode& n) { op::outBool(rt, n, 0, trimText(op::text(rt, n, 0)).empty()); }

        void stringReplace(GraphRuntime& rt, const BoundNode& n) {
            std::string value = op::text(rt, n, 0);
            const std::string from = op::text(rt, n, 1);
            const std::string to = op::text(rt, n, 2);
            if (from.empty()) {
                op::outString(rt, n, 0, value);
                return;
            }
            size_t pos = 0;
            size_t guard = 0;
            while ((pos = value.find(from, pos)) != std::string::npos && guard++ < 4096) {
                value.replace(pos, from.size(), to);
                pos += to.size();
                if (value.size() > MAX_STRING_LENGTH) {
                    value.resize(MAX_STRING_LENGTH);
                    break;
                }
            }
            op::outString(rt, n, 0, value);
        }

        void stringSubstring(GraphRuntime& rt, const BoundNode& n) {
            const std::string value = op::text(rt, n, 0);
            int64_t start = op::integer(rt, n, 1);
            const int64_t length = op::integer(rt, n, 2);
            if (start < 0) {
                start = 0;
            }
            if (static_cast<size_t>(start) >= value.size()) {
                op::outString(rt, n, 0, std::string());
                return;
            }
            const size_t count = length < 0 ? std::string::npos : static_cast<size_t>(length);
            op::outString(rt, n, 0, value.substr(static_cast<size_t>(start), count));
        }

        void stringRepeat(GraphRuntime& rt, const BoundNode& n) {
            const std::string value = op::text(rt, n, 0);
            const int64_t count = op::integer(rt, n, 1);
            std::string out;
            if (!value.empty() && count > 0) {
                const size_t times = static_cast<size_t>(std::min<int64_t>(count,
                    static_cast<int64_t>(MAX_STRING_LENGTH / value.size())));
                for (size_t i = 0; i < times; ++i) {
                    out += value;
                }
            }
            op::outString(rt, n, 0, out);
        }

        // {0} {1} {2} are replaced by the a/b/c pins. A value that is not a string is
        // rendered the way the debugger shows it, so a vector reads as "(x, y, z)".
        void stringFormat(GraphRuntime& rt, const BoundNode& n) {
            const std::string tmpl = op::text(rt, n, 0);
            const GraphValue args[3] = {op::in(rt, n, 1), op::in(rt, n, 2), op::in(rt, n, 3)};

            std::string out;
            out.reserve(tmpl.size() + 16);
            for (size_t i = 0; i < tmpl.size(); ++i) {
                if (tmpl[i] == '{' && i + 2 < tmpl.size() && tmpl[i + 1] >= '0' && tmpl[i + 1] <= '2' && tmpl[i + 2] == '}') {
                    const GraphValue& value = args[tmpl[i + 1] - '0'];
                    out += value.type == GraphValueType::String ? value.asString() : value.toDisplayString();
                    i += 2;
                    continue;
                }
                if (tmpl[i] == '{' && i + 1 < tmpl.size() && tmpl[i + 1] == '{') {
                    out += '{';
                    ++i;
                    continue;
                }
                out += tmpl[i];
                if (out.size() > MAX_STRING_LENGTH) {
                    break;
                }
            }
            op::outString(rt, n, 0, out);
        }

    }

    void registerLogicOps() {
        registerGraphOp("logic.and", BoundOpKind::Pure, nullptr, &logicAnd);
        registerGraphOp("logic.or", BoundOpKind::Pure, nullptr, &logicOr);
        registerGraphOp("logic.xor", BoundOpKind::Pure, nullptr, &logicXor);
        registerGraphOp("logic.not", BoundOpKind::Pure, nullptr, &logicNot);
        registerGraphOp("logic.greater", BoundOpKind::Pure, nullptr, &logicGreater);
        registerGraphOp("logic.less", BoundOpKind::Pure, nullptr, &logicLess);
        registerGraphOp("logic.greaterEqual", BoundOpKind::Pure, nullptr, &logicGreaterEqual);
        registerGraphOp("logic.lessEqual", BoundOpKind::Pure, nullptr, &logicLessEqual);
        registerGraphOp("logic.equal", BoundOpKind::Pure, nullptr, &logicEqual);
        registerGraphOp("logic.notEqual", BoundOpKind::Pure, nullptr, &logicNotEqual);
        registerGraphOp("logic.between", BoundOpKind::Pure, nullptr, &logicBetween);
        registerGraphOp("logic.approximately", BoundOpKind::Pure, nullptr, &logicApproximately);
        registerGraphOp("logic.isNil", BoundOpKind::Pure, nullptr, &logicIsNil);
        registerGraphOp("logic.isNotNil", BoundOpKind::Pure, nullptr, &logicIsNotNil);
        registerGraphOp("logic.select", BoundOpKind::Pure, nullptr, &logicSelect);
        registerGraphOp("logic.typeOf", BoundOpKind::Pure, nullptr, &logicTypeOf);
        registerGraphOp("logic.validate", BoundOpKind::Pure, nullptr, &logicValidate);

        registerGraphOp("string.concat", BoundOpKind::Pure, nullptr, &stringConcat);
        registerGraphOp("string.length", BoundOpKind::Pure, nullptr, &stringLength);
        registerGraphOp("string.upper", BoundOpKind::Pure, nullptr, &stringUpper);
        registerGraphOp("string.lower", BoundOpKind::Pure, nullptr, &stringLower);
        registerGraphOp("string.trim", BoundOpKind::Pure, nullptr, &stringTrim);
        registerGraphOp("string.contains", BoundOpKind::Pure, nullptr, &stringContains);
        registerGraphOp("string.startsWith", BoundOpKind::Pure, nullptr, &stringStartsWith);
        registerGraphOp("string.endsWith", BoundOpKind::Pure, nullptr, &stringEndsWith);
        registerGraphOp("string.equals", BoundOpKind::Pure, nullptr, &stringEquals);
        registerGraphOp("string.isEmpty", BoundOpKind::Pure, nullptr, &stringIsEmpty);
        registerGraphOp("string.replace", BoundOpKind::Pure, nullptr, &stringReplace);
        registerGraphOp("string.substring", BoundOpKind::Pure, nullptr, &stringSubstring);
        registerGraphOp("string.repeat", BoundOpKind::Pure, nullptr, &stringRepeat);
        registerGraphOp("string.format", BoundOpKind::Pure, nullptr, &stringFormat);
    }

}
}
