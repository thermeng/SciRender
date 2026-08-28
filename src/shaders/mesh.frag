#version 460 core

layout(std140) uniform MeshUBO {
    mat4  uMVP;
    mat4  uModel;
    vec4  uViewPos_PS;      // xyz = viewPos, w = pointSize
    vec4  uMeshColor_Wire;  // xyz = meshColor, w = wireframe
    vec4  uSurfaceColor_Op; // xyz = surfaceColor, w = surfaceOpacity
    vec4  uPointClip;       // x = isPoint, y = pointUseScalar, z = pointOpacity, w = clipEnabled
    vec4  uLightDir;
    vec4  uLightFill;
    vec4  uLightBack1;
    vec4  uLightBack2;
    vec4  uLightHead;
    vec4  uKeyColor;
    vec4  uFillColor;
    vec4  uBackColor;
    vec4  uHeadColor;
    vec4  uScalars;         // x = scalarMin, y = scalarMax, z = hasScalars(0/1), w = 0
    vec4  uClipY;           // x = clipHeightX, y = clipHeightY, z = clipHeightZ, w = 0
    vec4  uClipEn;          // x = clipEnabledX, y = clipEnabledY, z = clipEnabledZ, w = 0
    vec4  uInvert;          // x = invertX, y = invertY, z = invertZ, w = 0
    vec4  uFilter;          // x = filterMin, y = filterMax, z = filterEnabled(0/1), w = 0
    vec4  uMaterial;        // x = matAmbient, y = matDiffuse, z = matSpecular
    vec4  uIntensities;     // x = keyIntensity, y = fillIntensity, z = backIntensity, w = headIntensity
    vec4  uPBR;             // x = matRoughness, y = matMetallic, z = pad, w = pad
    vec4  uShadingMode;     // x = 0.0 smooth, 1.0 flat
};

in MeshVarying {
    vec3 vWorldPos;
    vec3 vNormal;
    float vScalar;
} mv;

uniform sampler1D uColormapLUT;
uniform float uNumBands = 0.0; // 0 = continuous, >1 = discrete bands

out vec4 FragColor;

// Material properties
vec3  uMatAmbient()    { return vec3(uMaterial.x); }
float uMatDiffuse()    { return uMaterial.y; }
float uMatSpecular()   { return uMaterial.z; }

// Light kit intensities
float uKeyIntensity()   { return uIntensities.x; }
float uFillIntensity()  { return uIntensities.y; }
float uBackIntensity()  { return uIntensities.z; }
float uHeadIntensity()  { return uIntensities.w; }

// PBR microfacet params
float uMatRoughness() { return uPBR.x; }
float uMatMetallic()  { return uPBR.y; }

// lightContributionPBR injected from pbr_common.glsl at compile time.

void main() {
    // 1. Unified Slicing & Isolation Filtering
    // [S6] Single fused clip predicate, cheapest test first: the scalar-range
    // compare runs before the 3-axis slice ladder, which only evaluates when
    // slice clipping is actually enabled. One discard site total.
    bool hasScalars = uScalars.z > 0.5;
    // Tolerance scaled to the data range: the filter window snaps to the data
    // min/max on every field switch, so extreme-value faces sit EXACTLY on the
    // bound. Perspective-correct interpolation then produces sub-ULP excursions
    // (e.g. -5.0000004 for a field pinned at -5) which a bare comparison turns
    // into discarded fragments — pinholes showing the far side as red/white
    // speckles. Driver-dependent, hence GPU-specific visibility.
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

    // ponytail: point sprites carved into shaded spheres via gl_PointCoord.
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

    // baseColor is resolved before lighting: it drives F0 (metals) and the diffuse albedo.
    vec3 baseColor = uSurfaceColor_Op.xyz;
    if (hasScalars && (uScalars.x != uScalars.y)) {
        float t = clamp((mv.vScalar - uScalars.x) / (uScalars.y - uScalars.x), 0.0, 1.0);
        if (uNumBands > 1.0) {
            t = floor(t * uNumBands) / (uNumBands - 1.0);
        }
        // [S5] Explicit LOD fetch skips the implicit derivative pair that
        // texture() computes for a mipless 1D LUT.
        baseColor = textureLod(uColormapLUT, t, 0.0).rgb;
    }
    bool pointUseScalar = uPointClip.y > 0.5;
    if (isPoint && !pointUseScalar) {
        baseColor = uSurfaceColor_Op.xyz;
    }

    // [S4] Normal selection gated on the UNIFORM shading flag, so the
    // screen-space derivative chain (dFdx/dFdy/cross/normalize) executes only
    // in flat mode; uniform control flow keeps the derivatives well-defined.
    // Screen-space cross normals are view-oriented regardless of winding, so
    // only the interpolated attribute normal takes the back-face flip — this
    // also removes the old double negation on the flat/back-face path while
    // preserving its net result.
    vec3 norm;
    if (uShadingMode.x > 0.5) {
        norm = normalize(cross(dFdx(mv.vWorldPos), dFdy(mv.vWorldPos)));
    } else {
        norm = normalize(sphereNormal);
        if (!gl_FrontFacing) norm = -norm;
    }
    vec3 viewDir = normalize(uViewPos_PS.xyz - mv.vWorldPos);

    // [S1] Microfacet parameters shared by every light in the kit, computed
    // once per fragment instead of five times.
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

    // [S1] baseColor and the material scalars fold in exactly once, after the
    // accumulated sums (algebraically identical to the previous per-light form).
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
