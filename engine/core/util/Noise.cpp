// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "Noise.h"

#include <cmath>

using namespace quantum;

namespace {

    // Integer-only hashing, shared with the editor brush (quantum::editor::TerrainNoise): the same
    // constants are what make an editor-baked map and a runtime sample land on one lattice.
    inline uint32_t mixFinalizer(uint32_t h) {
        h ^= h >> 16;
        h *= 0x7FEB352Du;
        h ^= h >> 15;
        h *= 0x846CA68Bu;
        h ^= h >> 16;
        return h;
    }

    inline uint32_t mixCoordinate(uint32_t h, int coordinate, uint32_t salt) {
        const uint32_t value = static_cast<uint32_t>(coordinate);
        return h ^ (value + salt + (h << 6) + (h >> 2));
    }

    inline uint32_t latticeHash(int x, int y, uint32_t seed) {
        uint32_t h = seed ^ 0x9E3779B9u;
        h = mixCoordinate(h, x, 0x85EBCA6Bu);
        h = mixCoordinate(h, y, 0xC2B2AE35u);
        return mixFinalizer(h);
    }

    inline uint32_t latticeHash(int x, int y, int z, uint32_t seed) {
        uint32_t h = seed ^ 0x9E3779B9u;
        h = mixCoordinate(h, x, 0x85EBCA6Bu);
        h = mixCoordinate(h, y, 0xC2B2AE35u);
        h = mixCoordinate(h, z, 0x27D4EB2Fu);
        return mixFinalizer(h);
    }

    // 24 of the 32 bits, because 2^24 divides the float mantissa exactly: the division needs no
    // rounding rule to agree between compilers, and the result can never reach 1.0.
    inline float unitValue(uint32_t h) {
        return static_cast<float>(h & 0x00FFFFFFu) * (1.0f / 16777215.0f);
    }

    inline float signedValue(uint32_t h) {
        return unitValue(h) * 2.0f - 1.0f;
    }

    // Perlin's improved curve: zero first and second derivative on the lattice, which is what makes a
    // gradient field continuous across cell borders.
    inline float fade(float t) {
        return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
    }

    // The curve quantum::editor::TerrainNoise uses, kept for value noise so a seed reads the same on
    // both sides of the editor/engine boundary.
    inline float smooth(float t) {
        return t * t * (3.0f - 2.0f * t);
    }

    inline float lerp(float a, float b, float t) {
        return a + (b - a) * t;
    }

    const float INVERSE_SQRT2 = 0.7071067811865476f;
    const float SQRT2 = 1.4142135623730951f;

    // Unit-length gradients (the reference tables use components in {-1, 0, 1} instead, whose longest
    // vector is sqrt(2)); unit length is what lets one scale factor put every base in [-1, 1].
    inline void gradient2(uint32_t h, float& gx, float& gy) {
        switch (h & 7u) {
            case 0: gx = 1.0f; gy = 0.0f; break;
            case 1: gx = -1.0f; gy = 0.0f; break;
            case 2: gx = 0.0f; gy = 1.0f; break;
            case 3: gx = 0.0f; gy = -1.0f; break;
            case 4: gx = INVERSE_SQRT2; gy = INVERSE_SQRT2; break;
            case 5: gx = -INVERSE_SQRT2; gy = INVERSE_SQRT2; break;
            case 6: gx = INVERSE_SQRT2; gy = -INVERSE_SQRT2; break;
            default: gx = -INVERSE_SQRT2; gy = -INVERSE_SQRT2; break;
        }
    }

    // The 12 edge midpoints of the cube, normalized.
    inline void gradient3(uint32_t h, float& gx, float& gy, float& gz) {
        const float k = INVERSE_SQRT2;
        switch (h % 12u) {
            case 0: gx = k; gy = k; gz = 0.0f; break;
            case 1: gx = -k; gy = k; gz = 0.0f; break;
            case 2: gx = k; gy = -k; gz = 0.0f; break;
            case 3: gx = -k; gy = -k; gz = 0.0f; break;
            case 4: gx = k; gy = 0.0f; gz = k; break;
            case 5: gx = -k; gy = 0.0f; gz = k; break;
            case 6: gx = k; gy = 0.0f; gz = -k; break;
            case 7: gx = -k; gy = 0.0f; gz = -k; break;
            case 8: gx = 0.0f; gy = k; gz = k; break;
            case 9: gx = 0.0f; gy = -k; gz = k; break;
            case 10: gx = 0.0f; gy = k; gz = -k; break;
            default: gx = 0.0f; gy = -k; gz = -k; break;
        }
    }

