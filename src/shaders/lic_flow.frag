#version 460 core

layout(std140) uniform MeshUBO {
    mat4  uMVP;
    mat4  uModel;
    vec4  uViewPos_PS;
    vec4  uMeshColor_Wire;
    vec4  uSurfaceColor_Op;
    vec4  uPointClip;
    vec4  uLightDir;
    vec4  uLightFill;
    vec4  uLightBack1;
    vec4  uLightBack2;
    vec4  uLightHead;
    vec4  uKeyColor;
    vec4  uFillColor;
    vec4  uBackColor;
    vec4  uHeadColor;
    vec4  uScalars;
    vec4  uClipY;
    vec4  uClipEn;
    vec4  uInvert;
    vec4  uFilter;
    vec4  uMaterial;
    vec4  uIntensities;
    vec4  uPBR;
    vec4  uShadingMode;
};

in MeshVarying {
    vec3 vWorldPos;
    vec3 vNormal;
    float vScalar;
} mv;

uniform sampler3D uVectorTex;
uniform vec3 uBoxMin;
uniform vec3 uBoxMax;
uniform vec3 uUvwScale = vec3(1.0);
uniform vec3 uUvwOffset = vec3(0.0);
uniform float uLicMagThresh = 5e-5;
uniform vec2 uViewport; // pixels

out vec4 FragColor;

vec3 worldToUVW(vec3 pos, vec3 boxMin, vec3 invRange, vec3 range) {
    float diag = max(max(abs(range.x), abs(range.y)), abs(range.z));
    float eps = max(diag * 1e-7, 1e-30);
    vec3 uvw = (pos - boxMin) * invRange;
    if (abs(range.x) <= eps) uvw.x = 0.5;
    if (abs(range.y) <= eps) uvw.y = 0.5;
    if (abs(range.z) <= eps) uvw.z = 0.5;
    uvw = uvw * uUvwScale + uUvwOffset;
    return uvw;
}

void main() {
    bool hasScalars = uScalars.z > 0.5;
    float filterEps = 1e-5 * abs(uScalars.y - uScalars.x) + 1e-9;
    bool clipped = (uFilter.z > 0.5) && hasScalars &&
                   (mv.vScalar < uFilter.x - filterEps || mv.vScalar > uFilter.y + filterEps);
    bool crinkleMode = uShadingMode.y > 0.5;
    if (!clipped && uPointClip.w > 0.5 && !crinkleMode) {
        bool clipX = bool(uClipEn.x) && ((uInvert.x > 0.5) ? (mv.vWorldPos.x < uClipY.x) : (mv.vWorldPos.x > uClipY.x));
        bool clipY = bool(uClipEn.y) && ((uInvert.y > 0.5) ? (mv.vWorldPos.y < uClipY.y) : (mv.vWorldPos.y > uClipY.y));
        bool clipZ = bool(uClipEn.z) && ((uInvert.z > 0.5) ? (mv.vWorldPos.z < uClipY.z) : (mv.vWorldPos.z > uClipY.z));
        clipped = clipX || clipY || clipZ;
    }
    if (clipped) discard;

    bool isPoint = uPointClip.x > 0.5;
    if (isPoint) {
        vec2 pc = gl_PointCoord * 2.0 - 1.0;
        if (dot(pc, pc) > 1.0) discard;
    }

    vec3 range = uBoxMax - uBoxMin;
    float diag = length(range);
    if (!(diag > 0.0)) diag = 1.0;
    float diagForInv = max(max(abs(range.x), abs(range.y)), abs(range.z));
    float epsInv = max(diagForInv * 1e-7, 1e-30);
    vec3 invRange;
    invRange.x = (abs(range.x) > epsInv) ? 1.0 / range.x : 0.0;
    invRange.y = (abs(range.y) > epsInv) ? 1.0 / range.y : 0.0;
    invRange.z = (abs(range.z) > epsInv) ? 1.0 / range.z : 0.0;
    vec3 uvw = worldToUVW(mv.vWorldPos, uBoxMin, invRange, range);

    vec3 v = texture(uVectorTex, uvw).rgb;
    vec3 norm = normalize((length(mv.vNormal) > 1e-20) ? mv.vNormal : vec3(0.0, 1.0, 0.0));
    if (!gl_FrontFacing) norm = -norm;
    vec3 vTan = v - dot(v, norm) * norm;
    float mTan = length(vTan);
    if (!(mTan >= uLicMagThresh) || isinf(mTan) || isnan(mTan)) {
        FragColor = vec4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    vec3 dir = vTan / mTan;

    // Project a small world-space step to screen space for stable pixel-space LIC.
    float epsW = max(diag * 1e-4, 1e-6);
    vec4 p0 = uMVP * vec4(mv.vWorldPos, 1.0);
    vec4 p1 = uMVP * vec4(mv.vWorldPos + dir * epsW, 1.0);
    if (abs(p0.w) < 1e-8 || abs(p1.w) < 1e-8) {
        FragColor = vec4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    vec2 ndc0 = p0.xy / p0.w;
    vec2 ndc1 = p1.xy / p1.w;
    vec2 px = (ndc1 - ndc0) * 0.5 * uViewport;
    float lpx = length(px);
    if (!(lpx > 1e-6) || isnan(lpx) || isinf(lpx)) {
        FragColor = vec4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    px /= lpx;
    FragColor = vec4(px, 1.0, clamp(mTan / max(uLicMagThresh * 1000.0, 1e-30), 0.0, 1.0));
}
