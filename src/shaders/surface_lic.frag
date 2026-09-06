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

uniform sampler1D uColormapLUT;
uniform float uNumBands = 0.0;

uniform sampler3D uVectorTex;
uniform sampler3D uNoiseTex; // 3D white noise — isotropic, no planar projection.
uniform vec3 uBoxMin;
uniform vec3 uBoxMax;
uniform int uLicSteps;          // half-kernel length; UI [4..128], GPU tolerates [0..128] (0 = single-sample fallback)
uniform float uLicStepSize;     // world units (= clamp(RenderSettings licStepSize, 0.001..2.0) * diag), clamped to [1e-6, diag*2]
uniform float uLicNoiseFreq;    // noise UV scale, expected [0.5..64]
uniform int uLicEnhanced;       // 0=single-pass, 1=enhanced 2-pass (sharpens coherence)
uniform int uLicIntegrator;     // 0=Euler, 1=Midpoint, 2=RK4
uniform bool uLicOnly;          // true = skip PBR, output unshaded LIC color
uniform vec3 uUvwScale = vec3(1.0);
uniform vec3 uUvwOffset = vec3(0.0);

out vec4 FragColor;

vec3 worldToUVW(vec3 pos, vec3 boxMin, vec3 invRange, vec3 range) {
    float diag = max(max(abs(range.x), abs(range.y)), abs(range.z));
    float eps = max(diag * 1e-7, 1e-7);
    vec3 uvw = (pos - boxMin) * invRange;
    if (abs(range.x) <= eps) uvw.x = 0.5;
    if (abs(range.y) <= eps) uvw.y = 0.5;
    if (abs(range.z) <= eps) uvw.z = 0.5;
    uvw = uvw * uUvwScale + uUvwOffset;
    return uvw;
}

vec3  uMatAmbient()    { return vec3(uMaterial.x); }
float uMatDiffuse()    { return uMaterial.y; }
float uMatSpecular()   { return uMaterial.z; }
float uKeyIntensity()   { return uIntensities.x; }
float uFillIntensity()  { return uIntensities.y; }
float uBackIntensity()  { return uIntensities.z; }
float uHeadIntensity()  { return uIntensities.w; }
float uMatRoughness() { return uPBR.x; }
float uMatMetallic()  { return uPBR.y; }

float licNoiseSample3D(vec3 uvw) {
    return texture(uNoiseTex, uvw * uLicNoiseFreq).r;
}

float licNoiseSample3D_scaled(vec3 uvw, float freqScale) {
    return texture(uNoiseTex, uvw * uLicNoiseFreq * freqScale).r;
}

// Helper: sample normalized vector direction at a world position.
// Returns sign * v/|v| if |v| > magThresh else vec3(0) signalling termination (matches StreamlineSet RK4).
vec3 licSampleDir(vec3 pos, vec3 boxMin, vec3 invRange, vec3 range, vec3 norm, float sign) {
    const float magThresh = 5e-5;
    vec3 uvw = worldToUVW(pos, boxMin, invRange, range);
    vec3 v = texture(uVectorTex, uvw).rgb;
    vec3 vTan = v - dot(v, norm) * norm;
    float mTan = length(vTan);
    if (mTan < magThresh) return vec3(0.0);
    return sign * (vTan / mTan);
}

// --- Integrators ---

// Euler: 1 vector sample per step per direction.
float licEuler(vec3 startPos, vec3 startUVW, vec3 boxMin, vec3 boxMax,
               vec3 invRange, vec3 range, vec3 norm, vec3 absNorm,
               int steps, float worldStepSize, float freqScale, float sign) {
    const float h = worldStepSize;
    float acc = licNoiseSample3D_scaled(startUVW, freqScale);
    float w = 1.0;
    vec3 pos = startPos;
    for (int i = 0; i < steps; ++i) {
        vec3 k1 = licSampleDir(pos, boxMin, invRange, range, norm, sign);
        if (dot(k1,k1) < 1e-12) break;
        pos = pos + h * k1;
        vec3 uvw = worldToUVW(pos, boxMin, invRange, range);
        acc += licNoiseSample3D_scaled(uvw, freqScale);
        w += 1.0;
    }
    return acc / max(w, 1.0);
}