    // The scale factors below bound the analytic peak of each base (Perlin: the cell centre, every
    // gradient pointing at it, sqrt(N)/2). The clamp is the guard for the interpolation overshoot
    // that bound does not cover, so the documented [-1, 1] contract always holds.
    inline float clampUnit(float value) {
        if (value <= -1.0f) return -1.0f;
        if (value >= 1.0f) return 1.0f;
        return value;
    }

    // The lattice index is an int, so NaN or a coordinate past 2^30 would be undefined behaviour on the
    // cast. A generator that walks off the edge reads flat rather than crashing the process.
    inline bool inLatticeRange(float x) {
        return x == x && x < 1000000000.0f && x > -1000000000.0f;
    }

    inline bool inLatticeRange(float x, float y) {
        return inLatticeRange(x) && inLatticeRange(y);
    }

    inline bool inLatticeRange(float x, float y, float z) {
        return inLatticeRange(x) && inLatticeRange(y) && inLatticeRange(z);
    }

    // A cleared inspector row reaches these as NaN; falling back to the classic values keeps a bad
    // parameter producing detail instead of nothing.
    inline float sanitize(float value, float low, float high, float fallback) {
        if (value != value) return fallback;
        if (value < low) return low;
        if (value > high) return high;
        return value;
    }

    inline int sanitizeOctaves(int octaves) {
        if (octaves < 1) return 1;
        if (octaves > Noise::MAX_OCTAVES) return Noise::MAX_OCTAVES;
        return octaves;
    }

    const float SKEW_2D = 0.3660254037844386f;   // (sqrt(3) - 1) / 2
    const float UNSKEW_2D = 0.2113248654051871f; // (3 - sqrt(3)) / 6
    const float SKEW_3D = 0.3333333333333333f;   // 1 / 3
    const float UNSKEW_3D = 0.1666666666666667f; // 1 / 6

    // 70.0 and 32.0 are the reference simplex scales for gradients up to sqrt(2) long; the unit
    // gradients above are shorter by that factor, so they need it back.
    const float SIMPLEX_2D_SCALE = 70.0f * SQRT2;
    const float SIMPLEX_3D_SCALE = 32.0f * SQRT2;

    float corner2(int x, int y, float dx, float dy, uint32_t seed) {
        float gx, gy;
        gradient2(latticeHash(x, y, seed), gx, gy);
        return gx * dx + gy * dy;
    }

    float corner3(int x, int y, int z, float dx, float dy, float dz, uint32_t seed) {
        float gx, gy, gz;
        gradient3(latticeHash(x, y, z, seed), gx, gy, gz);
        return gx * dx + gy * dy + gz * dz;
    }

    // Simplex corners fall out of the lattice: (0.5 - r^2)^4 (2D) and (0.6 - r^2)^4 (3D) times the
    // gradient dot, which is exactly zero on the cell edge, so the sum needs no blend weights.
    float simplexCorner(uint32_t hash, float dx, float dy, float radiusSquared) {
        const float t = 0.5f - radiusSquared;
        if (t <= 0.0f) return 0.0f;
        float gx, gy;
        gradient2(hash, gx, gy);
        const float tt = t * t;
        return tt * tt * (gx * dx + gy * dy);
    }

    float simplexCorner(uint32_t hash, float dx, float dy, float dz, float radiusSquared) {
        const float t = 0.6f - radiusSquared;
        if (t <= 0.0f) return 0.0f;
        float gx, gy, gz;
        gradient3(hash, gx, gy, gz);
        const float tt = t * t;
        return tt * tt * (gx * dx + gy * dy + gz * dz);
    }

    // How an octave sum shapes each sample: raw, rectified (turbulence) or inverted around the zero
    // crossing (ridged).
    enum class OctaveShape {
        PLAIN,
        ABSOLUTE,
        RIDGE
    };

    inline float shapeSample(float sample, OctaveShape shape) {
        switch (shape) {
            case OctaveShape::ABSOLUTE: return std::fabs(sample);
            case OctaveShape::RIDGE: {
                const float ridge = 1.0f - std::fabs(sample);
                return ridge * ridge;
            }
            default: return sample;
        }
    }

