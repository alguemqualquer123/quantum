// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "GraphValue.h"

#include <cmath>
#include <cstdio>

namespace quantum {
namespace graph {

    GraphValue GraphValue::makeNone() {
        return GraphValue();
    }

    GraphValue GraphValue::makeBool(bool v) {
        GraphValue out;
        out.type = GraphValueType::Bool;
        out.value = v;
        return out;
    }

    GraphValue GraphValue::makeInt(int64_t v) {
        GraphValue out;
        out.type = GraphValueType::Int;
        out.value = v;
        return out;
    }

    GraphValue GraphValue::makeFloat(double v) {
        GraphValue out;
        out.type = GraphValueType::Float;
        out.value = v;
        return out;
    }

    GraphValue GraphValue::makeString(const std::string& v) {
        GraphValue out;
        out.type = GraphValueType::String;
        out.value = v;
        return out;
    }

    GraphValue GraphValue::makeVector2(const Vector2& v) {
        GraphValue out;
        out.type = GraphValueType::Vector2;
        out.value = v;
        return out;
    }

    GraphValue GraphValue::makeVector3(const Vector3& v) {
        GraphValue out;
        out.type = GraphValueType::Vector3;
        out.value = v;
        return out;
    }

    GraphValue GraphValue::makeVector4(const Vector4& v) {
        GraphValue out;
        out.type = GraphValueType::Vector4;
        out.value = v;
        return out;
    }

    GraphValue GraphValue::makeQuaternion(const Quaternion& v) {
        GraphValue out;
        out.type = GraphValueType::Quaternion;
        out.value = v;
        return out;
    }

    GraphValue GraphValue::makeRect(const Rect& v) {
        GraphValue out;
        out.type = GraphValueType::Rect;
        out.value = v;
        return out;
    }

    GraphValue GraphValue::makeColor3(const Vector3& v) {
        GraphValue out;
        out.type = GraphValueType::Color3;
        out.value = v;
        return out;
    }

    GraphValue GraphValue::makeColor4(const Vector4& v) {
        GraphValue out;
        out.type = GraphValueType::Color4;
        out.value = v;
        return out;
    }

    GraphValue GraphValue::makeEntity(Entity v) {
        GraphValue out;
        out.type = GraphValueType::Entity;
        out.value = v;
        return out;
    }

    bool GraphValue::asBool() const {
        switch (type) {
            case GraphValueType::Bool: {
                const bool* v = std::get_if<bool>(&value);
                return v && *v;
            }
            case GraphValueType::Int: {
                const int64_t* v = std::get_if<int64_t>(&value);
                return v && *v != 0;
            }
            case GraphValueType::Float: {
                const double* v = std::get_if<double>(&value);
                return v && *v != 0.0;
            }
            default:
                return false;
        }
    }

    int64_t GraphValue::asInt() const {
        switch (type) {
            case GraphValueType::Bool:
                return asBool() ? 1 : 0;
            case GraphValueType::Int: {
                const int64_t* v = std::get_if<int64_t>(&value);
                return v ? *v : 0;
            }
            case GraphValueType::Float: {
                const double* v = std::get_if<double>(&value);
                return v ? static_cast<int64_t>(*v) : 0;
            }
            case GraphValueType::Entity: {
                const Entity* v = std::get_if<Entity>(&value);
                return v ? static_cast<int64_t>(*v) : 0;
            }
            default:
                return 0;
        }
    }

    double GraphValue::asFloat() const {
        switch (type) {
            case GraphValueType::Bool:
                return asBool() ? 1.0 : 0.0;
            case GraphValueType::Int: {
                const int64_t* v = std::get_if<int64_t>(&value);
                return v ? static_cast<double>(*v) : 0.0;
            }
            case GraphValueType::Float: {
                const double* v = std::get_if<double>(&value);
                return v ? *v : 0.0;
            }
            default:
                return 0.0;
        }
    }

