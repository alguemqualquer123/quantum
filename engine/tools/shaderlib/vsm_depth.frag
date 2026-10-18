#version 450

// Virtual Shadow Maps: page-based virtual shadow mapping
// Renders depth into virtual pages for efficient shadow mapping

in vec4 v_position;
in vec2 v_texcoord;

out vec4 frag_color;

uniform u_fs_vsmParams {
    mat4 viewProjection;
    vec4 params; // x = pageSize, y = pageScale, z = cascadeLevel, w = clipmapOffset
} vsmParams;

// Virtual page ID calculation
ivec2 getVirtualPageID(vec2 worldPos, float pageSize, float pageScale){
    vec2 pageCoord = worldPos / (pageSize * pageScale);
    return ivec2(floor(pageCoord));
}

// Physical page lookup
vec2 getPhysicalPageUV(ivec2 virtualID, vec2 physicalPageSize){
    // Simple hash function for virtual to physical mapping
    uint hash = uint(virtualID.x) * 1973u + uint(virtualID.y) * 9277u + 26699u;
    hash = hash ^ (hash >> 16u);
    hash *= 0x45d9f3bu;
    hash = hash ^ (hash >> 16u);

    float physicalX = float(hash % uint(physicalPageSize.x));
    float physicalY = float(hash / uint(physicalPageSize.x));

    return vec2(physicalX, physicalY) / physicalPageSize;
}

void main(){
    // Output depth only
    float depth = v_position.z / v_position.w;

    // Remap to [0,1] range for reversed-Z
    depth = depth * 0.5 + 0.5;

    frag_color = vec4(depth, depth, depth, 1.0);
}