    float octaves2(NoiseType type, float x, float y, uint32_t seed, int count, float lacunarity, float gain, OctaveShape shape) {
        float sum = 0.0f;
        float weight = 1.0f;
        float weightSum = 0.0f;
        float frequency = 1.0f;

        for (int o = 0; o < count; o++) {
            // The per-octave seed stride keeps adjacent octaves off one lattice, which would otherwise
            // reinforce each other and print a grid into the result. Same stride as the editor brush.
            float sample = Noise::sample(type, x * frequency, y * frequency,
                seed + static_cast<uint32_t>(o * 1013));
            sum += shapeSample(sample, shape) * weight;
            weightSum += weight;
            weight *= gain;
            frequency *= lacunarity;
        }

        return weightSum > 0.0f ? sum / weightSum : 0.0f;
    }

    float octaves3(NoiseType type, float x, float y, float z, uint32_t seed, int count, float lacunarity, float gain, OctaveShape shape) {
        float sum = 0.0f;
        float weight = 1.0f;
        float weightSum = 0.0f;
        float frequency = 1.0f;

        for (int o = 0; o < count; o++) {
            float sample = Noise::sample3(type, x * frequency, y * frequency, z * frequency,
                seed + static_cast<uint32_t>(o * 1013));
            sum += shapeSample(sample, shape) * weight;
            weightSum += weight;
            weight *= gain;
            frequency *= lacunarity;
        }

        return weightSum > 0.0f ? sum / weightSum : 0.0f;
    }

    // The two displacement fields of a domain warp read from these, so a warp of one seed cannot line
    // its x and y offsets up on one lattice - that would shear the field along a diagonal instead of
    // folding it.
    const uint32_t WARP_SEED_X = 0x9E3779B9u;
    const uint32_t WARP_SEED_Y = 0x85EBCA6Bu;
    const uint32_t WARP_SEED_Z = 0xC2B2AE35u;
    const int WARP_OCTAVES = 3;
}

float Noise::perlin2(float x, float y, uint32_t seed) {
    if (!inLatticeRange(x, y)) return 0.0f;

    const int ix = static_cast<int>(std::floor(x));
    const int iy = static_cast<int>(std::floor(y));
    const float fx = x - static_cast<float>(ix);
    const float fy = y - static_cast<float>(iy);

    const float n00 = corner2(ix, iy, fx, fy, seed);
    const float n10 = corner2(ix + 1, iy, fx - 1.0f, fy, seed);
    const float n01 = corner2(ix, iy + 1, fx, fy - 1.0f, seed);
    const float n11 = corner2(ix + 1, iy + 1, fx - 1.0f, fy - 1.0f, seed);

    const float ux = fade(fx);
    const float uy = fade(fy);
    return clampUnit(lerp(lerp(n00, n10, ux), lerp(n01, n11, ux), uy) * SQRT2);
}

float Noise::perlin3(float x, float y, float z, uint32_t seed) {
    if (!inLatticeRange(x, y, z)) return 0.0f;

    const int ix = static_cast<int>(std::floor(x));
    const int iy = static_cast<int>(std::floor(y));
    const int iz = static_cast<int>(std::floor(z));
    const float fx = x - static_cast<float>(ix);
    const float fy = y - static_cast<float>(iy);
    const float fz = z - static_cast<float>(iz);

    const float n000 = corner3(ix, iy, iz, fx, fy, fz, seed);
    const float n100 = corner3(ix + 1, iy, iz, fx - 1.0f, fy, fz, seed);
    const float n010 = corner3(ix, iy + 1, iz, fx, fy - 1.0f, fz, seed);
    const float n110 = corner3(ix + 1, iy + 1, iz, fx - 1.0f, fy - 1.0f, fz, seed);
    const float n001 = corner3(ix, iy, iz + 1, fx, fy, fz - 1.0f, seed);
    const float n101 = corner3(ix + 1, iy, iz + 1, fx - 1.0f, fy, fz - 1.0f, seed);
    const float n011 = corner3(ix, iy + 1, iz + 1, fx, fy - 1.0f, fz - 1.0f, seed);
    const float n111 = corner3(ix + 1, iy + 1, iz + 1, fx - 1.0f, fy - 1.0f, fz - 1.0f, seed);

    const float ux = fade(fx);
    const float uy = fade(fy);
    const float uz = fade(fz);
    // Peak of the 3D lattice is sqrt(3)/2, so the inverse of that brings it back to 1.
    return clampUnit(lerp(
        lerp(lerp(n000, n100, ux), lerp(n010, n110, ux), uy),
        lerp(lerp(n001, n101, ux), lerp(n011, n111, ux), uy), uz) * 1.1547005383792516f);
}