    std::string GraphValue::asString() const {
        switch (type) {
            case GraphValueType::String: {
                const std::string* v = std::get_if<std::string>(&value);
                return v ? *v : std::string();
            }
            case GraphValueType::Bool:
                return asBool() ? "true" : "false";
            case GraphValueType::Int: {
                char buffer[32];
                std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(asInt()));
                return buffer;
            }
            case GraphValueType::Float: {
                char buffer[64];
                std::snprintf(buffer, sizeof(buffer), "%g", asFloat());
                return buffer;
            }
            default:
                return std::string();
        }
    }

    Vector2 GraphValue::asVector2() const {
        switch (type) {
            case GraphValueType::Vector2: {
                const Vector2* v = std::get_if<Vector2>(&value);
                return v ? *v : Vector2();
            }
            case GraphValueType::Vector3: {
                const Vector3* v = std::get_if<Vector3>(&value);
                return v ? Vector2(v->x, v->y) : Vector2();
            }
            default:
                return Vector2();
        }
    }

    Vector3 GraphValue::asVector3() const {
        switch (type) {
            case GraphValueType::Vector3:
            case GraphValueType::Color3: {
                const Vector3* v = std::get_if<Vector3>(&value);
                return v ? *v : Vector3();
            }
            case GraphValueType::Vector2: {
                const Vector2* v = std::get_if<Vector2>(&value);
                return v ? Vector3(v->x, v->y, 0.0f) : Vector3();
            }
            default:
                return Vector3();
        }
    }

    Vector4 GraphValue::asVector4() const {
        switch (type) {
            case GraphValueType::Vector4:
            case GraphValueType::Color4: {
                const Vector4* v = std::get_if<Vector4>(&value);
                return v ? *v : Vector4();
            }
            default:
                return Vector4();
        }
    }

    Quaternion GraphValue::asQuaternion() const {
        const Quaternion* v = std::get_if<Quaternion>(&value);
        return v ? *v : Quaternion();
    }

    Rect GraphValue::asRect() const {
        const Rect* v = std::get_if<Rect>(&value);
        return v ? *v : Rect(0.0f, 0.0f, 0.0f, 0.0f);
    }

    Entity GraphValue::asEntity() const {
        const Entity* v = std::get_if<Entity>(&value);
        return v ? *v : NULL_ENTITY;
    }

    bool graphValueTypesCompatible(GraphValueType from, GraphValueType to) {
        if (to == GraphValueType::Any || from == GraphValueType::Any) {
            return true;
        }
        if (to == from) {
            return true;
        }
        if ((from == GraphValueType::Color3 && to == GraphValueType::Vector3) ||
            (from == GraphValueType::Vector3 && to == GraphValueType::Color3) ||
            (from == GraphValueType::Color4 && to == GraphValueType::Vector4) ||
            (from == GraphValueType::Vector4 && to == GraphValueType::Color4)) {
            return true;
        }
        const bool numericSource = from == GraphValueType::Bool ||
                                   from == GraphValueType::Int ||
                                   from == GraphValueType::Float;
        const bool numericTarget = to == GraphValueType::Bool ||
                                   to == GraphValueType::Int ||
                                   to == GraphValueType::Float;
        if (numericSource && numericTarget) {
            return true;
        }
        if ((from == GraphValueType::Vector2 && to == GraphValueType::Vector3) ||
            (from == GraphValueType::Vector3 && to == GraphValueType::Vector2)) {
            return true;
        }
        // A numeric literal may feed an Entity pin only when it is an int.
        if (to == GraphValueType::Entity && from == GraphValueType::Int) {
            return true;
        }
        return false;
    }

    bool GraphValue::canConvertTo(GraphValueType to) const {
        return graphValueTypesCompatible(type, to);
    }

    bool GraphValue::convertTo(GraphValueType to, GraphValue& out) const {
        if (!canConvertTo(to)) {
            return false;
        }
        if (type == to) {
            out = *this;
            return true;
        }
        switch (to) {
            case GraphValueType::Bool:
                out = makeBool(asBool());
                return true;
            case GraphValueType::Int:
                out = makeInt(asInt());
                return true;
            case GraphValueType::Float:
                out = makeFloat(asFloat());
                return true;
            case GraphValueType::Vector2:
                out = makeVector2(asVector2());
                return true;
            case GraphValueType::Vector3:
                out = makeVector3(asVector3());
                return true;
            case GraphValueType::Color3:
                out = makeColor3(asVector3());
                return true;
            case GraphValueType::Vector4:
                out = makeVector4(asVector4());
                return true;
            case GraphValueType::Color4:
                out = makeColor4(asVector4());
                return true;
            case GraphValueType::Entity:
                out = makeEntity(static_cast<Entity>(asInt()));
                return true;
            default:
                return false;
        }
    }

