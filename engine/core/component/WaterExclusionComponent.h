// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef WATER_EXCLUSION_COMPONENT_H
#define WATER_EXCLUSION_COMPONENT_H

#include "math/AABB.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include <cstdint>
#include <vector>

#define MAX_WATER_EXCLUSIONS 8 // per water draw, nearest to the camera first
#define WATER_EXCLUSION_MAP_SIZE 128

namespace quantum{

    enum class WaterExclusionShape{
        BOX,
        SPHERE,
        HULL
    };

    // Volume that keeps water out, like a boat: the water surface inside it is not drawn,
    // a camera inside sees no underwater fade and the bodies inside do not float
    struct QUANTUM_API WaterExclusionComponent{
        WaterExclusionShape shape = WaterExclusionShape::HULL;
        Vector3 center = Vector3(0.0f, 0.0f, 0.0f); // of the box or sphere, local
        Vector3 size = Vector3(1.0f, 1.0f, 1.0f); // of the box or sphere, local

        // convex hull of the entity's meshes in its local space, built by RenderSystem
        std::vector<Vector4> hullPlanes; // xyz = outward normal, w = distance
        std::vector<Vector3> hullPoints;
        AABB hullBounds;
        std::vector<unsigned char> hullMap; // RGBA texels: bottom, top, outside the walls
        uint64_t hullSignature = 0; // of the geometry the hull was built from
        int hullLayer = -1; // in RenderSystem's hull map array

        AABB aabb;
        AABB worldAABB;
    };

}

#endif //WATER_EXCLUSION_COMPONENT_H
