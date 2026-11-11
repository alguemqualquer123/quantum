// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef WATERSYSTEM_H
#define WATERSYSTEM_H

#include "SubSystem.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "component/Transform.h"
#include "component/WaterComponent.h"

#include <cstdint>
#include <vector>

namespace quantum{

    // Steps the volume field of the waters that simulate: the water keeps a depth per cell,
    // runs downhill through the cell walls, is filled by rain, is taken back by evaporation
    // and disturbs its own surface with ripples. Registered before RenderSystem, so a frame
    // packs its texels and bounds from the field the same frame steps it.
    class QUANTUM_API WaterSystem: public SubSystem{

    public:

        WaterSystem(Scene* scene);
        virtual ~WaterSystem();

        void load() override;
        void draw() override;
        void destroy() override;
        void update(double dt) override;
        void fixedUpdate(double dt) override;

        // Surface of the simulated field at the world point (x, z): the height over the
        // water entity's world Y, its normal and the depth of the column. False where the
        // field is dry or the point is outside it, which is what keeps a body from floating
        // over a puddle the water has not reached yet.
        static bool getSimSurface(const WaterComponent& water, float x, float z,
                float& height, Vector3& normal, float& depth);

        // Flow speed of the cell at the world point, metres per second, for the water that
        // carries a floating body along. False where dry or outside.
        static bool getSimFlow(const WaterComponent& water, float x, float z, Vector3& velocity);

        // Pushes the surface down in a disc, like a body hitting the water, a footstep or a
        // bucket tipping over. The crater is what rings out over the field.
        void addDisturbance(const Vector3& worldPosition, float radius, float strength);

        // Pour cubicMetres into the field, and take up to cubicMetres back out of it, at a world
        // point rather than over the whole area. These are the two ends of a fall: the heightfield
        // cannot drop water off a cliff by itself, so a script moves the volume from the upper
        // field to the lower one and drainWater says how much the upper one actually had.
        // Draining scales to what each cell holds, so it never digs a hole to the bed.
        static void addWater(WaterComponent& water, const Vector3& worldPosition,
                float radius, float cubicMetres);
        static float drainWater(WaterComponent& water, const Vector3& worldPosition,
                float radius, float cubicMetres);

        // Litres standing in the field of one water, which the step keeps constant while
        // neither rain nor evaporation runs
        double getVolume(Entity entity) const;

        // The field is vertical: it displaces the drawn surface along Y only, so a tilted
        // water entity shows the field as if it were level, as the waves already do
        static bool isSimulated(const WaterComponent& water);

        // cells per axis the authored resolution comes out as, after the cap and the floor.
        // RenderSystem sizes the texel texture with it, before the first step allocates
        static unsigned int resolutionOf(const WaterComponent& water);

    private:

        // grids, the world rect and the baked ground of one water
        void resize(WaterComponent& water, Transform& transform);
        void bakeFloor(WaterComponent& water, Transform& transform);
        void stepFlow(WaterComponent& water, float dt);
        void stepRipple(WaterComponent& water, float dt);
        void stepSources(WaterComponent& water, float dt);
        void rainDrops(WaterComponent& water, float dt);
        void writeTexels(WaterComponent& water);

        unsigned int cellsOf(const WaterComponent& water) const;
        float nextRandom();

        // reused by every water of the step, so the passes allocate nothing
        std::vector<float> nextDepth;
        std::vector<float> nextRipple;
        uint64_t rngState = 0x9e3779b97f4a7c15ull;
    };

}

#endif //WATERSYSTEM_H
