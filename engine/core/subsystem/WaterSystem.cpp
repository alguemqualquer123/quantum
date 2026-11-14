// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "WaterSystem.h"

#include "Scene.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "subsystem/MeshSystem.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace quantum;

namespace {
    constexpr float WATER_GRAVITY = 9.81f;
    // the explicit ripple step only holds while a wave travels less than this per cell
    constexpr float WATER_RIPPLE_COURANT = 0.7f;
    // drops per second over a square metre of a full shower
    constexpr float WATER_DROPS_PER_SQUARE_METRE = 6.0f;
    // rain is a broad source, so the drops that show as rings are a bounded few
    constexpr unsigned int WATER_MAX_DROPS_PER_STEP = 48;
    // a film thinner than this carries neither a wave nor a floating body's current
    constexpr float WATER_THIN_FILM = 0.01f;
    // the field is a closed basin unless the water is allowed to leave it
    constexpr float WATER_MIN_CELL = 1.0e-4f;
    // per-second flux decay of a still bed: what turns the head-driven acceleration into a
    // terminal speed instead of a runaway one, roughly a rocky bed's Manning drag
    constexpr float WATER_BED_DRAG = 2.0f;

    unsigned int cellIndex(unsigned int x, unsigned int z, unsigned int n){
        return z * n + x;
    }

    // Bilinear over a field at cell coordinates: 0 is the centre of the first cell and n-1
    // the centre of the last one, so a lookup never leaves the area.
    float sampleField(const float* field, unsigned int stride, unsigned int component, unsigned int n, float fx, float fz){
        fx = std::clamp(fx, 0.0f, (float)(n - 1));
        fz = std::clamp(fz, 0.0f, (float)(n - 1));
        const unsigned int x0 = (unsigned int)std::floor(fx);
        const unsigned int z0 = (unsigned int)std::floor(fz);
        const unsigned int x1 = std::min(x0 + 1, n - 1);
        const unsigned int z1 = std::min(z0 + 1, n - 1);
        const float ax = fx - (float)x0;
        const float az = fz - (float)z0;
        const float row0 = field[cellIndex(x0, z0, n) * stride + component] * (1.0f - ax) +
                field[cellIndex(x1, z0, n) * stride + component] * ax;
        const float row1 = field[cellIndex(x0, z1, n) * stride + component] * (1.0f - ax) +
                field[cellIndex(x1, z1, n) * stride + component] * ax;
        return row0 * (1.0f - az) + row1 * az;
    }

    float sampleField(const std::vector<float>& field, unsigned int n, float fx, float fz){
        return sampleField(field.data(), 1u, 0u, n, fx, fz);
    }

    // the surface the field puts over the entity plane, at cell coordinates
    float surfaceHeight(const WaterComponent& water, unsigned int n, float fx, float fz){
        return sampleField(water.simFloor, n, fx, fz) + sampleField(water.simWater, n, fx, fz) +
                sampleField(water.simRipple, n, fx, fz);
    }

    float cellCoordinate(float from, float origin, float cell){
        return (from - origin) / std::max(cell, WATER_MIN_CELL) - 0.5f;
    }
}

bool WaterSystem::isSimulated(const WaterComponent& water){
    return water.simulate && water.simCells >= 2 && !water.simWater.empty() && !water.simFloor.empty();
}

unsigned int WaterSystem::resolutionOf(const WaterComponent& water){
    return std::clamp(water.simResolution, (unsigned int)MIN_WATER_SIM, (unsigned int)MAX_WATER_SIM);
}

WaterSystem::WaterSystem(Scene* scene): SubSystem(scene){
    signature.set(scene->getComponentId<WaterComponent>());
}

WaterSystem::~WaterSystem(){
}

void WaterSystem::load(){
}

void WaterSystem::draw(){
}

void WaterSystem::destroy(){
    nextDepth.clear();
    nextRipple.clear();
}

