#version 330 core
layout(location = 0) in vec3 aPos;     // unit-arrow vertex (local space, arrow along +Y)
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 iOrigin;  // per-instance glyph base position
layout(location = 3) in vec3 iDir;     // per-instance raw direction

uniform mat4 uMVP;
uniform float uScale;
uniform float uMagMin;
uniform float uMagMax;

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

    // uniform-length arrows — scale the whole arrow by uScale (not magnitude)
    vec3 local = R * (aPos * uScale);
    vec3 world = iOrigin + local;

    vWorldPos = world;
    vNormal = R * aNormal;
    // normalize magnitude to [0,1] across the field for LUT lookup
    float span = max(uMagMax - uMagMin, 1e-6);
    vMag = clamp((mag - uMagMin) / span, 0.0, 1.0);
    gl_Position = uMVP * vec4(world, 1.0);
}
