// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef WATER_EXCLUSION_H
#define WATER_EXCLUSION_H

#include "object/Object.h"
#include "component/WaterExclusionComponent.h"

namespace quantum{

    class QUANTUM_API WaterExclusion: public Object{

    public:
        WaterExclusion(Scene* scene);
        WaterExclusion(Scene* scene, Entity entity);
        virtual ~WaterExclusion();

        // a hull is the convex hull of the entity's meshes
        void setShape(WaterExclusionShape shape);
        WaterExclusionShape getShape() const;

        void setCenter(Vector3 center);
        void setCenter(const float x, const float y, const float z);
        Vector3 getCenter() const;

        void setSize(Vector3 size);
        void setSize(const float x, const float y, const float z);
        Vector3 getSize() const;

        // whether the world point is inside the volume
        bool contains(Vector3 point) const;
    };
}

#endif //WATER_EXCLUSION_H