float Noise::simplex2(float x, float y, uint32_t seed) {
    if (!inLatticeRange(x, y)) return 0.0f;

    // Skewing the square lattice onto the triangular one is what leaves three corners to blend.
    const float skew = (x + y) * SKEW_2D;
    const float sx = x + skew;
    const float sy = y + skew;
    const int i = static_cast<int>(std::floor(sx));
    const int j = static_cast<int>(std::floor(sy));
    const float unscrew = static_cast<float>(i + j) * UNSKEW_2D;

    const float x0 = x - (static_cast<float>(i) - unscrew);
    const float y0 = y - (static_cast<float>(j) - unscrew);

    // The two in-between corners of whichever half of the skewed square the point sits in.
    const bool lower = x0 > y0;
    const int i1 = lower ? 1 : 0;
    const int j1 = lower ? 0 : 1;

    const float x1 = x0 - static_cast<float>(i1) + UNSKEW_2D;
    const float y1 = y0 - static_cast<float>(j1) + UNSKEW_2D;
    const float x2 = x0 - 1.0f + 2.0f * UNSKEW_2D;
    const float y2 = y0 - 1.0f + 2.0f * UNSKEW_2D;

    const float sum =
        simplexCorner(latticeHash(i, j, seed), x0, y0, x0 * x0 + y0 * y0) +
        simplexCorner(latticeHash(i + i1, j + j1, seed), x1, y1, x1 * x1 + y1 * y1) +
        simplexCorner(latticeHash(i + 1, j + 1, seed), x2, y2, x2 * x2 + y2 * y2);

    return clampUnit(sum * SIMPLEX_2D_SCALE);
}

float Noise::simplex3(float x, float y, float z, uint32_t seed) {
    if (!inLatticeRange(x, y, z)) return 0.0f;

    const float skew = (x + y + z) * SKEW_3D;
    const int i = static_cast<int>(std::floor(x + skew));
    const int j = static_cast<int>(std::floor(y + skew));
    const int k = static_cast<int>(std::floor(z + skew));
    const float unscrew = static_cast<float>(i + j + k) * UNSKEW_3D;

    const float x0 = x - (static_cast<float>(i) - unscrew);
    const float y0 = y - (static_cast<float>(j) - unscrew);
    const float z0 = z - (static_cast<float>(k) - unscrew);

    // Ranking the three offsets picks one of the six tetrahedra of the rhombohedron.
    int i1, j1, k1, i2, j2, k2;
    if (x0 >= y0) {
        if (y0 >= z0) { i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 1; k2 = 0; }
        else if (x0 >= z0) { i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 0; k2 = 1; }
        else { i1 = 0; j1 = 0; k1 = 1; i2 = 1; j2 = 0; k2 = 1; }
    } else {
        if (y0 < z0) { i1 = 0; j1 = 0; k1 = 1; i2 = 0; j2 = 1; k2 = 1; }
        else if (x0 < z0) { i1 = 0; j1 = 1; k1 = 0; i2 = 0; j2 = 1; k2 = 1; }
        else { i1 = 0; j1 = 1; k1 = 0; i2 = 1; j2 = 1; k2 = 0; }
    }

    const float x1 = x0 - static_cast<float>(i1) + UNSKEW_3D;
    const float y1 = y0 - static_cast<float>(j1) + UNSKEW_3D;
    const float z1 = z0 - static_cast<float>(k1) + UNSKEW_3D;
    const float x2 = x0 - static_cast<float>(i2) + 2.0f * UNSKEW_3D;
    const float y2 = y0 - static_cast<float>(j2) + 2.0f * UNSKEW_3D;
    const float z2 = z0 - static_cast<float>(k2) + 2.0f * UNSKEW_3D;
    const float x3 = x0 - 1.0f + 3.0f * UNSKEW_3D;
    const float y3 = y0 - 1.0f + 3.0f * UNSKEW_3D;
    const float z3 = z0 - 1.0f + 3.0f * UNSKEW_3D;

    const float sum =
        simplexCorner(latticeHash(i, j, k, seed), x0, y0, z0, x0 * x0 + y0 * y0 + z0 * z0) +
        simplexCorner(latticeHash(i + i1, j + j1, k + k1, seed), x1, y1, z1, x1 * x1 + y1 * y1 + z1 * z1) +
        simplexCorner(latticeHash(i + i2, j + j2, k + k2, seed), x2, y2, z2, x2 * x2 + y2 * y2 + z2 * z2) +
        simplexCorner(latticeHash(i + 1, j + 1, k + 1, seed), x3, y3, z3, x3 * x3 + y3 * y3 + z3 * z3);

    return clampUnit(sum * SIMPLEX_3D_SCALE);
}

