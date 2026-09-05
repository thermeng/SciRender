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
uniform sampler2D uNoiseTex;
uniform vec3 uBoxMin;
uniform vec3 uBoxMax;
uniform int uLicSteps;
uniform float uLicStepSize;
uniform float uLicNoiseFreq;
uniform int uLicBoundaryMode;
uniform vec3 uUvwScale = vec3(1.0);
uniform vec3 uUvwOffset = vec3(0.0);

out vec4 FragColor;

vec3 worldToUVW(vec3 pos, vec3 boxMin, vec3 invRange, vec3 range) {
    vec3 uvw = clamp((pos - boxMin) * invRange, vec3(0.0), vec3(1.0));
    if (abs(range.x) <= 1e-8) uvw.x = 0.5;
    if (abs(range.y) <= 1e-8) uvw.y = 0.5;
    if (abs(range.z) <= 1e-8) uvw.z = 0.5;
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

vec2 noiseUVForPos(vec3 worldPos, vec3 boxMin, vec3 invRange, vec3 absNorm) {
    vec3 npos = (worldPos - boxMin) * invRange;
    if (absNorm.z >= absNorm.x && absNorm.z >= absNorm.y) {
        return npos.xy * uLicNoiseFreq;
    } else if (absNorm.y >= absNorm.x) {
        return npos.xz * uLicNoiseFreq;
    } else {
        return npos.yz * uLicNoiseFreq;
    }
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
    vec3 invRange;
    invRange.x = (abs(range.x) > 1e-8) ? 1.0 / range.x : 0.0;
    invRange.y = (abs(range.y) > 1e-8) ? 1.0 / range.y : 0.0;
    invRange.z = (abs(range.z) > 1e-8) ? 1.0 / range.z : 0.0;
    vec3 uvw = worldToUVW(mv.vWorldPos, uBoxMin, invRange, range);

    vec3 scalarBase = uSurfaceColor_Op.xyz;
    if (hasScalars && (uScalars.x != uScalars.y)) {
        float st = clamp((mv.vScalar - uScalars.x) / (uScalars.y - uScalars.x), 0.0, 1.0);
        if (uNumBands > 1.0) {
            st = floor(st * uNumBands) / (uNumBands - 1.0);
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

    vec3 norm;
    if (isPoint) {
        norm = normalize(sphereNormal);
        if (!gl_FrontFacing) norm = -norm;
    } else if (uShadingMode.x > 0.5) {
        norm = normalize(cross(dFdx(mv.vWorldPos), dFdy(mv.vWorldPos)));
    } else {
        norm = normalize(sphereNormal);
        if (!gl_FrontFacing) norm = -norm;
    }

    vec3 absNorm = abs(norm);
    int steps = clamp(uLicSteps, 0, 128);
    float worldStepSize = max(uLicStepSize, 1e-6);

    vec3 dir;
    if (mag > magThresh) {
        dir = vec / mag;
    } else {
        if (absNorm.z >= absNorm.x && absNorm.z >= absNorm.y) {
            dir = vec3(1.0, 0.0, 0.0);
        } else if (absNorm.y >= absNorm.x) {
            dir = vec3(1.0, 0.0, 0.0);
        } else {
            dir = vec3(0.0, 0.0, 1.0);
        }
    }

    float acc = 0.0;
    float totalWeight = 0.0;

        {
            vec2 noiseUV = noiseUVForPos(mv.vWorldPos, uBoxMin, invRange, absNorm);
            acc += texture(uNoiseTex, noiseUV).r;
            totalWeight += 1.0;
        }

        {
            vec3 curPos = mv.vWorldPos;
            vec3 curDir = dir;
            for (int i = 0; i < steps; ++i) {
                vec3 nextPos = curPos + curDir * worldStepSize;
                bool oob = nextPos.x < uBoxMin.x - 1e-6 || nextPos.x > uBoxMax.x + 1e-6 ||
                           nextPos.y < uBoxMin.y - 1e-6 || nextPos.y > uBoxMax.y + 1e-6 ||
                           nextPos.z < uBoxMin.z - 1e-6 || nextPos.z > uBoxMax.z + 1e-6;
                if (oob) {
                    break;
                }
                vec3 sUVW = worldToUVW(nextPos, uBoxMin, invRange, range);
                vec3 sVec = texture(uVectorTex, sUVW).rgb;
                float sMag = length(sVec);
                if (sMag > magThresh) {
                    curDir = sVec / sMag;
                }
                vec2 noiseUV = noiseUVForPos(nextPos, uBoxMin, invRange, absNorm);
                acc += texture(uNoiseTex, noiseUV).r;
                totalWeight += 1.0;
                curPos = nextPos;
            }
        }
        {
            vec3 curPos = mv.vWorldPos;
            vec3 curDir = -dir;
            for (int i = 0; i < steps; ++i) {
                vec3 nextPos = curPos + curDir * worldStepSize;
                bool oob = nextPos.x < uBoxMin.x - 1e-6 || nextPos.x > uBoxMax.x + 1e-6 ||
                           nextPos.y < uBoxMin.y - 1e-6 || nextPos.y > uBoxMax.y + 1e-6 ||
                           nextPos.z < uBoxMin.z - 1e-6 || nextPos.z > uBoxMax.z + 1e-6;
                if (oob) {
                    break;
                }
                vec3 sUVW = worldToUVW(nextPos, uBoxMin, invRange, range);
                vec3 sVec = texture(uVectorTex, sUVW).rgb;
                float sMag = length(sVec);
                if (sMag > magThresh) {
                    curDir = -sVec / sMag;
                }
                vec2 noiseUV = noiseUVForPos(nextPos, uBoxMin, invRange, absNorm);
                acc += texture(uNoiseTex, noiseUV).r;
                totalWeight += 1.0;
                curPos = nextPos;
            }
        }

        licGray = (totalWeight > 0.0) ? acc / totalWeight : 0.5;

    float licContrast = 1.8;
    licGray = clamp((licGray - 0.5) * licContrast + 0.5, 0.0, 1.0);
    vec3 baseColor = scalarBase * (0.35 + 0.85 * licGray);
    baseColor = mix(vec3(dot(baseColor, vec3(0.2126,0.7152,0.0722))), baseColor, 0.85);

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