// Midpoint (2nd order Runge-Kutta): 2 vector samples per step per direction.
float licMidpoint(vec3 startPos, vec3 startUVW, vec3 boxMin, vec3 boxMax,
                   vec3 invRange, vec3 range, vec3 norm, vec3 absNorm,
                   int steps, float worldStepSize, float freqScale, float sign) {
    const float h = worldStepSize;
    float acc = licNoiseSample3D_scaled(startUVW, freqScale);
    float w = 1.0;
    vec3 pos = startPos;
    for (int i = 0; i < steps; ++i) {
        vec3 k1 = licSampleDir(pos, boxMin, invRange, range, norm, sign);
        if (dot(k1,k1) < 1e-12) break;
        vec3 midPos = pos + 0.5 * h * k1;
        vec3 k2 = licSampleDir(midPos, boxMin, invRange, range, norm, sign);
        if (dot(k2,k2) < 1e-12) break;
        pos = pos + h * k2;
        vec3 uvw = worldToUVW(pos, boxMin, invRange, range);
        acc += licNoiseSample3D_scaled(uvw, freqScale);
        w += 1.0;
    }
    return acc / max(w, 1.0);
}

// RK4: 4 vector samples per step per direction.
float licRK4(vec3 startPos, vec3 startUVW, vec3 boxMin, vec3 boxMax,
              vec3 invRange, vec3 range, vec3 norm, vec3 absNorm,
              int steps, float worldStepSize, float freqScale, float sign) {
    const float h = worldStepSize;
    float acc = licNoiseSample3D_scaled(startUVW, freqScale);
    float w = 1.0;
    vec3 pos = startPos;
    for (int i = 0; i < steps; ++i) {
        vec3 k1 = licSampleDir(pos, boxMin, invRange, range, norm, sign);
        if (dot(k1,k1) < 1e-12) break;
        vec3 k2 = licSampleDir(pos + 0.5 * h * k1, boxMin, invRange, range, norm, sign);
        if (dot(k2,k2) < 1e-12) break;
        vec3 k3 = licSampleDir(pos + 0.5 * h * k2, boxMin, invRange, range, norm, sign);
        if (dot(k3,k3) < 1e-12) break;
        vec3 k4 = licSampleDir(pos + h * k3, boxMin, invRange, range, norm, sign);
        if (dot(k4,k4) < 1e-12) break;
        pos = pos + (h / 6.0) * (k1 + 2.0 * k2 + 2.0 * k3 + k4);
        vec3 uvw = worldToUVW(pos, boxMin, invRange, range);
        acc += licNoiseSample3D_scaled(uvw, freqScale);
        w += 1.0;
    }
    return acc / max(w, 1.0);
}

// Single-pass dispatcher: choose integrator, run forward (+1) and backward (-1).
float computeLicGray(vec3 startPos, vec3 startUVW, vec3 dir, vec3 boxMin, vec3 boxMax,
                     vec3 invRange, vec3 range, vec3 norm, vec3 absNorm,
                     int steps, float worldStepSize, float freqScale) {
    float fwd = 0.0, bwd = 0.0;
    if (uLicIntegrator == 0) {
        fwd = licEuler(startPos, startUVW, boxMin, boxMax, invRange, range, norm, absNorm, steps, worldStepSize, freqScale, 1.0);
        bwd = licEuler(startPos, startUVW, boxMin, boxMax, invRange, range, norm, absNorm, steps, worldStepSize, freqScale, -1.0);
    } else if (uLicIntegrator == 1) {
        fwd = licMidpoint(startPos, startUVW, boxMin, boxMax, invRange, range, norm, absNorm, steps, worldStepSize, freqScale, 1.0);
        bwd = licMidpoint(startPos, startUVW, boxMin, boxMax, invRange, range, norm, absNorm, steps, worldStepSize, freqScale, -1.0);
    } else {
        fwd = licRK4(startPos, startUVW, boxMin, boxMax, invRange, range, norm, absNorm, steps, worldStepSize, freqScale, 1.0);
        bwd = licRK4(startPos, startUVW, boxMin, boxMax, invRange, range, norm, absNorm, steps, worldStepSize, freqScale, -1.0);
    }
    return (fwd + bwd) / 2.0;
}

