// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "Wind.h"

#include "../Scene.h"
#include "Angle.h"
#include "Noise.h"

#include <algorithm>
#include <cmath>

using namespace quantum;

namespace {

    // The side of one gust, in metres. Gusts and the calm gaps between them are a hundred metres
    // apart on a pond and a kilometre apart over an open sea; one cell of 150 m keeps a squall
    // crossing a scene-sized body of water in seconds at a walking pace, and a whole template
    // island inside a couple of minutes.
    const float WIND_GUST_CELL = 150.0f;

    // Two octaves: the big sweep plus the smaller eddies riding on it. More detail per sample would
    // read as hiss on the water, which turns the wave train to noise rather than weather.
    const int WIND_OCTAVES = 2;
    const float WIND_LACUNARITY = 2.0f;
    const float WIND_GAIN = 0.5f;

    // One field for the whole engine. A seed a project could change would make two scenes disagree
    // about the same authored numbers, and the wind is not content.
    const uint32_t WIND_SPEED_SEED = 0x494E4400u;
    const uint32_t WIND_VEER_SEED = 0x56454500u;

    // How far a gust swings the heading, in radians: about twenty degrees, the most a squall line
    // does to a lake before the next one straightens it out again.
    const float WIND_VEER_MAX = 0.35f;

    // The gust envelope: 1 at steady air, and down to 0 / up to 2 as the field runs to its extremes.
    // windChange is the third axis, so it buys a shifty day without touching how big the gusts are.
    float gustFactor(const Scene& scene, const Vector2& drifted, float seconds){
        const float renew = std::max(0.0f, scene.getWindChange()) * seconds;
        const float n = Noise::fbm3(NoiseType::PERLIN, drifted.x, drifted.y, renew, WIND_SPEED_SEED,
            WIND_OCTAVES, WIND_LACUNARITY, WIND_GAIN);
        return std::max(0.0f, 1.0f + scene.getWindGustiness() * n);
    }

    // The heading, veered by its own field: gusts do not arrive along the mean wind, and a wave
    // train that never wanders is the tell that the weather is a constant with a new name.
    float gustDirection(const Scene& scene, const Vector2& drifted, float seconds){
        const float base = Angle::defaultToRad(scene.getWindDirection());
        const float renew = std::max(0.0f, scene.getWindChange()) * seconds;
        const float n = Noise::fbm3(NoiseType::SIMPLEX, drifted.x, drifted.y, renew, WIND_VEER_SEED,
            WIND_OCTAVES, WIND_LACUNARITY, WIND_GAIN);
        return base + WIND_VEER_MAX * scene.getWindGustiness() * n;
    }

    // The gust pattern travels downwind, so one gust crosses the water instead of every point
    // blinking on and off together. Metres, at the mean speed: a gust lives long enough to arrive.
    // Sampling upstream is what makes it move - F(point - wind * t) reads the gust that left
    // `point`'s place at t = 0, so the same value reaches a point further downwind later on.
    Vector2 driftOf(const Vector2& unit, float seconds, float mean){
        return unit * (seconds * mean);
    }

}

Vector2 Wind::sample(const Scene& scene, const Vector2& point, float seconds){
    if (!scene.isWindEnabled())
        return Vector2(0.0f, 0.0f);

    const float mean = scene.getWindSpeed();
    if (mean <= 0.0f)
        return Vector2(0.0f, 0.0f);

    // +X rotated around Y, the way a Water's waveDirection arrives in the shader.
    const float authored = Angle::defaultToRad(scene.getWindDirection());
    const Vector2 meanUnit(std::cos(authored), -std::sin(authored));

    const Vector2 drifted = (point - driftOf(meanUnit, seconds, mean)) * (1.0f / WIND_GUST_CELL);

    const float angle = gustDirection(scene, drifted, seconds);
    const Vector2 unit(std::cos(angle), -std::sin(angle));

    return unit * (mean * gustFactor(scene, drifted, seconds));
}

float Wind::speed(const Scene& scene, const Vector2& point, float seconds){
    return sample(scene, point, seconds).length();
}

float Wind::direction(const Scene& scene){
    return Angle::defaultToRad(scene.getWindDirection());
}
