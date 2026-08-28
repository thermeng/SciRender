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
    vec4  uPBR;             // x = matRoughness, y = matMetallic, z = pad, w = pad
    vec4  uShadingMode;     // x = 0.0 smooth, 1.0 flat; y = 0.0 normal clip, 1.0 crinkle clip
};

in vec3 vNormal;
in vec3 vWorldPos;
in float vScalar;

uniform sampler2D uPrevDepth;
uniform sampler1D uColormapLUT;
uniform float uNumBands = 0.0; // 0 = continuous, >1 = discrete bands
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

// lightContributionPBR injected from pbr_common.glsl at compile time.

void main() {
    // [S6] Fused clip predicate, cheapest test first (mirrors mesh.frag).
    bool hasScalars = uScalars.z > 0.5;
    // Same epsilon as mesh.frag — avoids pinholes from perspective interpolation
    // sub-ULP excursions when filter bounds snap exactly to data min/max.
    float filterEps = 1e-5 * abs(uScalars.y - uScalars.x) + 1e-9;
    bool clipped = (uFilter.z > 0.5) && hasScalars &&
                   (vScalar < uFilter.x - filterEps || vScalar > uFilter.y + filterEps);
    bool crinkleMode = uShadingMode.y > 0.5;
    // Peel is surfaces-only, but keep the crinkle gate so slice clipping is
    // consistent between opaque and transparent layers.
    if (!clipped && uPointClip.w > 0.5 && !crinkleMode) {
        bool clipX = bool(uClipEn.x) && ((uInvert.x > 0.5) ? (vWorldPos.x < uClipY.x) : (vWorldPos.x > uClipY.x));
        bool clipY = bool(uClipEn.y) && ((uInvert.y > 0.5) ? (vWorldPos.y < uClipY.y) : (vWorldPos.y > uClipY.y));
        bool clipZ = bool(uClipEn.z) && ((uInvert.z > 0.5) ? (vWorldPos.z < uClipY.z) : (vWorldPos.z > uClipY.z));
        clipped = clipX || clipY || clipZ;
    }
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
        if (uNumBands > 1.0) {
            t = floor(t * uNumBands) / (uNumBands - 1.0);
        }
        // [S5] Explicit LOD skips implicit derivatives on the mipless 1D LUT.
        baseColor = textureLod(uColormapLUT, t, 0.0).rgb;
    }

    // [S1] Microfacet parameters shared by every light in the kit.
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
    // [S1] baseColor and material scalars fold in once after accumulation.
    vec3 finalColor = baseColor * uMatAmbient()
                    + baseColor * totalDiffuse * uMatDiffuse()
                    + totalSpecular * uMatSpecular();

    FragColor = vec4(finalColor, surfaceOpacity);
}
