#version 460 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 iOrigin;
layout(location = 3) in vec3 iDir;

layout(std140) uniform GlyphUBO {
    mat4  uMVP;
    vec4  uScale_MagMin_MagMax_ScaleByMag; // x=scale, y=magMin, z=magMax, w=scaleByMag
    vec4  uMeshExtent_MagTransform_ViewPosY_ColorR; // x=meshExtent, y=magTransform, z=viewPos.y, w=colorR
    vec4  uLightDir_ColorGB; // xyz=lightDir, w=colorG
    vec4  uColorB_ColorMode; // x=colorB, y=colorMode(0-4), zw=pad
    vec4  uCompMin; // xyz=compMin X,Y,Z, w=pad
    vec4  uCompMax; // xyz=compMax X,Y,Z, w=pad
    vec4  uPBR;              // x = matRoughness, y = matMetallic, z = pad, w = pad
};

out vec3 vNormal;
out vec3 vWorldPos;
out float vMag;
out float vColorScalar;

float txMag(float m) {
    int mode = int(uMeshExtent_MagTransform_ViewPosY_ColorR.y);
    if (mode == 1) return sqrt(max(m, 0.0));
    if (mode == 2) return log(1.0 + max(m, 0.0));
    return m;
}

mat3 alignToDir(vec3 dir) {
    vec3 f = normalize(dir);
    vec3 ref = (abs(f.x) < 0.99) ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 0.0, 1.0);
    vec3 right = normalize(cross(ref, f));
    vec3 up2 = cross(right, f);
    return mat3(right, f, up2);
}

void main() {
    float mag = length(iDir);
    vec3 dir = mag > 1e-6 ? iDir / mag : vec3(0.0, 1.0, 0.0);
    mat3 R = alignToDir(dir);

    float magMin = uScale_MagMin_MagMax_ScaleByMag.y;
    float magMax = uScale_MagMin_MagMax_ScaleByMag.z;
    float span = max(txMag(magMax) - txMag(magMin), 1e-6);
    vMag = clamp((txMag(mag) - txMag(magMin)) / span, 0.0, 1.0);

    // Per-component normalization
    vec3 cMin = uCompMin.xyz;
    vec3 cMax = uCompMax.xyz;
    vec3 cSpan = max(cMax - cMin, vec3(1e-6));
    vec3 vComp = clamp((iDir - cMin) / cSpan, 0.0, 1.0);

    int colorMode = int(uColorB_ColorMode.y);
    if (colorMode == 1)      vColorScalar = vMag;
    else if (colorMode == 2) vColorScalar = vComp.x;
    else if (colorMode == 3) vColorScalar = vComp.y;
    else if (colorMode == 4) vColorScalar = vComp.z;
    else                     vColorScalar = 0.0;

    float scale = uScale_MagMin_MagMax_ScaleByMag.x;
    float scaleByMag = uScale_MagMin_MagMax_ScaleByMag.w;
    float meshExtent = uMeshExtent_MagTransform_ViewPosY_ColorR.x;
    float refLen = max(meshExtent, 1e-6) * 0.05 * scale;
    float lengthScale = refLen * (1.0 + scaleByMag * (vMag * 1.25 - 0.25));

    vec3 local = R * (aPos * lengthScale);
    vec3 world = iOrigin + local;

    vWorldPos = world;
    vNormal = R * aNormal;
    gl_Position = uMVP * vec4(world, 1.0);
}