float Noise::value2(float x, float y, uint32_t seed) {
    if (!inLatticeRange(x, y)) return 0.0f;

    const int ix = static_cast<int>(std::floor(x));
    const int iy = static_cast<int>(std::floor(y));
    const float fx = x - static_cast<float>(ix);
    const float fy = y - static_cast<float>(iy);

    const float a = signedValue(latticeHash(ix, iy, seed));
    const float b = signedValue(latticeHash(ix + 1, iy, seed));
    const float c = signedValue(latticeHash(ix, iy + 1, seed));
    const float d = signedValue(latticeHash(ix + 1, iy + 1, seed));

    const float ux = smooth(fx);
    const float uy = smooth(fy);
    // A convex blend of four values already in [-1, 1] cannot leave it, so no clamp here.
    return lerp(lerp(a, b, ux), lerp(c, d, ux), uy);
}

float Noise::value3(float x, float y, float z, uint32_t seed) {
    if (!inLatticeRange(x, y, z)) return 0.0f;

    const int ix = static_cast<int>(std::floor(x));
    const int iy = static_cast<int>(std::floor(y));
    const int iz = static_cast<int>(std::floor(z));
    const float fx = x - static_cast<float>(ix);
    const float fy = y - static_cast<float>(iy);
    const float fz = z - static_cast<float>(iz);

    const float ux = smooth(fx);
    const float uy = smooth(fy);
    const float uz = smooth(fz);

    const float a = signedValue(latticeHash(ix, iy, iz, seed));
    const float b = signedValue(latticeHash(ix + 1, iy, iz, seed));
    const float c = signedValue(latticeHash(ix, iy + 1, iz, seed));
    const float d = signedValue(latticeHash(ix + 1, iy + 1, iz, seed));
    const float e = signedValue(latticeHash(ix, iy, iz + 1, seed));
    const float f = signedValue(latticeHash(ix + 1, iy, iz + 1, seed));
    const float g = signedValue(latticeHash(ix, iy + 1, iz + 1, seed));
    const float h = signedValue(latticeHash(ix + 1, iy + 1, iz + 1, seed));

    return lerp(lerp(lerp(a, b, ux), lerp(c, d, ux), uy),
        lerp(lerp(e, f, ux), lerp(g, h, ux), uy), uz);
}

float Noise::hash2(int x, int y, uint32_t seed) {
    return unitValue(latticeHash(x, y, seed));
}

float Noise::hash3(int x, int y, int z, uint32_t seed) {
    return unitValue(latticeHash(x, y, z, seed));
}

float Noise::sample(NoiseType type, float x, float y, uint32_t seed) {
    switch (type) {
        case NoiseType::PERLIN: return perlin2(x, y, seed);
        case NoiseType::SIMPLEX: return simplex2(x, y, seed);
        case NoiseType::VALUE: return value2(x, y, seed);
        // A value outside the enum comes from a script or a scene written by an older build, and
        // nothing here may throw, so an unknown base reads flat.
        default: return 0.0f;
    }
}

float Noise::sample3(NoiseType type, float x, float y, float z, uint32_t seed) {
    switch (type) {
        case NoiseType::PERLIN: return perlin3(x, y, z, seed);
        case NoiseType::SIMPLEX: return simplex3(x, y, z, seed);
        case NoiseType::VALUE: return value3(x, y, z, seed);
        default: return 0.0f;
    }
}

float Noise::fbm(NoiseType type, float x, float y, uint32_t seed, int octaves, float lacunarity, float gain) {
    return octaves2(type, x, y, seed, sanitizeOctaves(octaves),
        sanitize(lacunarity, 1.0f, 8.0f, 2.0f), sanitize(gain, 0.0f, 1.0f, 0.5f), OctaveShape::PLAIN);
}

float Noise::fbm3(NoiseType type, float x, float y, float z, uint32_t seed, int octaves, float lacunarity, float gain) {
    return octaves3(type, x, y, z, seed, sanitizeOctaves(octaves),
        sanitize(lacunarity, 1.0f, 8.0f, 2.0f), sanitize(gain, 0.0f, 1.0f, 0.5f), OctaveShape::PLAIN);
}

