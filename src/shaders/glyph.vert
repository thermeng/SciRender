#version 330 core
layout(location = 0) in vec3 aPos;     // unit-arrow vertex (local space, arrow along +Y)
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 iOrigin;  // per-instance glyph base position
layout(location = 3) in vec3 iDir;     // per-instance raw direction

uniform mat4 uMVP;
uniform float uScale;
uniform float uMagMin;
uniform float uMagMax;
uniform float uScaleByMag; // 1.0 = scale arrow length by magnitude, 0.0 = uniform length

out vec3 vNormal;
out vec3 vWorldPos;
out float vMag;   // per-instance magnitude for LUT coloring

// rotate +Y onto dir (Rodrigues about up x dir), uniform-length arrows
mat3 alignToDir(vec3 dir) {
    vec3 up = vec3(0.0, 1.0, 0.0);
    float c = dot(up, dir);
    if (c > 0.9999) return mat3(1.0);
    if (c < -0.9999) return mat3(-1.0, 0.0, 0.0,  0.0, 1.0, 0.0,  0.0, 0.0, -1.0);
    vec3 v = cross(up, dir);
    float s = sqrt(1.0 - c * c);
    mat3 V = mat3(0.0, -v.z, v.y,  v.z, 0.0, -v.x,  -v.y, v.x, 0.0);
    return mat3(1.0) + s * V + (1.0 - c) * (V * V);
}

void main() {
    float mag = length(iDir);
    vec3 dir = mag > 1e-6 ? iDir / mag : vec3(0.0, 1.0, 0.0);
    mat3 R = alignToDir(dir);

    // Normalize magnitude to [0,1] for length scaling and LUT lookup
    float span = max(uMagMax - uMagMin, 1e-6);
    vMag = clamp((mag - uMagMin) / span, 0.0, 1.0);

    // When uScaleByMag is on, longer arrows for stronger vectors (kept in a
    // sane [0.25, 1.5] range so tiny vectors stay visible). Otherwise the
    // arrow length is uniform and only the color encodes magnitude.
    float lengthScale = uScale * (1.0 + uScaleByMag * (vMag * 1.25 - 0.25));
    vec3 local = R * (aPos * lengthScale);
    vec3 world = iOrigin + local;

    vWorldPos = world;
    vNormal = R * aNormal;
    gl_Position = uMVP * vec4(world, 1.0);
}
