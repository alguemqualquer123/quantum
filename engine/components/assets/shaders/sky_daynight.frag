#version 450

// Procedural day/night sky: atmosphere gradient, sun and moon discs whose phase comes from the
// real sun direction, stars, and two raymarched cloud decks.
//
// Every knob is a member of u_fs_customParams, filled by name from the Sky entity's shader
// uniforms. DayNightCycle.lua owns sunDir/moonDir/skyColour/sunDisc/moonDisc/atmosphere/stars,
// Clouds.lua owns cloudLayer/cloudShape/cloudWind/cloudTint, and camPos is written by whichever
// component runs last. The two never write the same member, so both can drive one Sky entity.
//
// Members stay float/vec4 only: the block is uploaded typed after its first member, so an int in
// here would silently corrupt every value after it.

out vec4 frag_color;

in vec3 uv;

uniform textureCube u_skyTexture;
uniform sampler u_sky_smp;

// This fork owns the dome's colour, so it deliberately does not declare u_fs_skyParams: the Sky
// entity's Color row keeps driving the water's sky reflection and the IBL ambient, which read
// SkyComponent::color directly (RenderSystem.cpp:7835), and multiplying that same near-black night
// ramp into an already-dark procedural dome would crush it a second time. cp.skyColour is the tint
// knob for the dome itself.

uniform u_fs_customParams {
    vec4 sunDir;       // xyz unit vector toward the sun, w = daylight 0..1
    vec4 moonDir;      // xyz unit vector toward the moon, w = moonlight 0..1
    vec4 skyColour;    // xyz zenith multiplier, w horizon multiplier
    vec4 sunDisc;      // xyz HDR disc colour (several times 1.0 so bloom catches it), w angular radius (rad)
    vec4 moonDisc;     // xyz disc colour, w angular radius (rad)
    vec4 atmosphere;   // x halo power, y halo strength, z sky brightness, w below-horizon fade depth
    vec4 stars;        // x brightness, y twinkle speed, z magnitude cut 0..1, w horizon extinction
    vec4 cloudLayer;   // x coverage 0..1, y altitude (m), z horizontal scale (m), w detail 0..1
    vec4 cloudShape;   // x density 0..1, y softness, z thickness (m), w cirrus amount 0..1
    vec4 cloudWind;    // xy wind velocity (m/s along world X,Z), z lit luminance, w shadow luminance
    vec4 cloudTint;    // xyz cloud colour, w night darkness 0..1
    vec4 camPos;       // xyz world camera position (cloud parallax), w sky-texture blend 0..1
    float time;        // written by the engine
} cp;

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float hash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

vec3 hash33(vec3 p) {
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.xxy + p.yxx) * p.zyx);
}

