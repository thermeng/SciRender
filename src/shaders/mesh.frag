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
    vec4  uSliceY;          // x = sliceHeightX, y = sliceHeightY, z = sliceHeightZ, w = 0
    vec4  uSliceEn;         // x = sliceEnabledX, y = sliceEnabledY, z = sliceEnabledZ, w = 0
    vec4  uInvert;          // x = invertX, y = invertY, z = invertZ, w = 0
    vec4  uFilter;          // x = filterMin, y = filterMax, z = 0, w = 0
    vec4  uMaterial;        // x = matAmbient, y = matDiffuse, z = matSpecular, w = matShininess
    vec4  uIntensities;     // x = keyIntensity, y = fillIntensity, z = backIntensity, w = headIntensity
    vec4  uPBR;             // x = matRoughness, y = matMetallic, z = pad, w = pad
};

in vec3 vNormal;
in vec3 vWorldPos; 
in float vScalar;

uniform sampler1D uColormapLUT;

out vec4 FragColor;

// Material properties
vec3  uMatAmbient()    { return vec3(uMaterial.x); }
float uMatDiffuse()    { return uMaterial.y; }
float uMatSpecular()   { return uMaterial.z; }
float uMatShininess()  { return uMaterial.w; }

// Light kit intensities
float uKeyIntensity()   { return uIntensities.x; }
float uFillIntensity()  { return uIntensities.y; }
float uBackIntensity()  { return uIntensities.z; }
float uHeadIntensity()  { return uIntensities.w; }

// PBR microfacet params
float uMatRoughness() { return uPBR.x; }
float uMatMetallic()  { return uPBR.y; }

// Microfacet (GGX normal distribution + Smith geometry + Schlick Fresnel),
// energy-conserving. uMatDiffuse/uMatSpecular are kept as gain knobs (defaults
// ~0.75/0.15) so existing presets reproduce their prior energy with metallic=0.
// baseColor is the full reflectance (albedo for dielectrics; F0 tint for metals).
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

    float a  = clamp(uMatRoughness(), 0.04, 1.0);
    float a2 = a * a;
    float d  = NdotH * NdotH * (a2 - 1.0) + 1.0;
    float D  = a2 / (3.14159265 * d * d);

    float k  = (a + 1.0) * (a + 1.0) / 24.0;        // direct-light Smith k'
    float Gl = NdotL / (NdotL * (1.0 - k) + k);
    float Gv = NdotV / (NdotV * (1.0 - k) + k);
    float G  = Gl * Gv;

    vec3  F0 = mix(vec3(0.04), baseColor, uMatMetallic());
    vec3  F  = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);

    float specFactor = D * G * VdotH / (4.0 * NdotL * NdotV + 1e-4);
    specular += lightColor * F * specFactor * intensity * uMatSpecular();

    vec3 kD = (1.0 - F) * (1.0 - uMatMetallic());
    diffuse += lightColor * kD * baseColor * NdotL * intensity * uMatDiffuse();
}

void main() {
    // 1. Unified Slicing & Isolation Filtering
    bool clipped = false;
    float clipEnabled = uPointClip.w;
    if (clipEnabled > 0.5) {
        bool clipX = bool(uSliceEn.x) && ((uInvert.x > 0.5) ? (vWorldPos.x < uSliceY.x) : (vWorldPos.x > uSliceY.x));
        bool clipY = bool(uSliceEn.y) && ((uInvert.y > 0.5) ? (vWorldPos.y < uSliceY.y) : (vWorldPos.y > uSliceY.y));
        bool clipZ = bool(uSliceEn.z) && ((uInvert.z > 0.5) ? (vWorldPos.z < uSliceY.z) : (vWorldPos.z > uSliceY.z));
        clipped = clipX || clipY || clipZ;
    }
    bool hasScalars = uScalars.z > 0.5;
    bool filterScalar = hasScalars && (vScalar < uFilter.x || vScalar > uFilter.y);
    clipped = clipped || filterScalar;

    if (clipped) {
        discard;
    }

    // ponytail: point sprites carved into shaded spheres via gl_PointCoord.
    vec3 sphereNormal = vNormal;
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

    vec3 norm = normalize(sphereNormal);
    if (!gl_FrontFacing) {
        norm = -norm;
    }
    vec3 viewDir = normalize(uViewPos_PS.xyz - vWorldPos);

    // baseColor is resolved before lighting: it drives F0 (metals) and the diffuse albedo.
    vec3 baseColor = uSurfaceColor_Op.xyz;
    if (hasScalars && (uScalars.x != uScalars.y)) {
        float t = clamp((vScalar - uScalars.x) / (uScalars.y - uScalars.x), 0.0, 1.0);
        baseColor = texture(uColormapLUT, t).rgb;
    }
    bool pointUseScalar = uPointClip.y > 0.5;
    if (isPoint && !pointUseScalar) {
        baseColor = uSurfaceColor_Op.xyz;
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
    float pointOpacity = uPointClip.z;
    if (isPoint) {
        finalColor += baseColor * 0.15;
        FragColor = vec4(finalColor, pointOpacity);
    } else {
        FragColor = vec4(finalColor, surfaceOpacity);
    }
}