    std::string GraphValue::toDisplayString() const {
        char buffer[160];
        switch (type) {
            case GraphValueType::None:
                return "<none>";
            case GraphValueType::Bool:
                return asBool() ? "true" : "false";
            case GraphValueType::Int: {
                std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(asInt()));
                return buffer;
            }
            case GraphValueType::Float: {
                std::snprintf(buffer, sizeof(buffer), "%g", asFloat());
                return buffer;
            }
            case GraphValueType::String:
                return asString();
            case GraphValueType::Vector2: {
                Vector2 v = asVector2();
                std::snprintf(buffer, sizeof(buffer), "(%g, %g)", v.x, v.y);
                return buffer;
            }
            case GraphValueType::Vector3:
            case GraphValueType::Color3: {
                Vector3 v = asVector3();
                std::snprintf(buffer, sizeof(buffer), "(%g, %g, %g)", v.x, v.y, v.z);
                return buffer;
            }
            case GraphValueType::Vector4:
            case GraphValueType::Color4: {
                Vector4 v = asVector4();
                std::snprintf(buffer, sizeof(buffer), "(%g, %g, %g, %g)", v.x, v.y, v.z, v.w);
                return buffer;
            }
            case GraphValueType::Quaternion: {
                Quaternion q = asQuaternion();
                std::snprintf(buffer, sizeof(buffer), "(%g, %g, %g, %g)", q.w, q.x, q.y, q.z);
                return buffer;
            }
            case GraphValueType::Rect: {
                Rect r = asRect();
                std::snprintf(buffer, sizeof(buffer), "(%g, %g, %g, %g)", r.getX(), r.getY(), r.getWidth(), r.getHeight());
                return buffer;
            }
            case GraphValueType::Entity: {
                std::snprintf(buffer, sizeof(buffer), "entity:%u", asEntity());
                return buffer;
            }
            case GraphValueType::Any:
                return "<any>";
        }
        return "<none>";
    }

    bool graphValueEquals(const GraphValue& a, const GraphValue& b) {
        if (a.type != b.type) {
            return false;
        }
        switch (a.type) {
            case GraphValueType::None: return true;
            case GraphValueType::Bool: return a.asBool() == b.asBool();
            case GraphValueType::Int: return a.asInt() == b.asInt();
            case GraphValueType::Float: return a.asFloat() == b.asFloat();
            case GraphValueType::String: return a.asString() == b.asString();
            case GraphValueType::Vector2: {
                Vector2 av = a.asVector2(), bv = b.asVector2();
                return av.x == bv.x && av.y == bv.y;
            }
            case GraphValueType::Vector3:
            case GraphValueType::Color3: {
                Vector3 av = a.asVector3(), bv = b.asVector3();
                return av.x == bv.x && av.y == bv.y && av.z == bv.z;
            }
            case GraphValueType::Vector4:
            case GraphValueType::Color4: {
                Vector4 av = a.asVector4(), bv = b.asVector4();
                return av.x == bv.x && av.y == bv.y && av.z == bv.z && av.w == bv.w;
            }
            case GraphValueType::Quaternion: {
                Quaternion aq = a.asQuaternion(), bq = b.asQuaternion();
                return aq.w == bq.w && aq.x == bq.x && aq.y == bq.y && aq.z == bq.z;
            }
            case GraphValueType::Rect: {
                Rect ar = a.asRect(), br = b.asRect();
                return ar.getX() == br.getX() && ar.getY() == br.getY() &&
                       ar.getWidth() == br.getWidth() && ar.getHeight() == br.getHeight();
            }
            case GraphValueType::Entity: return a.asEntity() == b.asEntity();
            case GraphValueType::Any: return true;
        }
        return false;
    }

}
}