float Noise::turbulence(NoiseType type, float x, float y, uint32_t seed, int octaves, float lacunarity, float gain) {
    return octaves2(type, x, y, seed, sanitizeOctaves(octaves),
        sanitize(lacunarity, 1.0f, 8.0f, 2.0f), sanitize(gain, 0.0f, 1.0f, 0.5f), OctaveShape::ABSOLUTE);
}

float Noise::turbulence3(NoiseType type, float x, float y, float z, uint32_t seed, int octaves, float lacunarity, float gain) {
    return octaves3(type, x, y, z, seed, sanitizeOctaves(octaves),
        sanitize(lacunarity, 1.0f, 8.0f, 2.0f), sanitize(gain, 0.0f, 1.0f, 0.5f), OctaveShape::ABSOLUTE);
}

float Noise::ridged(NoiseType type, float x, float y, uint32_t seed, int octaves, float lacunarity, float gain) {
    return octaves2(type, x, y, seed, sanitizeOctaves(octaves),
        sanitize(lacunarity, 1.0f, 8.0f, 2.0f), sanitize(gain, 0.0f, 1.0f, 0.5f), OctaveShape::RIDGE);
}

float Noise::ridged3(NoiseType type, float x, float y, float z, uint32_t seed, int octaves, float lacunarity, float gain) {
    return octaves3(type, x, y, z, seed, sanitizeOctaves(octaves),
        sanitize(lacunarity, 1.0f, 8.0f, 2.0f), sanitize(gain, 0.0f, 1.0f, 0.5f), OctaveShape::RIDGE);
}

void Noise::warp2(float& x, float& y, uint32_t seed, float warpAmplitude, float warpFrequency) {
    const float amplitude = sanitize(warpAmplitude, 0.0f, 16.0f, 1.0f);
    const float frequency = sanitize(warpFrequency, 0.0f, 8.0f, 1.0f);

    const float wx = octaves2(NoiseType::SIMPLEX, x * frequency, y * frequency, seed ^ WARP_SEED_X,
        WARP_OCTAVES, 2.0f, 0.5f, OctaveShape::PLAIN);
    const float wy = octaves2(NoiseType::SIMPLEX, x * frequency, y * frequency, seed ^ WARP_SEED_Y,
        WARP_OCTAVES, 2.0f, 0.5f, OctaveShape::PLAIN);

    x += amplitude * wx;
    y += amplitude * wy;
}

void Noise::warp3(float& x, float& y, float& z, uint32_t seed, float warpAmplitude, float warpFrequency) {
    const float amplitude = sanitize(warpAmplitude, 0.0f, 16.0f, 1.0f);
    const float frequency = sanitize(warpFrequency, 0.0f, 8.0f, 1.0f);

    const float wx = octaves3(NoiseType::SIMPLEX, x * frequency, y * frequency, z * frequency,
        seed ^ WARP_SEED_X, WARP_OCTAVES, 2.0f, 0.5f, OctaveShape::PLAIN);
    const float wy = octaves3(NoiseType::SIMPLEX, x * frequency, y * frequency, z * frequency,
        seed ^ WARP_SEED_Y, WARP_OCTAVES, 2.0f, 0.5f, OctaveShape::PLAIN);
    const float wz = octaves3(NoiseType::SIMPLEX, x * frequency, y * frequency, z * frequency,
        seed ^ WARP_SEED_Z, WARP_OCTAVES, 2.0f, 0.5f, OctaveShape::PLAIN);

    x += amplitude * wx;
    y += amplitude * wy;
    z += amplitude * wz;
}

float Noise::warpedFbm(NoiseType type, float x, float y, uint32_t seed, int octaves, float lacunarity,
    float gain, float warpAmplitude, float warpFrequency) {

    warp2(x, y, seed, warpAmplitude, warpFrequency);
    return octaves2(type, x, y, seed, sanitizeOctaves(octaves),
        sanitize(lacunarity, 1.0f, 8.0f, 2.0f), sanitize(gain, 0.0f, 1.0f, 0.5f), OctaveShape::PLAIN);
}

float Noise::warpedFbm3(NoiseType type, float x, float y, float z, uint32_t seed, int octaves,
    float lacunarity, float gain, float warpAmplitude, float warpFrequency) {

    warp3(x, y, z, seed, warpAmplitude, warpFrequency);
    return octaves3(type, x, y, z, seed, sanitizeOctaves(octaves),
        sanitize(lacunarity, 1.0f, 8.0f, 2.0f), sanitize(gain, 0.0f, 1.0f, 0.5f), OctaveShape::PLAIN);
}
