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
    vec4  uSliceY;
    vec4  uSliceEn;
    vec4  uInvert;
    vec4  uFilter;
    vec4  uMaterial;
    vec4  uIntensities;
    vec4  uPBR;             // x = matRoughness, y = matMetallic, z = pad, w = pad
    vec4  uShadingMode;     // x = 0.0 smooth, 1.0 flat; y = 0.0 normal clip, 1.0 crinkle clip
};

in vec3 vNormal;
in vec3 vWorldPos;
in float vScalar;

uniform sampler2D uPrevDepth;
uniform sampler1D uColormapLUT;
uniform int uLayerIndex;

out vec4 FragColor;

vec3  uMatAmbient()    { return vec3(uMaterial.x); }
float uMatDiffuse()    { return uMaterial.y; }
float uMatSpecular()   { return uMaterial.z; }
float uKeyIntensity()   { return uIntensities.x; }
float uFillIntensity()  { return uIntensities.y; }
float uBackIntensity()  { return uIntensities.z; }
float uHeadIntensity()  { return uIntensities.w; }
float uMatRoughness() { return uPBR.x; }
float uMatMetallic()  { return uPBR.y; }

void lightContributionPBR(vec3 rawLightDir, vec3 norm, float intensity,
                          vec3 lightColor, vec3 viewDir, vec3 baseColor,
                          inout vec3 diffuse, inout vec3 specular) {
    vec3 L = normalize(rawLightDir);
    float NdotL = max(dot(norm, L), 0.0);
    float NdotV = max(dot(norm, viewDir), 0.0);
    if (NdotL <= 0.0) return;
    vec3 H = normalize(L + viewDir);
    float NdotH = max(dot(norm, H), 0.0);
    float VdotH = max(dot(viewDir, H), 0.0);
    float a  = clamp(uMatRoughness() * uMatRoughness(), 0.04, 1.0);
    float a2 = a * a;
    float d  = NdotH * NdotH * (a2 - 1.0) + 1.0;
    float D  = a2 / (3.14159265 * d * d);
    float k  = (a + 1.0) * (a + 1.0) / 8.0;
    float Gl = NdotL / (NdotL * (1.0 - k) + k);
    float Gv = NdotV / (NdotV * (1.0 - k) + k);
    float G  = Gl * Gv;
    vec3  F0 = mix(vec3(0.04), baseColor, uMatMetallic());
    vec3  F  = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);
    float specFactor = D * G / (4.0 * NdotL * NdotV + 1e-4);
    specular += lightColor * F * specFactor * intensity * uMatSpecular();
    vec3 kD = (1.0 - F) * (1.0 - uMatMetallic());
    diffuse += lightColor * kD * baseColor * NdotL * intensity * uMatDiffuse();
}

void main() {
    bool clipped = false;
    float clipEnabled = uPointClip.w;
    bool crinkleMode = uShadingMode.y > 0.5;
    // Peel is surfaces-only, but keep the same crinkle gate as mesh.frag:95
    // so slice clipping is consistent between opaque and transparent layers.
    if (clipEnabled > 0.5 && !crinkleMode) {
        bool clipX = bool(uSliceEn.x) && ((uInvert.x > 0.5) ? (vWorldPos.x < uSliceY.x) : (vWorldPos.x > uSliceY.x));
        bool clipY = bool(uSliceEn.y) && ((uInvert.y > 0.5) ? (vWorldPos.y < uSliceY.y) : (vWorldPos.y > uSliceY.y));
        bool clipZ = bool(uSliceEn.z) && ((uInvert.z > 0.5) ? (vWorldPos.z < uSliceY.z) : (vWorldPos.z > uSliceY.z));
        clipped = clipX || clipY || clipZ;
    }
    bool hasScalars = uScalars.z > 0.5;
    // Same epsilon as mesh.frag:108 — avoids pinholes from perspective interpolation
    // sub-ULP excursions when filter bounds snap exactly to data min/max.
    float filterEps = 1e-5 * abs(uScalars.y - uScalars.x) + 1e-9;
    bool filterScalar = hasScalars && (vScalar < uFilter.x - filterEps || vScalar > uFilter.y + filterEps);
    clipped = clipped || filterScalar;
    if (clipped) discard;

    ivec2 pix = ivec2(gl_FragCoord.xy);
    float prevDepth = texelFetch(uPrevDepth, pix, 0).r;
    // Layer 0 tests against the opaque-scene depth (keep transparent surfaces
    // in front of opaque geometry). Layers >= 1 keep only fragments strictly
    // BEHIND the previous layer - the old single `>=` test discarded those,
    // so every layer past the first came out empty and transparency showed
    // just one front surface. Small epsilon avoids z-fighting re-peel of the
    // same depth value (depth32F still has quantization).
    const float kPeelEps = 1e-5;
    if (uLayerIndex == 0) {
        if (gl_FragCoord.z >= prevDepth) discard;
    } else {
        if (gl_FragCoord.z <= prevDepth + kPeelEps) discard;
    }
    // Screen-space cross(dFdx, dFdy) normals are view-oriented regardless of
    // winding; attribute normals are not, so only they need the back-face flip.
    vec3 norm;
    if (uShadingMode.x > 0.5) {
        norm = normalize(cross(dFdx(vWorldPos), dFdy(vWorldPos)));
    } else {
        norm = normalize(vNormal);
        if (!gl_FrontFacing) norm = -norm;
    }
    vec3 viewDir = normalize(uViewPos_PS.xyz - vWorldPos);

    vec3 baseColor = uSurfaceColor_Op.xyz;
    if (hasScalars && (uScalars.x != uScalars.y)) {
        float t = clamp((vScalar - uScalars.x) / (uScalars.y - uScalars.x), 0.0, 1.0);
        baseColor = texture(uColormapLUT, t).rgb;
    }

    vec3 totalDiffuse = vec3(0.0);
    vec3 totalSpecular = vec3(0.0);
    lightContributionPBR(uLightDir.xyz,   norm, uKeyIntensity(),   uKeyColor.xyz,   viewDir, baseColor, totalDiffuse, totalSpecular);
    lightContributionPBR(uLightFill.xyz,  norm, uFillIntensity(),  uFillColor.xyz,  viewDir, baseColor, totalDiffuse, totalSpecular);
    lightContributionPBR(uLightBack1.xyz, norm, uBackIntensity(),  uBackColor.xyz,  viewDir, baseColor, totalDiffuse, totalSpecular);
    lightContributionPBR(uLightBack2.xyz, norm, uBackIntensity(),  uBackColor.xyz,  viewDir, baseColor, totalDiffuse, totalSpecular);
    lightContributionPBR(uLightHead.xyz,  norm, uHeadIntensity(),  uHeadColor.xyz,  viewDir, baseColor, totalDiffuse, totalSpecular);

    float surfaceOpacity = uSurfaceColor_Op.w;
    vec3 ambientComponent = baseColor * uMatAmbient();
    vec3 diffuseComponent = totalDiffuse;
    vec3 specularComponent = totalSpecular;
    vec3 finalColor = ambientComponent + diffuseComponent + specularComponent;

    FragColor = vec4(finalColor, surfaceOpacity);
}
