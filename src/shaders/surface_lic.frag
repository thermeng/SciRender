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
uniform sampler2D uLicTex; // R16F, screen-space LIC intensity; negative = invalid
uniform bool uLicOnly = false;

out vec4 FragColor;

vec3  uMatAmbient()    { return vec3(uMaterial.x); }
float uMatDiffuse()    { return uMaterial.y; }
float uMatSpecular()   { return uMaterial.z; }
float uKeyIntensity()  { return uIntensities.x; }
float uFillIntensity() { return uIntensities.y; }
float uBackIntensity() { return uIntensities.z; }
float uHeadIntensity() { return uIntensities.w; }
float uMatRoughness()  { return uPBR.x; }
float uMatMetallic()   { return uPBR.y; }

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

    vec3 scalarBase = uSurfaceColor_Op.xyz;
    if (hasScalars && (uScalars.x != uScalars.y)) {
        float st = clamp((mv.vScalar - uScalars.x) / (uScalars.y - uScalars.x), 0.0, 1.0);
        if (uNumBands > 1.0) st = clamp(floor(st * uNumBands) / (uNumBands - 1.0), 0.0, 1.0);
        scalarBase = textureLod(uColormapLUT, st, 0.0).rgb;
    }
    bool pointUseScalar = uPointClip.y > 0.5;
    if (isPoint && !pointUseScalar) scalarBase = uSurfaceColor_Op.xyz;

    ivec2 texel = ivec2(gl_FragCoord.xy);
    float licGray = texelFetch(uLicTex, texel, 0).r;
    if (!(licGray >= 0.0)) licGray = 0.5;
    licGray = clamp(licGray, 0.0, 1.0);

    vec3 baseColor = scalarBase * (0.35 + 0.85 * licGray);
    baseColor = mix(vec3(dot(baseColor, vec3(0.2126, 0.7152, 0.0722))), baseColor, 0.85);

    if (uLicOnly) {
        FragColor = vec4(baseColor, uSurfaceColor_Op.w);
        return;
    }

    float normEps = 1e-12;
    float flatEps = 1e-20;
    vec3 norm;
    if (isPoint) {
        vec3 n0 = sphereNormal;
        if (dot(n0, n0) < normEps) n0 = vec3(0.0, 1.0, 0.0);
        norm = normalize(n0);
        if (!gl_FrontFacing) norm = -norm;
    } else if (uShadingMode.x > 0.5) {
        vec3 fdx = dFdx(mv.vWorldPos);
        vec3 fdy = dFdy(mv.vWorldPos);
        vec3 fc = cross(fdx, fdy);
        if (dot(fc, fc) < flatEps) fc = sphereNormal;
        if (dot(fc, fc) < normEps) fc = vec3(0.0, 1.0, 0.0);
        norm = normalize(fc);
    } else {
        vec3 n0 = sphereNormal;
        if (dot(n0, n0) < normEps) n0 = vec3(0.0, 1.0, 0.0);
        norm = normalize(n0);
        if (!gl_FrontFacing) norm = -norm;
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

    vec3 finalColor = baseColor * uMatAmbient()
                    + baseColor * totalDiffuse * uMatDiffuse()
                    + totalSpecular * uMatSpecular();
    float pointOpacity = uPointClip.z;
    if (isPoint) {
        finalColor += baseColor * 0.15;
        FragColor = vec4(finalColor, pointOpacity);
    } else {
        FragColor = vec4(finalColor, uSurfaceColor_Op.w);
    }
}