// Sine-free hashes throughout: a star field that changes between two drivers of the same GPU is
// worse than a slightly worse hash.
float vnoise2(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash12(i);
    float b = hash12(i + vec2(1.0, 0.0));
    float c = hash12(i + vec2(0.0, 1.0));
    float d = hash12(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

float vnoise3(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n00 = mix(mix(hash13(i), hash13(i + vec3(1.0, 0.0, 0.0)), f.x),
                    mix(hash13(i + vec3(0.0, 1.0, 0.0)), hash13(i + vec3(1.0, 1.0, 0.0)), f.x), f.y);
    float n10 = mix(mix(hash13(i + vec3(0.0, 0.0, 1.0)), hash13(i + vec3(1.0, 0.0, 1.0)), f.x),
                    mix(hash13(i + vec3(0.0, 1.0, 1.0)), hash13(i + vec3(1.0, 1.0, 1.0)), f.x), f.y);
    return mix(n00, n10, f.z);
}

float fbm5(vec2 p) {
    float sum = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 5; i++) {
        sum += amp * vnoise2(p);
        p = p * 2.03 + vec2(17.1, 9.7);
        amp *= 0.5;
    }
    return sum;
}

float fbm3(vec2 p) {
    float sum = 0.0;
    float amp = 0.55;
    for (int i = 0; i < 3; i++) {
        sum += amp * vnoise2(p);
        p = p * 2.07 + vec2(7.3, 21.9);
        amp *= 0.5;
    }
    return sum;
}

float fbm3d(vec3 p) {
    float sum = 0.0;
    float amp = 0.55;
    for (int i = 0; i < 3; i++) {
        sum += amp * vnoise3(p);
        p = p * 2.11 + vec3(3.7, 11.3, 7.9);
        amp *= 0.5;
    }
    return sum;
}

// One cloud deck: the plane at `alt` intersected with the view ray, then noise-scrolled by the wind.
//
// The internal shading is the mean of three taps of the same noise pushed along the light
// direction. That is the whole trick behind a cloud that looks like a volume: a patch is dark
// because the noise it would see looking back at the sun is thick, which is also why the terminator
// stays soft when the sun is low and the streaks lengthen on their own.
//
// Both noises are fBm sums, so their values crowd tightly around their own mean instead of filling
// [0,1]: every threshold here remaps through the observed range first. A raw fbm5 against
// `1 - coverage` makes coverage 0.99 saturate the whole sky, and a coverage 0.35 that already looks
// right is then only correct by accident.
vec3 deck(vec3 sky, vec3 dir, vec3 origin, vec3 lightDir, float lightStrength, vec3 lightColour,
          float alt, float coverage, float scale, vec2 wind, float detail, float softness,
          float thickness, float alpha, float twilight, float nightDark) {
    if (coverage <= 0.001 || dir.y <= 0.006) return sky;

    float t = (alt - origin.y) / dir.y;
    if (t <= 0.0) return sky;

    vec2 world = origin.xz + dir.xz * t;
    vec2 p = world / scale + wind * cp.time / scale;

    // Past a few tens of kilometres the plane is compressed to a couple of noise cells per pixel.
    // Blending toward the mean is a distance blur: it stops the aliasing without erasing the layer,
    // which is what an overcast horizon needs - fading the density instead leaves a bare dome with
    // a band of clouds only overhead.
    float near = 1.0 - smoothstep(12000.0, 30000.0, t);
    float shape = mix(0.484, fbm5(p), near);
    shape += (fbm5(p * 3.7 + vec2(5.3, 1.9)) - 0.5) * detail * near;
    shape = clamp((shape - 0.18) * 1.613, 0.0, 1.0);

    float edge = 1.0 - coverage;
    float density = smoothstep(edge, edge + softness, shape) * alpha;
    if (density <= 0.002) return sky;

    float lit = 0.0;
    vec2 offset = lightDir.xz / (abs(lightDir.y) + 0.22) * (thickness / scale) * 0.5;
    for (int i = 0; i < 3; i++) {
        lit += fbm3(p + offset * (float(i) + 0.5));
    }
    lit = clamp((lit * 0.3333 - 0.16) * 1.5625, 0.0, 1.0);

    vec3 body = lightColour * mix(clamp(cp.cloudWind.w, 0.0, 1.0), clamp(cp.cloudWind.z, 0.0, 2.0), lit);
    // The base of a cloud is the part you see at sunset, and it is the part the ground lights:
    // a warm underside with a cold top is what makes the deck read as being above you.
    body += lightColour * vec3(1.0, 0.42, 0.14) * twilight * (1.0 - lit) * 1.6;
    // Silver lining: thin edges toward the light scatter forward instead of reflecting.
    float towards = pow(max(dot(dir, lightDir), 0.0), 14.0);
    body += lightColour * towards * (1.0 - density) * 0.9 * lightStrength;

    // Clouds at night are a cold, nearly black sheet; the moon only recovers their silhouette.
    body *= mix(1.0, nightDark, clamp(1.0 - lightStrength, 0.0, 1.0));

    return mix(sky, body, clamp(density, 0.0, 1.0));
}

void main(){
    vec3 dir = normalize(uv);

    // An undriven fork still has to show a sky: the engine zeroes any member a script never wrote,
    // so a bare sunDir would render the dome black and read as a broken shader rather than as an
    // unconfigured one.
    vec3 sun = cp.sunDir.xyz;
    float daylight = cp.sunDir.w;
    if (dot(sun, sun) < 0.5) {
        sun = normalize(vec3(0.35, 0.62, 0.45));
        daylight = 1.0;
    } else {
        sun = normalize(sun);
    }

    vec3 moon = cp.moonDir.xyz;
    float moonlight = cp.moonDir.w;
    if (dot(moon, moon) < 0.5) moon = -sun; else moon = normalize(moon);

    float h = dir.y;
    float sunHeight = sun.y;

    float dayWeight = smoothstep(-0.16, 0.22, sunHeight);
    float twilight = exp(-abs(sunHeight) * 6.0);
    float aboveHorizon = smoothstep(-0.06, 0.06, sunHeight);

    vec3 zenithDay = vec3(0.055, 0.19, 0.55);
    vec3 horizonDay = vec3(0.55, 0.70, 0.88);
    vec3 zenithNight = vec3(0.008, 0.014, 0.038);
    vec3 horizonNight = vec3(0.035, 0.050, 0.100);
    vec3 zenithMoon = vec3(0.020, 0.035, 0.075);
    vec3 horizonMoon = vec3(0.060, 0.085, 0.140);

    vec3 zenith = mix(zenithNight, zenithDay, dayWeight) + zenithMoon * moonlight;
    vec3 horizon = mix(horizonNight, horizonDay, dayWeight) + horizonMoon * moonlight;
    zenith *= vec3(cp.skyColour.xyz);
    horizon *= cp.skyColour.w;

    float grad = pow(clamp(h, 0.0, 1.0), mix(0.55, 0.95, dayWeight));
    vec3 sky = mix(horizon, zenith, grad);

    // Rayleigh path length: the horizon is brighter all day and red only while the sun is low.
    // The glow sits on the sun's azimuth; the anti-solar side goes purple, which is the band a
    // single orange ramp loses.
    vec2 hd = normalize(dir.xz + vec2(1e-5));
    vec2 hs = normalize(sun.xz + vec2(1e-5));
    float azimuth = dot(hd, hs);
    float band = pow(clamp(1.0 - abs(h), 0.0, 1.0), 6.0);
    vec3 sunset = vec3(1.0, 0.36, 0.08) * band * (pow(max(azimuth, 0.0), 3.0) * 1.5 + 0.12)
                * twilight * daylight;
    sky += sunset;
    sky += vec3(0.30, 0.10, 0.42) * band * pow(max(-azimuth, 0.0), 2.0) * twilight * daylight * 0.7;

    float cosSun = dot(dir, sun);
    float haloPower = max(cp.atmosphere.x, 2.0);
    float halo = pow(max(cosSun, 0.0), haloPower) + pow(max(cosSun, 0.0), 1.6) * 0.06;
    sky += vec3(1.0, 0.72, 0.42) * halo * cp.atmosphere.y * (0.35 + twilight * 1.6)
         * aboveHorizon * daylight;

    float starNight = pow(clamp(1.0 - dayWeight, 0.0, 1.0), 1.5);
    if (cp.stars.x > 0.0 && starNight > 0.01) {
        // Quantising the direction is enough for point lights and avoids the seam an equirect
        // star map needs. The cells are square in the grid, so the field is slightly denser near
        // the cube-face centres; at 160 cells per radian nobody reads it as a pattern.
        vec3 g = dir * 160.0;
        vec3 cell = floor(g);
        vec3 local = fract(g) - 0.5;
        float seed = hash13(cell);
        if (seed > clamp(cp.stars.z, 0.0, 0.99)) {
            vec3 offset = (hash33(cell + vec3(11.3)) - 0.5) * 0.66;
            float d = length(local - offset);
            float mag = fract(seed * 13.71);
            float twinkle = 0.65 + 0.35 * sin(cp.time * cp.stars.y * (0.5 + mag * 2.5) + seed * 51.0);
            float star = smoothstep(0.06, 0.0, d) * twinkle * (0.25 + mag * mag * 2.0);
            float extinction = smoothstep(0.0, 0.10 + clamp(cp.stars.w, 0.0, 1.0), h);
            sky += vec3(0.85, 0.88, 1.0) * star * cp.stars.x * starNight * extinction;
        }
    }

    float sunR = max(cp.sunDisc.w, 0.0008);
    float angSun = acos(clamp(cosSun, -1.0, 1.0));
    sky += cp.sunDisc.xyz * smoothstep(sunR * 1.45, sunR * 0.85, angSun) * aboveHorizon * daylight;

    float moonR = max(cp.moonDisc.w, 0.0008);
    float cosMoon = dot(dir, moon);
    float angMoon = acos(clamp(cosMoon, -1.0, 1.0));
    float moonMask = smoothstep(moonR * 1.45, moonR * 0.85, angMoon);
    if (moonMask > 0.0) {
        // A distant sphere projects orthographically, so the surface normal behind a screen point
        // at offset (ox,oy) from the centre is (mu*ox + mv*oy)/radius + viewAxis*sqrt(1-r^2).
        // dot(that, sun) is then the terminator, and it curves the way the real one does - the
        // phase is never a number that can drift out of sync with the geometry.
        vec3 upHint = abs(moon.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        vec3 mu = normalize(cross(upHint, moon));
        vec3 mv = cross(moon, mu);
        vec2 o = vec2(dot(dir, mu), dot(dir, mv)) / moonR;
        float r2 = clamp(dot(o, o), 0.0, 1.0);
        vec3 normal = (mu * o.x + mv * o.y) + moon * sqrt(max(0.0, 1.0 - r2));
        float lambert = max(dot(normal, sun), 0.0);
        float mare = smoothstep(0.40, 0.66, fbm3d(normal * 3.1)) * 0.28;
        vec3 disc = cp.moonDisc.xyz * (1.0 - mare) * (0.10 + 0.90 * pow(lambert, 0.7));
        // The unlit half is not empty: earthshine is the part that makes a crescent look solid.
        disc += vec3(0.020, 0.026, 0.045) * (1.0 - smoothstep(0.0, 0.20, lambert));
        sky = mix(sky, disc, moonMask);
    }
    sky += cp.moonDisc.xyz * 0.05 * exp(-(angMoon * angMoon) / (moonR * moonR * 9.0))
         * max(moonlight, 0.0) * smoothstep(-0.02, 0.08, moon.y);

    // Below the horizon the dome is ground haze. A flat or low scene shows this region and a hard
    // line where the gradient stops reads as a bug rather than as distance.
    float under = smoothstep(0.0, -max(cp.atmosphere.w, 0.02), h);
    sky = mix(sky, horizon * 0.45 + sunset * 0.5, under);

    vec3 origin = cp.camPos.xyz;
    vec2 wind = cp.cloudWind.xy;
    float coverage = clamp(cp.cloudLayer.x, 0.0, 1.0);
    float scale = max(cp.cloudLayer.z, 20.0);
    float alt = max(cp.cloudLayer.y, 60.0);
    float softness = max(cp.cloudShape.y, 0.02);
    float densityParam = clamp(cp.cloudShape.x, 0.0, 1.0);
    float thickness = max(cp.cloudShape.z, 20.0);
    float detail = clamp(cp.cloudLayer.w, 0.0, 1.0);
    float nightDark = clamp(1.0 - cp.cloudTint.w, 0.0, 1.0);
    vec3 cloudColour = cp.cloudTint.xyz;

    // Sun first, then the moon: at civil twilight both are up and the deck catches the last warm
    // light from below while the moonlit side is already cold.
    sky = deck(sky, dir, origin, sun, daylight, cloudColour * vec3(1.0, 0.96, 0.9),
               alt, coverage, scale, wind, detail, softness, thickness, densityParam,
               twilight * (1.0 - dayWeight * 0.4), nightDark);
    if (moonlight > 0.02) {
        sky = deck(sky, dir, origin, moon, moonlight, cloudColour * vec3(0.62, 0.70, 0.95),
                   alt, coverage, scale, wind, detail, softness, thickness, densityParam * 0.85,
                   0.0, 1.0);
    }
    float cirrus = clamp(cp.cloudShape.w, 0.0, 1.0);
    if (cirrus > 0.01) {
        sky = deck(sky, dir, origin, sun, daylight, cloudColour * vec3(1.0, 0.98, 0.95),
                   alt * 2.4, coverage * cirrus * 0.55, scale * 2.6, wind * 2.3 + vec2(3.0, -2.0),
                   0.25, softness * 2.2, thickness * 0.35, 0.45, twilight, nightDark);
    }

    // The cubemap stays reachable: a project that already owns a sky texture can blend the
    // procedural dome into it instead of choosing one or the other.
    float toCube = clamp(cp.camPos.w, 0.0, 1.0);
    if (toCube > 0.0) {
        sky = mix(sky, texture(samplerCube(u_skyTexture, u_sky_smp), dir).rgb, toCube);
    }

    frag_color = vec4(sky * max(cp.atmosphere.z, 0.0), 1.0);
}
