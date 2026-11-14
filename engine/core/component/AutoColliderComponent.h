// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef AUTOCOLLIDER_COMPONENT_H
#define AUTOCOLLIDER_COMPONENT_H

#include "Engine.h"
#include "ecs/Entity.h"
#include "math/Vector3.h"

namespace quantum{

    enum class AutoColliderType{
        NONE,
        SPHERE,
        BOX,
        PLANE,
        CAPSULE,
        CYLINDER,
        CONVEX_HULL,
        MESH_AUTO
    };

    struct QUANTUM_API AutoColliderComponent{
        AutoColliderType type = AutoColliderType::BOX;

        // shape dimensions (world units, before entity scale)
        float radius = 0.5f;
        float width = 1.0f;
        float height = 1.0f;
        float depth = 1.0f;
        float halfHeight = 0.5f;

        // shape offset from the entity transform
        Vector3 center = Vector3::ZERO;

        // overlap only: fires sensor contact events, never collides
        bool sensor = false;

        // kg/m3: mass = scaled volume * density
        float density = 1000.0f;

        // collision layers
        uint16_t categoryBits = 1;
        uint16_t maskBits = 0xFFFF;

        // for CONVEX_HULL / MESH_AUTO: read geometry from this entity (defaults to self)
        Entity sourceEntity = NULL_ENTITY;

        // set to true to (re)build the physics shape on the next system update
        bool needUpdate = true;
    };

}

#endif //AUTOCOLLIDER_COMPONENT_H
