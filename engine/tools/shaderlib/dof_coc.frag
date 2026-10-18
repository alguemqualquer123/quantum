#version 450

// DOF Circle of Confusion: calculates blur amount based on depth
// Physical camera model: aperture, focal length, focus distance

in vec2 v_texcoord;
out vec4 frag_color;

uniform texture2D u_sceneColorTexture;
uniform sampler u_sceneColor_smp;

uniform texture2D u_depthTexture;
uniform sampler u_depth_smp;

uniform u_fs_dofCocParams {
    vec4 params0; // x = focusDistance, y = aperture, z = focalLength, w = maxBlurSize
    vec4 params1; // x = nearStart, y = nearEnd, z = farStart, w = farEnd
} dofCocParams;

// Linearize depth from packed format
float linearizeDepth(float depth){
    // Assuming reversed-Z (near=1, far=0) or standard projection
    float near = 0.1;
    float far = 1000.0;
    return (2.0 * near * far) / (far + near - depth * (far - near));
}

void main(){
    float focusDistance = dofCocParams.params0.x;
    float aperture = dofCocParams.params0.y;
    float focalLength = dofCocParams.params0.z;
    float maxBlurSize = dofCocParams.params0.w;

    float nearStart = dofCocParams.params1.x;
    float nearEnd = dofCocParams.params1.y;
    float farStart = dofCocParams.params1.z;
    float farEnd = dofCocParams.params1.w;

    // Sample depth
    float depth = texture(sampler2D(u_depthTexture, u_depth_smp), v_texcoord).r;
    float linearDepth = linearizeDepth(depth);

    // Calculate CoC (Circle of Confusion)
    // Physical formula: CoC = abs(A * f * (S - D)) / (S * (D - f))
    // Simplified for real-time: CoC = abs(D - focus) * scale
    float distFromFocus = abs(linearDepth - focusDistance);

    // Determine if in front of or behind focus
    float coc;
    if (linearDepth < focusDistance){
        // Near field blur (stronger than far field for artistic effect)
        coc = smoothstep(nearStart, nearEnd, linearDepth);
    } else {
        // Far field blur
        coc = smoothstep(farStart, farEnd, linearDepth);
    }

    // Scale by focus distance falloff
    coc *= distFromFocus / (focusDistance + 0.001);

    // Clamp to max blur size
    coc = min(coc, maxBlurSize);

    // Output: x = CoC value, y = depth for debugging
    frag_color = vec4(coc, linearDepth / 100.0, 0.0, 1.0);
}