void WaterSystem::update(double dt){
    (void)dt;

    // packed once per frame, however many fixed steps ran before it. RenderSystem uploads
    // them on the draw, and clears the flag
    auto waters = scene->getComponentArray<WaterComponent>();
    if (!waters)
        return;

    for (int i = 0; i < waters->size(); i++){
        WaterComponent& water = waters->getComponentFromIndex(i);
        if (!water.simulate)
            continue;

        // the grids come up here rather than on the first step, so a scene that never
        // advances still shows the water it was authored with
        Transform* transform = scene->findComponent<Transform>(waters->getEntity(i));
        if (transform)
            resize(water, *transform);

        if (water.simDirty && isSimulated(water))
            writeTexels(water);
    }
}

void WaterSystem::fixedUpdate(double dt){
    if (paused)
        return;

    auto waters = scene->getComponentArray<WaterComponent>();
    if (!waters || waters->size() == 0)
        return;

    const float step = (float)dt;
    for (int i = 0; i < waters->size(); i++){
        WaterComponent& water = waters->getComponentFromIndex(i);
        if (!water.simulate)
            continue;

        Transform* transform = scene->findComponent<Transform>(waters->getEntity(i));
        if (!transform)
            continue;

        resize(water, *transform);
        if (!isSimulated(water))
            continue;

        stepFlow(water, step);
        if (water.simRipples)
            stepRipple(water, step);
        stepSources(water, step);
    }
}

float WaterSystem::nextRandom(){
    // xorshift64*, so a step does not depend on the platform's rand()
    rngState ^= rngState >> 12;
    rngState ^= rngState << 25;
    rngState ^= rngState >> 27;
    const uint64_t mixed = rngState * 0x2545F4914F6CDD1Dull;
    return (float)((mixed >> 40) * (1.0 / 16777216.0)); // 24 bits over 2^24, in [0, 1)
}

unsigned int WaterSystem::cellsOf(const WaterComponent& water) const{
    return resolutionOf(water);
}

