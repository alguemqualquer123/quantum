#version 450

// DOF Blur: Gaussian/Bokeh blur weighted by CoC
// Two-pass separable blur (horizontal + vertical)

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_sceneColorTexture;
uniform sampler u_sceneColor_smp;

uniform texture2D u_depthTexture; // CoC texture
uniform sampler u_depth_smp;

uniform u_fs_dofBlurParams {
    vec4 params; // xy = texelSize, z = blurRadius, w = bokehShape (0 = gaussian, 1 = circle, 2 = hex)
} dofBlurParams;

// Gaussian weight function
float gaussianWeight(float x, float sigma){
    return exp(-0.5 * (x * x) / (sigma * sigma));
}

// Bokeh circle shape
float bokehCircle(vec2 offset, float radius){
    float dist = length(offset);
    return 1.0 - smoothstep(radius - 0.5, radius + 0.5, dist);
}

void main(){
    vec2 texelSize = dofBlurParams.params.xy;
    float blurRadius = dofBlurParams.params.z;
    float bokehShape = dofBlurParams.params.w;

    // Sample center CoC
    float centerCoC = texture(sampler2D(u_depthTexture, u_depth_smp), v_texcoord).r;
    vec3 centerColor = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), v_texcoord).rgb;

    vec3 blurredColor = vec3(0.0);
    float totalWeight = 0.0;

    // Sample in a disk pattern
    for (float x = -blurRadius; x <= blurRadius; x += 1.0){
        for (float y = -blurRadius; y <= blurRadius; y += 1.0){
            vec2 offset = vec2(x, y) * texelSize;
            vec2 sampleUV = v_texcoord + offset;

            // Sample color and CoC
            vec3 sampleColor = texture(sampler2D(u_sceneColorTexture, u_sceneColor_smp), sampleUV).rgb;
            float sampleCoC = texture(sampler2D(u_depthTexture, u_depth_smp), sampleUV).r;

            // Weight by CoC (far CoC affects near less, near CoC affects far less)
            float cocDiff = abs(sampleCoC - centerCoC);
            float weight = 1.0;

            if (bokehShape < 0.5){
                // Gaussian blur
                weight = gaussianWeight(length(vec2(x, y)), blurRadius * 0.5);
            } else if (bokehShape < 1.5){
                // Circle bokeh
                weight = bokehCircle(vec2(x, y), blurRadius);
            } else {
                // Hexagonal bokeh (approximated)
                vec2 hex = vec2(x * 0.866, y + x * 0.5);
                weight = bokehCircle(hex, blurRadius * 0.8);
            }

            // Reduce weight for samples with very different CoC (prevent bleeding)
            weight *= 1.0 - smoothstep(0.0, blurRadius * 0.3, cocDiff);

            blurredColor += sampleColor * weight;
            totalWeight += weight;
        }
    }

    blurredColor /= max(totalWeight, 0.001);

    // Blend based on CoC
    float blendFactor = smoothstep(0.0, 1.0, centerCoC);
    vec3 result = mix(centerColor, blurredColor, blendFactor);

    frag_color = vec4(result, 1.0);
}
