#version 450

// Eye adaptation, run into a 1x1 target once per frame: it reads the whole HDR scene with a
// jittered grid of taps, averages luminance in log space (a geometric mean, which is what keeps a
// few street lamps from dragging the exposure of an entire night street), and adapts the result
// toward the previous frame's value held in the history texel. Brightening is faster than
// darkening, like a real pupil, and the adaptation happens in log space so a half-stop and a
// five-stop change feel equally quick.
//
// The average is weighted, not plain. An unweighted geometric mean keys the *whole* frame to 18%
// grey, and a high-dynamic-range frame - a sunlit facade against a black letterbox, a wet street
// against a night sky - then needs several stops to reach that key, which pushes every highlight
// past the tone map's ceiling. Measured on the forest fixture: the plain mean chose 4x and clipped
// 43% of the pixels. So the mean is taken twice: once coarse, then again with the Beumer et al.
// piecewise-exponential weights, which suppress the bins that sit more than a couple of stops
// below or above it. What is left is the mid-tone mass, which is what a photographer meters on.
//
// Nothing is read back to the CPU: the tone map samples this texel directly, which is why the
// pass can afford to run every frame without a pipeline stall.

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_sceneColorTexture;
uniform sampler u_sceneColor_smp;
uniform texture2D u_exposureTexture;
uniform sampler u_exposure_smp;

uniform u_fs_exposureParams {
    vec4 params; // x = min exposure multiplier, y = max exposure multiplier, z = adaptation speed, w = EV compensation
    vec4 adapt;  // x = time delta, y = exposure to use when there is no history, zw unused
} exposure;

// ITU-R BT.709 luminance
float getLuminance(vec3 color){
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

// 16x16 taps, walked twice (coarse mean, then weighted mean) for the same total fetch count the
// single 24x24 pass used. Offset by half a cell per row so the grid does not alias against a
// regular scene (a fence, a row of windows). Every tap is already bilinear.
const int TAPS = 16;
// Black borders are not a subject: a zero-valued tap has no luminance to average and would anchor
// the meter to the floor of the scale. Above this, the weights handle the tail.
const float LUM_FLOOR = 1e-4;
const float LUM_CEIL = 32.0;
// The knees and the decay of the weight curve, all in stops around the coarse mean. Shadows are
// cut harder than highlights: a dark background is far more common than a blown one, and it is the
// side that over-brightens the image.
const float SHADOW_STOPS = 2.0;
const float HIGHLIGHT_STOPS = 1.25;
const float SHADOW_DECAY = 1.35;
const float HIGHLIGHT_DECAY = 1.1;

float luminanceAt(vec2 uv){
    return getLuminance(texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), uv).rgb);
}

// Tap uv for cell (i, j) of the jittered grid.
vec2 tapUV(float i, float j){
    float fx = (j + 0.5) / float(TAPS);
    float fy = (i + 0.5) / float(TAPS);
    return vec2(fx, fy) + vec2(fract(fx * 13.0) - 0.5, fract(fy * 17.0) - 0.5) / float(TAPS);
}

// Beumer et al.: the weight of a sample falls off exponentially with its distance, in stops, from
// the coarse mean. A bin inside the knees keeps full weight.
float meterWeight(float stopsFromMean){
    float under = stopsFromMean < -SHADOW_STOPS ? (stopsFromMean + SHADOW_STOPS) * SHADOW_DECAY : 0.0;
    float over = stopsFromMean > HIGHLIGHT_STOPS ? (stopsFromMean - HIGHLIGHT_STOPS) * HIGHLIGHT_DECAY : 0.0;
    return exp2(min(under, over));
}

void main(){
    // Stage 1: where the light actually sits, in log space, so a single lamp cannot set the average
    float sum = 0.0;
    float count = 0.0;
    for (int i = 0; i < TAPS; i++){
        for (int j = 0; j < TAPS; j++){
            float lum = luminanceAt(tapUV(float(i), float(j)));
            if (lum > LUM_FLOOR && lum < LUM_CEIL){
                sum += log2(lum);
                count += 1.0;
            }
        }
    }
    // -2.47 stops is 0.18: the key a bare geometric mean would aim at even when the frame is empty
    float coarse = count > 0.0 ? sum / count : -2.47;

    // Stage 2: the same taps again, this time weighted, so the key follows the mid-tone mass
    float weighted = 0.0;
    float weightSum = 0.0;
    for (int i = 0; i < TAPS; i++){
        for (int j = 0; j < TAPS; j++){
            float lum = luminanceAt(tapUV(float(i), float(j)));
            if (lum > LUM_FLOOR && lum < LUM_CEIL){
                float l = log2(lum);
                float w = meterWeight(l - coarse);
                weighted += w * l;
                weightSum += w;
            }
        }
    }
    float metered = weightSum > 0.0 ? weighted / weightSum : coarse;
    float geoMean = exp2(metered);

    // 18% middle grey is the key; the compensation is an EV offset on top of it
    float key = 0.18 * exp2(exposure.params.w);
    float target = clamp(key / (geoMean + 1e-4), exposure.params.x, exposure.params.y);

    // The history texel is zero (or undefined) on the first frame, so a non-positive read is the
    // signal to start from the scene's own manual exposure instead of adapting from black.
    float previous = exposure.adapt.y;
    float history = texture(sampler2D(u_exposureTexture, u_exposure_smp), vec2(0.5)).x;
    if (history > 0.0)
        previous = history;

    float speed = exposure.params.z;
    // the pupil constricts about twice as fast as it dilates
    float rate = target > previous ? min(speed * 2.0 * exposure.adapt.x, 1.0)
                                   : min(speed * exposure.adapt.x, 1.0);
    float adapted = exp2(mix(log2(max(previous, 1e-5)), log2(target), rate));

    frag_color = vec4(adapted, geoMean, target, 1.0);
}