// The field covers the authored area in world space. A moved or resized water keeps the
// grids it already has, and only re-bakes the ground once it really left the cells.
void WaterSystem::resize(WaterComponent& water, Transform& transform){
    const unsigned int n = cellsOf(water);
    const size_t cells = (size_t)n * n;

    const float halfWidth = std::abs(water.size.x) * 0.5f;
    const float halfDepth = std::abs(water.size.y) * 0.5f;
    const Vector2 corners[4] = {Vector2(-halfWidth, -halfDepth), Vector2(halfWidth, -halfDepth),
            Vector2(-halfWidth, halfDepth), Vector2(halfWidth, halfDepth)};

    Vector2 min(std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
    Vector2 max(-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max());
    for (const Vector2& corner : corners){
        const Vector3 world = transform.modelMatrix * Vector3(corner.x, 0.0f, corner.y);
        min.x = std::min(min.x, world.x);
        min.y = std::min(min.y, world.z);
        max.x = std::max(max.x, world.x);
        max.y = std::max(max.y, world.z);
    }

    const Vector2 span = max - min;
    const Vector2 cell(std::max(span.x, WATER_MIN_CELL) / (float)n, std::max(span.y, WATER_MIN_CELL) / (float)n);
    const bool grew = water.simCells != n || water.simFloor.size() != cells;
    // half a cell of slack, so a walking entity does not resample the ground every step
    const bool movedSinceBake = water.simFloorLoaded &&
            (std::abs(min.x - water.simBaked.x) > cell.x * 0.5f ||
             std::abs(min.y - water.simBaked.y) > cell.y * 0.5f ||
             std::abs(transform.worldPosition.y - water.simLevel) > cell.y * 0.5f);

    water.simOrigin = min;
    water.simCell = cell;

    if (grew){
        water.simCells = n;
        water.simFloor.assign(cells, 0.0f);
        water.simWater.assign(cells, std::max(water.simDepth, 0.0f));
        water.simRipple.assign(cells, 0.0f);
        water.simRippleVel.assign(cells, 0.0f);
        water.simFlux.assign(cells * WATER_DIRS, 0.0f);
        water.simVelocity.assign(cells * 2, 0.0f);
        water.simTexels.assign(cells * 4, 0.0f);
        nextDepth.assign(cells, 0.0f);
        nextRipple.assign(cells, 0.0f);
        water.simFloorLoaded = false;
        water.simDirty = true;
    }

    // the scratch of a scene that was destroyed and reloaded is gone, but its fields are not
    if (nextDepth.size() != cells){
        nextDepth.assign(cells, 0.0f);
        nextRipple.assign(cells, 0.0f);
    }

    if (!water.simFloorLoaded || movedSinceBake)
        bakeFloor(water, transform);
}

// The ground under each cell, from the terrain that covers it. Where none does, the floor is
// the entity's own plane, so a water with no terrain under it is a basin with a flat bottom.
void WaterSystem::bakeFloor(WaterComponent& water, Transform& transform){
    water.simLevel = transform.worldPosition.y;
    water.simBaked = water.simOrigin;
    water.simFloorLoaded = true;

    const unsigned int n = water.simCells;
    if (!isSimulated(water))
        return;

    std::fill(water.simFloor.begin(), water.simFloor.end(), 0.0f);

    auto terrains = scene->getComponentArray<TerrainComponent>();
    auto mesh = scene->getSystem<MeshSystem>();
    if (terrains && terrains->size() > 0 && mesh){
        for (int t = 0; t < terrains->size(); t++){
            TerrainComponent& terrain = terrains->getComponentFromIndex(t);
            Transform* terrainTransform = scene->findComponent<Transform>(terrains->getEntity(t));
            if (!terrainTransform || terrain.terrainSize <= 0.0f)
                continue;

            // terrain-local lookup is what the foliage scatter and the heightfield shape use,
            // so the floor agrees with the surface the bodies collide with
            const Matrix4 inverse = terrainTransform->modelMatrix.inverse();
            const float half = terrain.terrainSize * 0.5f;
            for (unsigned int z = 0; z < n; z++){
                for (unsigned int x = 0; x < n; x++){
                    const Vector3 world(water.simOrigin.x + ((float)x + 0.5f) * water.simCell.x, water.simLevel,
                            water.simOrigin.y + ((float)z + 0.5f) * water.simCell.y);
                    const Vector3 local = inverse * world;
                    if (std::abs(local.x) > half || std::abs(local.z) > half)
                        continue;

                    float height;
                    Vector3 normal;
                    mesh->sampleTerrainSurface(terrain, local.x, local.z, height, normal);
                    // back to world, so the terrain's own scale is kept, then over this water
                    const Vector3 surface = terrainTransform->modelMatrix * Vector3(local.x, height, local.z);
                    water.simFloor[cellIndex(x, z, n)] = surface.y - water.simLevel;
                }
            }
        }
    }

    float minFloor = std::numeric_limits<float>::max();
    float maxFloor = -std::numeric_limits<float>::max();
    for (unsigned int i = 0; i < n * n; i++){
        minFloor = std::min(minFloor, water.simFloor[i]);
        maxFloor = std::max(maxFloor, water.simFloor[i]);
    }
    water.simFloorMin = minFloor;
    water.simFloorMax = maxFloor;
    water.simDirty = true;
}

// The shallow water step of the pipe model: every cell wall passes water at the rate its
// head difference asks for, and no cell gives away more than it holds, which is what keeps
// the volume constant without a global solve.
void WaterSystem::stepFlow(WaterComponent& water, float dt){
    const unsigned int n = water.simCells;
    const float lx = std::max(water.simCell.x, WATER_MIN_CELL);
    const float lz = std::max(water.simCell.y, WATER_MIN_CELL);
    const float cellArea = lx * lz;
    const float mobility = std::clamp(water.simFlow, 0.0f, 1.0f);
    const float k = dt * WATER_GRAVITY * mobility;
    // Bed drag. Without it the flux integrates the head difference every step and nothing takes
    // it back, so water on a slope accelerates until the cell is empty: a shelf drains itself bone
    // dry in a few seconds and a stream is never deep enough to see. The reciprocal form is
    // unconditionally stable, so a big step cannot push a flux negative through the damping.
    const float damping = 1.0f / (1.0f + dt * WATER_BED_DRAG * (1.0f - mobility * 0.8f));

    float* flux = water.simFlux.data();
    const float* floor = water.simFloor.data();
    const float* depth = water.simWater.data();

    for (unsigned int z = 0; z < n; z++){
        for (unsigned int x = 0; x < n; x++){
            const unsigned int cell = cellIndex(x, z, n);
            const float head = floor[cell] + depth[cell];

            for (int dir = 0; dir < WATER_DIRS; dir++){
                const int otherX = (int)x + ((dir == 0) ? 1 : (dir == 1) ? -1 : 0);
                const int otherZ = (int)z + ((dir == 2) ? 1 : (dir == 3) ? -1 : 0);
                float& flow = flux[cell * WATER_DIRS + dir];

                if (otherX < 0 || (unsigned int)otherX >= n || otherZ < 0 || (unsigned int)otherZ >= n){
                    // the border is a wall, unless the water may leave the area
                    if (!water.simEdgeDrain){
                        flow = 0.0f;
                        continue;
                    }
                    // it falls out of the field, so nothing holds it back once it gets there
                    flow = std::max(0.0f, flow * damping + k * depth[cell] * 0.5f * depth[cell]);
                    continue;
                }

                const unsigned int other = cellIndex((unsigned int)otherX, (unsigned int)otherZ, n);
                // the wall is one cell wide at the average depth and the pipe is one cell
                // long, so the length cancels and only the head difference is left
                const float average = (depth[cell] + depth[other]) * 0.5f;
                flow = std::max(0.0f, flow * damping + k * average * (head - (floor[other] + depth[other])));
            }

            const float out = flux[cell * WATER_DIRS] + flux[cell * WATER_DIRS + 1] +
                    flux[cell * WATER_DIRS + 2] + flux[cell * WATER_DIRS + 3];
            const float available = depth[cell] * cellArea / dt;
            if (out > available && out > 0.0f){
                const float scale = available / out;
                for (int dir = 0; dir < WATER_DIRS; dir++){
                    flux[cell * WATER_DIRS + dir] *= scale;
                }
            }
        }
    }

    float* velocity = water.simVelocity.data();
    float* next = nextDepth.data();

    for (unsigned int z = 0; z < n; z++){
        for (unsigned int x = 0; x < n; x++){
            const unsigned int cell = cellIndex(x, z, n);
            const float east = flux[cell * WATER_DIRS + 0];
            const float west = flux[cell * WATER_DIRS + 1];
            const float south = flux[cell * WATER_DIRS + 2];
            const float north = flux[cell * WATER_DIRS + 3];

            float in = 0.0f;
            if (x > 0) in += flux[cellIndex(x - 1, z, n) * WATER_DIRS + 0]; // the wall it faces here
            if (x + 1 < n) in += flux[cellIndex(x + 1, z, n) * WATER_DIRS + 1];
            if (z > 0) in += flux[cellIndex(x, z - 1, n) * WATER_DIRS + 2];
            if (z + 1 < n) in += flux[cellIndex(x, z + 1, n) * WATER_DIRS + 3];

            next[cell] = std::max(depth[cell] + (in - (east + west + south + north)) * dt / cellArea, 0.0f);

            // the average speed through the cell, which is what carries a floating body
            const float across = std::max(depth[cell] + next[cell], WATER_THIN_FILM) * 0.5f;
            velocity[cell * 2] = (east - west) / (lx * across);
            velocity[cell * 2 + 1] = (south - north) / (lz * across);
        }
    }

    std::copy(next, next + (size_t)n * n, water.simWater.begin());
    water.simDirty = true;
}

// The fast layer moves no volume, it only disturbs the surface, so a rain drop or a body
// hitting the water rings across the field and dies out.
void WaterSystem::stepRipple(WaterComponent& water, float dt){
    const unsigned int n = water.simCells;
    const float lx = std::max(water.simCell.x, WATER_MIN_CELL);
    const float lz = std::max(water.simCell.y, WATER_MIN_CELL);
    // an explicit step is only stable while the wave crosses a fraction of a cell per step
    const float speed = std::min(std::max(water.simRippleSpeed, 0.0f),
            WATER_RIPPLE_COURANT * std::min(lx, lz) / dt);
    const float c2 = speed * speed;
    const float damping = 1.0f / (1.0f + std::max(water.simRippleDamping, 0.0f) * dt);
    const float thin = std::max(water.simDryDepth * 4.0f, WATER_THIN_FILM);

    const float* depth = water.simWater.data();
    float* wave = water.simRipple.data();
    float* vel = water.simRippleVel.data();
    float* next = nextRipple.data();

    for (unsigned int z = 0; z < n; z++){
        for (unsigned int x = 0; x < n; x++){
            const unsigned int cell = cellIndex(x, z, n);
            // the walls of the field reflect, so a pool rings instead of leaking at the edge
            const unsigned int xm = (x > 0) ? x - 1 : x;
            const unsigned int xp = (x + 1 < n) ? x + 1 : x;
            const unsigned int zm = (z > 0) ? z - 1 : z;
            const unsigned int zp = (z + 1 < n) ? z + 1 : z;

            const float lapX = (wave[cellIndex(xp, z, n)] - 2.0f * wave[cell] + wave[cellIndex(xm, z, n)]) / (lx * lx);
            const float lapZ = (wave[cellIndex(x, zp, n)] - 2.0f * wave[cell] + wave[cellIndex(x, zm, n)]) / (lz * lz);

            // a film of water cannot carry a wave, so it puts the ripple out instead of
            // letting it grow on top of nothing
            const float carry = std::clamp(depth[cell] / thin, 0.0f, 1.0f);
            const float v = (vel[cell] + dt * c2 * (lapX + lapZ)) * damping * carry;
            vel[cell] = v;
            next[cell] = (wave[cell] + dt * v) * ((carry > 0.0f) ? damping : 0.0f);
        }
    }

    std::copy(next, next + (size_t)n * n, water.simRipple.begin());
}

// The sources and the sinks: rain fills the area, evaporation takes it back. A puddle the
// rain stopped filling is what finally disappears, which is the whole point of the pair.
void WaterSystem::stepSources(WaterComponent& water, float dt){
    const float rain = std::max(water.simRain, 0.0f) * dt;
    const float evaporate = std::max(water.simEvaporation, 0.0f) * dt;
    if (rain <= 0.0f && evaporate <= 0.0f)
        return;

    const unsigned int n = water.simCells;
    for (unsigned int i = 0; i < n * n; i++){
        water.simWater[i] = std::max(water.simWater[i] + rain - evaporate, 0.0f);
    }

    if (rain > 0.0f && water.simRipples)
        rainDrops(water, dt);

    water.simDirty = true;
}

// A few drops of the shower show up as rings. The volume itself is a broad source, so the
// drops only ever carry the look, never the water.
void WaterSystem::rainDrops(WaterComponent& water, float dt){
    const unsigned int n = water.simCells;
    const float strength = std::max(water.simRippleStrength, 0.0f);
    if (strength <= 0.0f)
        return;

    const float intensity = std::clamp(water.simRain * 4.0f, 0.0f, 1.0f);
    const float area = water.simCell.x * water.simCell.y * (float)(n * n);
    const float expected = WATER_DROPS_PER_SQUARE_METRE * intensity * area * dt;
    const unsigned int drops = (unsigned int)std::min(expected, (float)WATER_MAX_DROPS_PER_STEP);

    for (unsigned int d = 0; d < drops; d++){
        const unsigned int x = (unsigned int)(nextRandom() * (float)(n - 1));
        const unsigned int z = (unsigned int)(nextRandom() * (float)(n - 1));
        for (int dz = -1; dz <= 1; dz++){
            for (int dx = -1; dx <= 1; dx++){
                const int pickX = (int)x + dx;
                const int pickZ = (int)z + dz;
                if (pickX < 0 || pickZ < 0 || (unsigned int)pickX >= n || (unsigned int)pickZ >= n)
                    continue;
                const unsigned int cell = cellIndex((unsigned int)pickX, (unsigned int)pickZ, n);
                if (water.simWater[cell] <= 0.0f)
                    continue;
                // a drop pushes a small crater down, and the crater is what spreads
                water.simRipple[cell] -= strength * ((dx == 0 && dz == 0) ? 1.0f : 0.35f);
            }
        }
    }
}

// One RGBA32F texel per cell: r = depth, g = floor, b = ripple, a = flow speed. The drawn
// surface is g + r + b over the entity plane, and a is what turns into white water.
void WaterSystem::writeTexels(WaterComponent& water){
    const size_t cells = (size_t)water.simCells * (size_t)water.simCells;
    if (cells == 0 || water.simTexels.size() < cells * 4)
        return;

    float maxWater = 0.0f;
    for (size_t i = 0; i < cells; i++){
        const float depth = water.simWater[i];
        const float speed = std::sqrt(water.simVelocity[i * 2] * water.simVelocity[i * 2] +
                water.simVelocity[i * 2 + 1] * water.simVelocity[i * 2 + 1]);
        maxWater = std::max(maxWater, depth);

        float* texel = &water.simTexels[i * 4];
        texel[0] = depth;
        texel[1] = water.simFloor[i];
        texel[2] = water.simRipple[i];
        // fast water is white water, so a fall and a spilling edge read as foam by themselves
        texel[3] = speed;
    }

    water.simWaterMax = maxWater;
}

bool WaterSystem::getSimSurface(const WaterComponent& water, float x, float z, float& height, Vector3& normal, float& depth){
    if (!isSimulated(water))
        return false;

    const unsigned int n = water.simCells;
    const float fx = cellCoordinate(x, water.simOrigin.x, water.simCell.x);
    const float fz = cellCoordinate(z, water.simOrigin.y, water.simCell.y);
    if (fx < -0.5f || fz < -0.5f || fx > (float)n - 0.5f || fz > (float)n - 0.5f)
        return false;

    depth = sampleField(water.simWater, n, fx, fz);
    if (depth < std::max(water.simDryDepth, 0.0f))
        return false;

    height = surfaceHeight(water, n, fx, fz);

    // the slope of the field, which the drawn and the queried normal both add to the wave one
    const float dx = std::max(water.simCell.x, WATER_MIN_CELL);
    const float dz = std::max(water.simCell.y, WATER_MIN_CELL);
    const float right = surfaceHeight(water, n, fx + 1.0f, fz);
    const float left = surfaceHeight(water, n, fx - 1.0f, fz);
    const float below = surfaceHeight(water, n, fx, fz + 1.0f);
    const float above = surfaceHeight(water, n, fx, fz - 1.0f);
    normal = Vector3(-(right - left) / (2.0f * dx), 1.0f, -(below - above) / (2.0f * dz)).normalized();
    return true;
}

bool WaterSystem::getSimFlow(const WaterComponent& water, float x, float z, Vector3& velocity){
    if (!isSimulated(water) || water.simVelocity.empty())
        return false;

    const unsigned int n = water.simCells;
    const float fx = cellCoordinate(x, water.simOrigin.x, water.simCell.x);
    const float fz = cellCoordinate(z, water.simOrigin.y, water.simCell.y);
    if (fx < -0.5f || fz < -0.5f || fx > (float)n - 0.5f || fz > (float)n - 0.5f)
        return false;

    // a film the surface query already calls dry carries no current a body would feel
    if (sampleField(water.simWater, n, fx, fz) < std::max(water.simDryDepth, 0.0f))
        return false;

    const float* flow = water.simVelocity.data();
    velocity = Vector3(sampleField(flow, 2u, 0u, n, fx, fz), 0.0f, sampleField(flow, 2u, 1u, n, fx, fz));
    return true;
}

// A body hitting the water, a footstep, a bucket tipping over. The waters the point is not
// over are left alone, so a caller does not need to know which one it found.
void WaterSystem::addDisturbance(const Vector3& worldPosition, float radius, float strength){
    auto waters = scene->getComponentArray<WaterComponent>();
    if (!waters)
        return;

    for (int i = 0; i < waters->size(); i++){
        WaterComponent& water = waters->getComponentFromIndex(i);
        if (!isSimulated(water))
            continue;

        const unsigned int n = water.simCells;
        const float fx = cellCoordinate(worldPosition.x, water.simOrigin.x, water.simCell.x);
        const float fz = cellCoordinate(worldPosition.z, water.simOrigin.y, water.simCell.y);
        const float reach = std::max(radius, 0.0f) / std::max(water.simCell.x, WATER_MIN_CELL);
        if (fx < -reach - 1.0f || fz < -reach - 1.0f || fx > (float)n + reach || fz > (float)n + reach)
            continue;

        const int cells = (int)std::ceil(reach);
        for (int dz = -cells; dz <= cells; dz++){
            for (int dx = -cells; dx <= cells; dx++){
                const int tapX = (int)std::round(fx) + dx;
                const int tapZ = (int)std::round(fz) + dz;
                if (tapX < 0 || tapZ < 0 || (unsigned int)tapX >= n || (unsigned int)tapZ >= n)
                    continue;
                const float dist = std::sqrt((float)(dx * dx + dz * dz)) / std::max(reach, 0.5f);
                if (dist > 1.0f)
                    continue;
                const unsigned int cell = cellIndex((unsigned int)tapX, (unsigned int)tapZ, n);
                if (water.simWater[cell] <= 0.0f)
                    continue;
                water.simRipple[cell] -= strength * (1.0f - dist * dist);
                water.simDirty = true;
            }
        }
    }
}

// The heightfield is a grid at one altitude: it can run water sideways and dry a cell out, but it
// has no way to let water *fall*, so a cliff lip is a wall. These two are the jump a script uses
// to move the volume from the field above the lip into the one below it.
//
// Both spread over a disc with a 1 - d² falloff, because a point source would pile one cell up and
// the flow step would spend the rest of the run flattening it back out.
void WaterSystem::addWater(WaterComponent& water, const Vector3& worldPosition, float radius, float cubicMetres){
    if (cubicMetres <= 0.0f || !isSimulated(water))
        return;

    const unsigned int n = water.simCells;
    const float fx = cellCoordinate(worldPosition.x, water.simOrigin.x, water.simCell.x);
    const float fz = cellCoordinate(worldPosition.z, water.simOrigin.y, water.simCell.y);
    const float cellArea = std::max(water.simCell.x, WATER_MIN_CELL) * std::max(water.simCell.y, WATER_MIN_CELL);
    const float reach = std::max(radius, 0.0f) / std::max(water.simCell.x, WATER_MIN_CELL);
    const int span = (int)std::ceil(reach);
    const int centreX = (int)std::round(fx);
    const int centreZ = (int)std::round(fz);

    // two passes over the same small disc: the weights have to be totalled before any of them can
    // be turned into metres, or the poured volume would depend on how much of the disc the field
    // happened to clip at the border
    float weightSum = 0.0f;
    for (int dz = -span; dz <= span; dz++){
        for (int dx = -span; dx <= span; dx++){
            const int tapX = centreX + dx;
            const int tapZ = centreZ + dz;
            if (tapX < 0 || tapZ < 0 || (unsigned int)tapX >= n || (unsigned int)tapZ >= n)
                continue;
            const float dist = std::sqrt((float)(dx * dx + dz * dz)) / std::max(reach, 0.5f);
            if (dist > 1.0f)
                continue;
            weightSum += 1.0f - dist * dist;
        }
    }
    if (weightSum <= 0.0f)
        return;

    for (int dz = -span; dz <= span; dz++){
        for (int dx = -span; dx <= span; dx++){
            const int tapX = centreX + dx;
            const int tapZ = centreZ + dz;
            if (tapX < 0 || tapZ < 0 || (unsigned int)tapX >= n || (unsigned int)tapZ >= n)
                continue;
            const float dist = std::sqrt((float)(dx * dx + dz * dz)) / std::max(reach, 0.5f);
            if (dist > 1.0f)
                continue;
            const unsigned int cell = cellIndex((unsigned int)tapX, (unsigned int)tapZ, n);
            water.simWater[cell] += cubicMetres * ((1.0f - dist * dist) / weightSum) / cellArea;
            water.simDirty = true;
        }
    }
}

float WaterSystem::drainWater(WaterComponent& water, const Vector3& worldPosition, float radius, float cubicMetres){
    if (cubicMetres <= 0.0f || !isSimulated(water))
        return 0.0f;

    const unsigned int n = water.simCells;
    const float fx = cellCoordinate(worldPosition.x, water.simOrigin.x, water.simCell.x);
    const float fz = cellCoordinate(worldPosition.z, water.simOrigin.y, water.simCell.y);
    const float cellArea = std::max(water.simCell.x, WATER_MIN_CELL) * std::max(water.simCell.y, WATER_MIN_CELL);
    const float reach = std::max(radius, 0.0f) / std::max(water.simCell.x, WATER_MIN_CELL);
    const int span = (int)std::ceil(reach);
    const int centreX = (int)std::round(fx);
    const int centreZ = (int)std::round(fz);

    float weightSum = 0.0f;
    float available = 0.0f;
    for (int dz = -span; dz <= span; dz++){
        for (int dx = -span; dx <= span; dx++){
            const int tapX = centreX + dx;
            const int tapZ = centreZ + dz;
            if (tapX < 0 || tapZ < 0 || (unsigned int)tapX >= n || (unsigned int)tapZ >= n)
                continue;
            const float dist = std::sqrt((float)(dx * dx + dz * dz)) / std::max(reach, 0.5f);
            if (dist > 1.0f)
                continue;
            const float weight = 1.0f - dist * dist;
            weightSum += weight;
            available += water.simWater[cellIndex((unsigned int)tapX, (unsigned int)tapZ, n)] * cellArea * weight;
        }
    }
    if (weightSum <= 0.0f || available <= 0.0f)
        return 0.0f;

    // One scale factor for the whole disc, so the drain is proportional to what each cell holds:
    // a rim that only just has water is not dug a hole down to the bed.
    const float taken = std::min(cubicMetres, available);
    const float scale = taken / available;

    for (int dz = -span; dz <= span; dz++){
        for (int dx = -span; dx <= span; dx++){
            const int tapX = centreX + dx;
            const int tapZ = centreZ + dz;
            if (tapX < 0 || tapZ < 0 || (unsigned int)tapX >= n || (unsigned int)tapZ >= n)
                continue;
            const float dist = std::sqrt((float)(dx * dx + dz * dz)) / std::max(reach, 0.5f);
            if (dist > 1.0f)
                continue;
            const unsigned int cell = cellIndex((unsigned int)tapX, (unsigned int)tapZ, n);
            water.simWater[cell] = std::max(water.simWater[cell] * (1.0f - scale), 0.0f);
            water.simDirty = true;
        }
    }
    return taken;
}

double WaterSystem::getVolume(Entity entity) const{
    const WaterComponent* water = scene->findComponent<WaterComponent>(entity);
    if (!water || water->simCells == 0)
        return 0.0;

    double cubicMetres = 0.0;
    for (const float depth : water->simWater){
        cubicMetres += depth;
    }
    return cubicMetres * (double)water->simCell.x * (double)water->simCell.y * 1000.0; // litres
}