// Dual-frequency enhanced dispatcher: same integrator path, two noise frequencies.
vec2 computeLicGrayDual(vec3 startPos, vec3 startUVW, vec3 dir, vec3 boxMin, vec3 boxMax,
                         vec3 invRange, vec3 range, vec3 norm, vec3 absNorm,
                         int steps, float worldStepSize) {
    float fwd1 = 0.0, fwd2 = 0.0, bwd1 = 0.0, bwd2 = 0.0;
    if (uLicIntegrator == 0) {
        fwd1 = licEuler(startPos, startUVW, boxMin, boxMax, invRange, range, norm, absNorm, steps, worldStepSize, 1.0, 1.0);
        fwd2 = licEuler(startPos, startUVW, boxMin, boxMax, invRange, range, norm, absNorm, steps, worldStepSize, 1.35, 1.0);
        bwd1 = licEuler(startPos, startUVW, boxMin, boxMax, invRange, range, norm, absNorm, steps, worldStepSize, 1.0, -1.0);
        bwd2 = licEuler(startPos, startUVW, boxMin, boxMax, invRange, range, norm, absNorm, steps, worldStepSize, 1.35, -1.0);
    } else if (uLicIntegrator == 1) {
        fwd1 = licMidpoint(startPos, startUVW, boxMin, boxMax, invRange, range, norm, absNorm, steps, worldStepSize, 1.0, 1.0);
        fwd2 = licMidpoint(startPos, startUVW, boxMin, boxMax, invRange, range, norm, absNorm, steps, worldStepSize, 1.35, 1.0);
        bwd1 = licMidpoint(startPos, startUVW, boxMin, boxMax, invRange, range, norm, absNorm, steps, worldStepSize, 1.0, -1.0);
        bwd2 = licMidpoint(startPos, startUVW, boxMin, boxMax, invRange, range, norm, absNorm, steps, worldStepSize, 1.35, -1.0);
    } else {
        fwd1 = licRK4(startPos, startUVW, boxMin, boxMax, invRange, range, norm, absNorm, steps, worldStepSize, 1.0, 1.0);
        fwd2 = licRK4(startPos, startUVW, boxMin, boxMax, invRange, range, norm, absNorm, steps, worldStepSize, 1.35, 1.0);
        bwd1 = licRK4(startPos, startUVW, boxMin, boxMax, invRange, range, norm, absNorm, steps, worldStepSize, 1.0, -1.0);
        bwd2 = licRK4(startPos, startUVW, boxMin, boxMax, invRange, range, norm, absNorm, steps, worldStepSize, 1.35, -1.0);
    }
    return vec2((fwd1 + bwd1) / 2.0, (fwd2 + bwd2) / 2.0);
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
    if (clipped) {
        discard;
    }

    vec3 sphereNormal = mv.vNormal;
    bool isPoint = uPointClip.x > 0.5;
    if (isPoint) {
        vec2 pc = gl_PointCoord * 2.0 - 1.0;
        float r2 = dot(pc, pc);
        if (r2 > 1.0) discard;
        sphereNormal = vec3(pc, sqrt(1.0 - r2));
    }

    bool wireframe = uMeshColor_Wire.w > 0.5;
    if (wireframe) {
        FragColor = vec4(uMeshColor_Wire.xyz, 1.0);
        return;
    }

    vec3 range = uBoxMax - uBoxMin;
    float diag = length(range);
    if (!(diag > 1e-6)) diag = 1.0;
    float diagForInv = max(max(abs(range.x), abs(range.y)), abs(range.z));
    float epsInv = max(diagForInv * 1e-7, 1e-7);
    vec3 invRange;
    invRange.x = (abs(range.x) > epsInv) ? 1.0 / range.x : 0.0;
    invRange.y = (abs(range.y) > epsInv) ? 1.0 / range.y : 0.0;
    invRange.z = (abs(range.z) > epsInv) ? 1.0 / range.z : 0.0;
    vec3 uvw = worldToUVW(mv.vWorldPos, uBoxMin, invRange, range);

    vec3 scalarBase = uSurfaceColor_Op.xyz;
    if (hasScalars && (uScalars.x != uScalars.y)) {
        float st = clamp((mv.vScalar - uScalars.x) / (uScalars.y - uScalars.x), 0.0, 1.0);
        if (uNumBands > 1.0) {
            st = clamp(floor(st * uNumBands) / (uNumBands - 1.0), 0.0, 1.0);
        }
        scalarBase = textureLod(uColormapLUT, st, 0.0).rgb;
    }
    bool pointUseScalar = uPointClip.y > 0.5;
    if (isPoint && !pointUseScalar) {
        scalarBase = uSurfaceColor_Op.xyz;
    }

    vec3 vec = texture(uVectorTex, uvw).rgb;
    float mag = length(vec);

    float licGray = 0.5;
    const float magThresh = 5e-5;
    bool hasVector = mag > magThresh;

    float normEps = max(diag * diag * 1e-14, 1e-12);
    float flatEps = max(diag * diag * 1e-16, 1e-20);
    vec3 norm;
    if (isPoint) {
        vec3 n0 = sphereNormal;
        if (dot(n0, n0) < normEps) {
            n0 = normalize(vec3(range.x, range.y, 1.0));
            if (dot(n0,n0) < 1e-12) n0 = vec3(0.0, 1.0, 0.0);
        }
        norm = normalize(n0);
        if (!gl_FrontFacing) norm = -norm;
    } else if (uShadingMode.x > 0.5) {
        vec3 fdx = dFdx(mv.vWorldPos);
        vec3 fdy = dFdy(mv.vWorldPos);
        vec3 fc = cross(fdx, fdy);
        if (dot(fc, fc) < flatEps) fc = sphereNormal;
        if (dot(fc, fc) < normEps) fc = normalize(vec3(0.0, 1.0, 0.0));
        norm = normalize(fc);
    } else {
        vec3 n0 = sphereNormal;
        if (dot(n0, n0) < normEps) n0 = normalize(vec3(0.0, 1.0, 0.0));
        norm = normalize(n0);
        if (!gl_FrontFacing) norm = -norm;
    }

    vec3 absNorm = abs(norm);
    int steps = clamp(uLicSteps, 0, 128);
    float worldStepSize = clamp(uLicStepSize, 1e-6, diag * 2.0);

    if (hasVector) {
        if (uLicEnhanced != 0) {
            vec2 g = computeLicGrayDual(mv.vWorldPos, uvw, vec, uBoxMin, uBoxMax, invRange, range, norm, absNorm, steps, worldStepSize);
            float g1 = g.x;
            float g2 = g.y;
            float high = g1 - g2;
            float fused = g1 + 0.55 * high;
            licGray = clamp((fused - 0.5) * 2.15 + 0.5, 0.0, 1.0);
        } else {
            float g1 = computeLicGray(mv.vWorldPos, uvw, vec, uBoxMin, uBoxMax, invRange, range, norm, absNorm, steps, worldStepSize, 1.0);
            licGray = clamp((g1 - 0.5) * 1.8 + 0.5, 0.0, 1.0);
        }
    } else {
        float n = licNoiseSample3D(uvw);
        licGray = clamp((n - 0.5) * 1.8 + 0.5, 0.0, 1.0);
    }

    vec3 baseColor = scalarBase * (0.35 + 0.85 * licGray);
    baseColor = mix(vec3(dot(baseColor, vec3(0.2126,0.7152,0.0722))), baseColor, 0.85);

    if (uLicOnly) {
        FragColor = vec4(baseColor, uSurfaceColor_Op.w);
        return;
    }

    vec3 viewDir = normalize(uViewPos_PS.xyz - mv.vWorldPos);

    float aa = clamp(uMatRoughness() * uMatRoughness(), 0.04, 1.0);
    float a2 = aa * aa;
    float k  = (aa + 1.0) * (aa + 1.0) / 8.0;
    vec3  F0 = mix(vec3(0.04), baseColor, uMatMetallic());
    float oneMinusMetallic = 1.0 - uMatMetallic();

    vec3 totalDiffuse = vec3(0.0);
    vec3 totalSpecular = vec3(0.0);
    lightContributionPBR(uLightDir.xyz,   norm, uKeyIntensity(),   uKeyColor.xyz,   viewDir, a2, k, F0, oneMinusMetallic, totalDiffuse, totalSpecular);
    lightContributionPBR(uLightFill.xyz,  norm, uFillIntensity(),  uFillColor.xyz,  viewDir, a2, k, F0, oneMinusMetallic, totalDiffuse, totalSpecular);
    lightContributionPBR(uLightBack1.xyz, norm, uBackIntensity(),  uBackColor.xyz,  viewDir, a2, k, F0, oneMinusMetallic, totalDiffuse, totalSpecular);
    lightContributionPBR(uLightBack2.xyz, norm, uBackIntensity(),  uBackColor.xyz,  viewDir, a2, k, F0, oneMinusMetallic, totalDiffuse, totalSpecular);
    lightContributionPBR(uLightHead.xyz,  norm, uHeadIntensity(),  uHeadColor.xyz,  viewDir, a2, k, F0, oneMinusMetallic, totalDiffuse, totalSpecular);

    float surfaceOpacity = uSurfaceColor_Op.w;
    vec3 finalColor = baseColor * uMatAmbient()
                    + baseColor * totalDiffuse * uMatDiffuse()
                    + totalSpecular * uMatSpecular();
    float pointOpacity = uPointClip.z;
    if (isPoint) {
        finalColor += baseColor * 0.15;
        FragColor = vec4(finalColor, pointOpacity);
    } else {
        FragColor = vec4(finalColor, surfaceOpacity);
    }
}
