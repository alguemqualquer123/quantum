// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef GRAPH_VALUE_H
#define GRAPH_VALUE_H

#include "GraphTypes.h"
#include "Export.h"
#include "ecs/Entity.h"
#include "math/Quaternion.h"
#include "math/Rect.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "math/Vector4.h"

#include <cstdint>
#include <string>
#include <variant>

namespace quantum {
namespace graph {

    // A tagged value flowing through pins and stored in variables/registers.
    // std::monostate is the "unset" state; the tag decides which alternative is
    // live, so the graph never reads a stale alternative.
    struct QUANTUM_API GraphValue {
        GraphValueType type = GraphValueType::None;
        std::variant<std::monostate, bool, int64_t, double, std::string,
                     Vector2, Vector3, Vector4, Quaternion, Rect, Entity> value;

        GraphValue() = default;

        static GraphValue makeNone();
        static GraphValue makeBool(bool v);
        static GraphValue makeInt(int64_t v);
        static GraphValue makeFloat(double v);
        static GraphValue makeString(const std::string& v);
        static GraphValue makeVector2(const Vector2& v);
        static GraphValue makeVector3(const Vector3& v);
        static GraphValue makeVector4(const Vector4& v);
        static GraphValue makeQuaternion(const Quaternion& v);
        static GraphValue makeRect(const Rect& v);
        static GraphValue makeColor3(const Vector3& v);
        static GraphValue makeColor4(const Vector4& v);
        static GraphValue makeEntity(Entity v);

        bool isNone() const { return type == GraphValueType::None; }

        bool asBool() const;
        int64_t asInt() const;
        double asFloat() const;
        std::string asString() const;
        Vector2 asVector2() const;
        Vector3 asVector3() const;
        Vector4 asVector4() const;
        Quaternion asQuaternion() const;
        Rect asRect() const;
        Entity asEntity() const;

        // Schema-facing coercion rules: what a pin of type `to` accepts from
        // this value without a cast node. Kept conservative (numeric widening,
        // Color3/4 <-> Vector3/4, Vec2 <-> Vec3) so authoring errors surface.
        bool canConvertTo(GraphValueType to) const;
        bool convertTo(GraphValueType to, GraphValue& out) const;

        std::string toDisplayString() const;
    };

    // Pure type-to-type coercion rule shared by the schema and by runtime pin
    // reads. Values use the same rule in canConvertTo().
    QUANTUM_API bool graphValueTypesCompatible(GraphValueType from, GraphValueType to);

    // Same value equality used by the editor to decide "was this pin edited?".
    QUANTUM_API bool graphValueEquals(const GraphValue& a, const GraphValue& b);

}
}

#endif //GRAPH_VALUE_H
