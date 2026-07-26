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
};

in vec3 vNormal;
in vec3 vWorldPos; 
in float vScalar;

uniform sampler1D uColormapLUT;

out vec4 FragColor;

// Material properties
vec3  uMatAmbient()    { return uMaterial.xyz; }
float uMatDiffuse()    { return uMaterial.y; }
float uMatSpecular()   { return uMaterial.z; }
float uMatShininess()  { return uMaterial.w; }

// Light kit intensities
float uKeyIntensity()   { return uIntensities.x; }
float uFillIntensity()  { return uIntensities.y; }
float uBackIntensity()  { return uIntensities.z; }
float uHeadIntensity()  { return uIntensities.w; }

// Blinn-Phong diffuse + specular from fixed world-space lights. The specular
// highlight tracks the camera as it orbits (half-vector uses the view dir).
void lightContribution(vec3 rawLightDir, vec3 norm, float intensity,
                       vec3 lightColor, vec3 viewDir, inout vec3 diffuse, inout vec3 specular) {
    vec3 L = normalize(rawLightDir);
    float diff = max(dot(norm, L), 0.0);
    diffuse += lightColor * diff * intensity;

    vec3 H = normalize(L + viewDir);
    float specAngle = max(dot(norm, H), 0.0);
    float spec = pow(specAngle, max(uMatShininess(), 1.0));
    specular += lightColor * spec * intensity;
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

    bool pointUseScalar = uPointClip.y > 0.5;
    if (isPoint && !pointUseScalar) {
        baseColor = uSurfaceColor_Op.xyz;
    }

    vec3 ambientComponent = baseColor * uMatAmbient();
    vec3 diffuseComponent = baseColor * totalDiffuse * uMatDiffuse();
    vec3 specularComponent = totalSpecular * uMatSpecular();

    vec3 finalColor = ambientComponent + diffuseComponent + specularComponent;
    float pointOpacity = uPointClip.z;
    float surfaceOpacity = uSurfaceColor_Op.w;
    if (isPoint) {
        finalColor += baseColor * 0.15;
        FragColor = vec4(finalColor, pointOpacity);
    } else {
        FragColor = vec4(finalColor, surfaceOpacity);
    }
}
