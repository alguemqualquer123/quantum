// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef Noise_h
#define Noise_h

#include "Export.h"

#include <cstdint>

namespace quantum {

    // Which lattice a fractal sum runs over. All three return [-1, 1], so a script can swap one for
    // another without rescaling: VALUE is the cheapest and shows the grid, PERLIN and SIMPLEX are
    // gradient noises, SIMPLEX having the least axis-aligned structure.
    enum class NoiseType : int {
        PERLIN = 0,
        SIMPLEX = 1,
        VALUE = 2
    };

    // CPU procedural noise, as pure functions of (coordinates, seed): no global state, no allocation,
    // no thread affinity, so a pooled world-generation task and the main thread can sample the same
    // field. Hashing is integer-only, so a seed baked into a scene returns the same map on MSVC,
    // MinGW and every machine - rand(), std::hash and float bit tricks do not.
    // Coordinates are lattice units, one cell per unit: sample at world position divided by a scale,
    // never at a pixel index, so a zoom and a tiling agree.
    class QUANTUM_API Noise {
    public:
        // Hard octave cap. fBm runs inside per-pixel loops, and a count coming from an inspector row
        // must cost detail when it goes wrong, not hang the frame.
        static const int MAX_OCTAVES = 16;

        // Classic Perlin gradient noise, [ -1, 1 ].
        static float perlin2(float x, float y, uint32_t seed);
        static float perlin3(float x, float y, float z, uint32_t seed);

        // Simplex gradient noise, [ -1, 1 ]. Fewer lattice corners than Perlin and less axis bias,
        // which is what makes it the better choice for clouds, smoke and water detail.
        static float simplex2(float x, float y, uint32_t seed);
        static float simplex3(float x, float y, float z, uint32_t seed);

        // Value noise: hashed lattice corners, interpolated. [ -1, 1 ]. value2 is the editor brush's
        // quantum::editor::TerrainNoise::value remapped from [0,1], so an editor-baked heightmap and a
        // runtime script agree for the same seed.
        static float value2(float x, float y, uint32_t seed);
        static float value3(float x, float y, float z, uint32_t seed);

        // White noise: the hash of a single lattice point in [0,1], interpolated by nothing. Use it for
        // jitter and per-tile randomness, where a smooth field would be wrong.
        static float hash2(int x, int y, uint32_t seed);
        static float hash3(int x, int y, int z, uint32_t seed);

        // Dispatch by type: the entry point for a script that holds the type as data (an inspector row,
        // a saved setting) instead of naming a function. An unknown type returns 0.0f - nothing here
        // throws, because exceptions cannot cross a pooled task in this engine.
        static float sample(NoiseType type, float x, float y, uint32_t seed);
        static float sample3(NoiseType type, float x, float y, float z, uint32_t seed);

        // fractal Brownian motion: `octaves` samples at lacunarity-stepped frequency and gain-stepped
        // weight, divided by the weight sum so the result stays in the base noise range. octaves is
        // clamped to [1, MAX_OCTAVES], gain to [0, 1] (an octave never outweighs the previous one) and
        // lacunarity to [1, 8]; one octave is exactly the base sample.
        static float fbm(NoiseType type, float x, float y, uint32_t seed, int octaves, float lacunarity, float gain);
        static float fbm3(NoiseType type, float x, float y, float z, uint32_t seed, int octaves, float lacunarity, float gain);

        // Turbulence: the same sum of the absolute values, so [0, 1] for these bases - ridged detail for
        // clouds, fire and shore foam. Same clamping as fbm.
        static float turbulence(NoiseType type, float x, float y, uint32_t seed, int octaves, float lacunarity, float gain);
        static float turbulence3(NoiseType type, float x, float y, float z, uint32_t seed, int octaves, float lacunarity, float gain);

        // Ridged multifractal: each octave contributes 1 - |sample| squared, so the crests where the
        // field crosses zero become sharp ridges instead of the rounded hills fbm gives. [0, 1], the
        // range of turbulence, and the same clamping. This is the mountain-divide and ocean-floor
        // fracture shape.
        static float ridged(NoiseType type, float x, float y, uint32_t seed, int octaves, float lacunarity, float gain);
        static float ridged3(NoiseType type, float x, float y, float z, uint32_t seed, int octaves, float lacunarity, float gain);

        // Domain warping as a coordinate displacement: x and y (or x, y, z) come back moved by a
        // simplex field, which is what turns an axis-aligned sum into coastlines, marble veins and
        // eroded ridges. The displacement is a fixed 3-octave sum read from seeds derived from `seed`,
        // so the axes never line up on one lattice. warpAmplitude is in lattice units of the caller's
        // own coordinates, so it scales with the frequency the caller samples at, not with the map
        // size. Simplex is used for the displacement whatever the caller samples afterwards: it has the
        // least axis-aligned structure, and a grid bias in a warp field prints a grid bias into the
        // result.
        static void warp2(float& x, float& y, uint32_t seed, float warpAmplitude, float warpFrequency);
        static void warp3(float& x, float& y, float& z, uint32_t seed, float warpAmplitude, float warpFrequency);

        // The fbm of a warped point, for the common case of a script that only wants one number out.
        // [-1, 1], like fbm. warpFrequency scales the displacement field itself.
        static float warpedFbm(NoiseType type, float x, float y, uint32_t seed, int octaves, float lacunarity,
            float gain, float warpAmplitude, float warpFrequency);
        static float warpedFbm3(NoiseType type, float x, float y, float z, uint32_t seed, int octaves,
            float lacunarity, float gain, float warpAmplitude, float warpFrequency);
    };
}

#endif /* Noise_h */
