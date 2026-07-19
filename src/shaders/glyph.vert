#version 330 core

// =====================================================================
// ATTRIBUTES
// =====================================================================
layout(location = 0) in vec3 aPos;     // Unit-arrow vertex geometry (local space, along +Y)
layout(location = 1) in vec3 aNormal;  // Local space vertex normals
layout(location = 2) in vec3 iOrigin;  // Per-instance glyph base position (world coordinate)
layout(location = 3) in vec3 iDir;     // Per-instance raw scientific vector direction

// =====================================================================
// UNIFORMS
// =====================================================================
uniform mat4 uMVP;          // Complete Model-View-Projection matrix from Camera
uniform float uScale;       // User UI multiplier scale factor
uniform float uMagMin;      // Minimum field scalar bounds
uniform float uMagMax;      // Maximum field scalar bounds
uniform float uScaleByMag;  // 1.0 = scale length by magnitude, 0.0 = uniform length
uniform float uMeshExtent;  // Overall Bounding volume extent for automatic scaling
uniform int uMagTransform;  // Scaling mode: 0 = linear, 1 = sqrt, 2 = log

// =====================================================================
// INTERFACE BLOCK / OUTPUTS
// =====================================================================
out vec3 vNormal;
out vec3 vWorldPos;
out float vMag;             // Normalized value [0,1] for Fragment Shader LUT sampling

// =====================================================================
// FUNCTIONS
// =====================================================================

// Applies non-linear transformations so low-magnitude elements remain visible
float txMag(float m) {
    if (uMagTransform == 1) return sqrt(max(m, 0.0));
    if (uMagTransform == 2) return log(1.0 + max(m, 0.0));
    return m;
}

// Rotates local +Y onto dir. The previous Rodrigues form
// (R = I + s*V + (1-c)*V^2, V = skew(normalize(up x dir))) is only correct when
// dir is perpendicular to up; for any other dir it produces wrong orientations
// (verified errors up to ~0.8 on a real swirl field). This explicit right-handed
// orthonormal basis is exact for all dir, including dir parallel to +/-Y.
mat3 alignToDir(vec3 dir) {
    vec3 f = normalize(dir);
    // pick a reference not parallel to f to span the perpendicular plane
    vec3 ref = (abs(f.x) < 0.99) ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 0.0, 1.0);
    vec3 right = normalize(cross(ref, f));
    vec3 up2 = cross(right, f); // right-handed: up2 = right x f  (det = +1)
    return mat3(right, f, up2); // columns map local X->right, Y->f, Z->up2
}

// =====================================================================
// ENTRY POINT
// =====================================================================
void main() {
    float mag = length(iDir);
    vec3 dir = mag > 1e-6 ? iDir / mag : vec3(0.0, 1.0, 0.0);
    
    // Extract correct rotation alignment matrix 
    mat3 R = alignToDir(dir);

    // Normalize transformed magnitude to [0,1] dynamic span for length bounds and color mapping
    float span = max(txMag(uMagMax) - txMag(uMagMin), 1e-6);
    vMag = clamp((txMag(mag) - txMag(uMagMin)) / span, 0.0, 1.0);

    // Compute absolute layout scale independent of arbitrary scientific dataset dimensions
    float refLen = max(uMeshExtent, 1e-6) * 0.05 * uScale;

    // Apply scaling logic. Clamp minimum limits to prevent zero-length rendering errors.
    float lengthScale = refLen * (1.0 + uScaleByMag * (vMag * 1.25 - 0.25));
    
    // Transform template local mesh vertices into world space orientation positions
    vec3 local = R * (aPos * lengthScale);
    vec3 world = iOrigin + local;

    // Direct interface outputs down to fragment shaders
    vWorldPos = world;
    vNormal = R * aNormal;
    
    // Final hardware clip coordinates calculation
    gl_Position = uMVP * vec4(world, 1.0);
}