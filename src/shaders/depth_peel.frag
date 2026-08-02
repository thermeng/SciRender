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
};

in vec3 vNormal;
in vec3 vWorldPos;
in float vScalar;

uniform sampler2D uPrevDepth;
uniform sampler1D uColormapLUT;
uniform float uPeelLayer;

out vec4 FragColor;

vec3  uMatAmbient()    { return vec3(uMaterial.x); }
float uMatDiffuse()    { return uMaterial.y; }
float uMatSpecular()   { return uMaterial.z; }
float uMatShininess()  { return uMaterial.w; }
float uKeyIntensity()   { return uIntensities.x; }
float uFillIntensity()  { return uIntensities.y; }
float uBackIntensity()  { return uIntensities.z; }
float uHeadIntensity()  { return uIntensities.w; }

void lightContribution(vec3 rawLightDir, vec3 norm, float intensity,
                       vec3 lightColor, vec3 viewDir, inout vec3 diffuse, inout vec3 specular) {
    vec3 L = normalize(rawLightDir);
    float diff = max(dot(norm, L), 0.0);
    diffuse += lightColor * diff * intensity;
    vec3 H = normalize(L + viewDir);
    float specAngle = max(dot(norm, H), 0.0);
    specular += lightColor * pow(specAngle, max(uMatShininess(), 1.0)) * intensity;
}

void main() {
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
    if (clipped) discard;

    if (uPeelLayer < 0.5) {
        // Layer 0: standard depth test (GL_LESS handled by GL state)
    } else {
        // Layer 1+: peel against previous layer's depth
        ivec2 pix = ivec2(gl_FragCoord.xy);
        float prevDepth = texelFetch(uPrevDepth, pix, 0).r;
        if (gl_FragCoord.z >= prevDepth) discard;
    }

    vec3 norm = normalize(vNormal);
    if (!gl_FrontFacing) norm = -norm;
    vec3 viewDir = normalize(uViewPos_PS.xyz - vWorldPos);

    vec3 totalDiffuse = vec3(0.0);
    vec3 totalSpecular = vec3(0.0);
    lightContribution(uLightDir.xyz, norm, uKeyIntensity(), uKeyColor.xyz, viewDir, totalDiffuse, totalSpecular);
    lightContribution(uLightFill.xyz, norm, uFillIntensity(), uFillColor.xyz, viewDir, totalDiffuse, totalSpecular);
    lightContribution(uLightBack1.xyz, norm, uBackIntensity(), uBackColor.xyz, viewDir, totalDiffuse, totalSpecular);
    lightContribution(uLightBack2.xyz, norm, uBackIntensity(), uBackColor.xyz, viewDir, totalDiffuse, totalSpecular);
    lightContribution(uLightHead.xyz, norm, uHeadIntensity(), uHeadColor.xyz, viewDir, totalDiffuse, totalSpecular);

    vec3 baseColor = uSurfaceColor_Op.xyz;
    if (hasScalars && (uScalars.x != uScalars.y)) {
        float t = clamp((vScalar - uScalars.x) / (uScalars.y - uScalars.x), 0.0, 1.0);
        baseColor = texture(uColormapLUT, t).rgb;
    }

    float surfaceOpacity = uSurfaceColor_Op.w;
    vec3 ambientComponent = baseColor * uMatAmbient();
    vec3 diffuseComponent = baseColor * totalDiffuse * uMatDiffuse();
    vec3 specularComponent = totalSpecular * uMatSpecular();
    vec3 finalColor = ambientComponent + diffuseComponent + specularComponent;

    FragColor = vec4(finalColor, surfaceOpacity);
}
