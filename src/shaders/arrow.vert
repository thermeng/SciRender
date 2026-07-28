#version 460 core

layout(location = 0) in vec3 aLocalPos;
layout(location = 1) in vec3 aLocalNorm;
layout(location = 2) in float aPathIdx;
layout(location = 3) in float aPhaseOff;
layout(location = 4) in float aMag;

layout(std140, binding = 0) uniform StreamlineUBO {
    mat4  uMVP;
    mat4  uModel;
    vec4  uViewPos;           // xyz = viewPos
    vec4  uLightDir;          // xyz = lightDir
    vec4  uTime_Opacity;      // x = uTime, y = opacity
    vec4  uColor_UseColormap; // xyz = color, w = useColormap(0/1)
    vec4  uMagRange;          // x = magMin, y = maxMag, zw = pad
    vec4  uMaterial;          // x = ambient, y = diffuse, z = specular, w = specularPower
    vec4  uRibbon;            // x = ribbonWidth, y = taperFactor, zw = pad
    vec4  uArrowParams;       // x = arrowAnimSpeed, yzw = pad
};

uniform sampler1D uPathTex;

#define PATH_RES 256

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vTexcoord;
out float vMag;

mat3 buildFrame(vec3 tangent) {
    vec3 t = normalize(tangent);
    vec3 up = abs(t.y) < 0.9f ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 n = normalize(cross(t, up));
    vec3 b = cross(t, n);
    return mat3(n, b, t);
}

void main() {
    float t = fract(aPhaseOff + uTime_Opacity.x * uArrowParams.x * aMag);

    int base = int(aPathIdx) * PATH_RES;
    int idx = base + int(t * float(PATH_RES - 1));
    idx = clamp(idx, base, base + PATH_RES - 1);

    vec3 p0 = texelFetch(uPathTex, idx, 0).rgb;
    vec3 pNext = texelFetch(uPathTex, min(idx + 1, base + PATH_RES - 1), 0).rgb;
    vec3 pPrev = texelFetch(uPathTex, max(idx - 1, base), 0).rgb;
    vec3 tangent = normalize(pNext - pPrev + 1e-12f);

    mat3 frame = buildFrame(tangent);

    vWorldPos = p0 + frame * aLocalPos;
    vNormal = normalize(frame * aLocalNorm);
    vTexcoord = vec2(aPhaseOff, 0.0);
    vMag = aMag;
    gl_Position = uMVP * vec4(vWorldPos, 1.0);
}
