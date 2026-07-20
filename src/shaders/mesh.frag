#version 330 core

in vec3 vNormal;
in vec3 vFragPos;
in vec3 vWorldPos; 
in float vScalar;

uniform vec3 uLightDir;      // Key light direction (camera-relative Light Kit, rotated to world by C++)
uniform vec3 uViewPos;       // Camera position in world space
uniform bool uWireframe;
uniform vec3 uMeshColor;     // Wireframe color
uniform vec3 uSurfaceColor;  // Surface/base color (used when no colormap)
uniform vec3 uLightFill;
uniform vec3 uLightBack1;
uniform vec3 uLightBack2;
uniform vec3 uLightHead;

uniform float uScalarMin;    
uniform float uScalarMax;
uniform bool uHasScalars;    // true only when mesh has per-vertex scalar data

// New visualization uniforms
uniform float uSliceHeightX; // Slices along X-axis
uniform float uSliceHeightY; // Slices along Y-axis
uniform float uSliceHeightZ; // Slices along Z-axis
uniform bool uSliceEnabledX; // per-axis enable
uniform bool uSliceEnabledY;
uniform bool uSliceEnabledZ;

// ?? NEW UNIFORMS FOR INVERSION ??
uniform int uInvertX; // 0 = Keep Left,   1 = Keep Right
uniform int uInvertY; // 0 = Keep Bottom, 1 = Keep Top
uniform int uInvertZ; // 0 = Keep Back,   1 = Keep Front

uniform float uFilterMin;
uniform float uFilterMax;

uniform sampler1D uColormapLUT;

uniform bool uIsPoint;       // ponytail: point-sprite path -> shade as sphere
uniform bool uPointUseScalar; // ponytail: color point by scalar; else solid
uniform float uPointOpacity;  // ponytail: point sprite alpha
uniform float uSurfaceOpacity; // ponytail: surface fill alpha

// clipping is OFF unless the UI explicitly enables it. With the old
// default (slice=0, invert=false) the shader discarded the whole mesh because
// vWorldPos.x>0 was true for almost every vertex -> blank viewport.
uniform bool uClipEnabled;

out vec4 FragColor;

// Material properties
uniform float uMatAmbient;
uniform float uMatDiffuse;
uniform float uMatSpecular;
uniform float uMatShininess;  

// Light kit intensities
uniform float uKeyIntensity;
uniform float uFillIntensity;
uniform float uBackIntensity;
uniform float uHeadIntensity;

// Light kit colors
uniform vec3 uKeyColor;
uniform vec3 uFillColor;
uniform vec3 uBackColor;
uniform vec3 uHeadColor;

// Blinn-Phong diffuse + specular from fixed world-space lights. The specular
// highlight tracks the camera as it orbits (half-vector uses the view dir).
void lightContribution(vec3 rawLightDir, vec3 norm, float intensity,
                       vec3 lightColor, vec3 viewDir, inout vec3 diffuse, inout vec3 specular) {
    vec3 L = normalize(rawLightDir);
    float diff = max(dot(norm, L), 0.0);
    diffuse += lightColor * diff * intensity;

    vec3 H = normalize(L + viewDir);
    float specAngle = max(dot(norm, H), 0.0);
    float spec = pow(specAngle, max(uMatShininess, 1.0));
    specular += lightColor * spec * intensity;
}

void main() {
    // 1. Unified Slicing & Isolation Filtering
    // Clipping planes are gated by uClipEnabled; the scalar isolation filter is
    // independent so its min/max sliders work without enabling clipping.
    bool clipped = false;
    if (uClipEnabled) {
        bool clipX = uSliceEnabledX && ((uInvertX == 1) ? (vWorldPos.x < uSliceHeightX) : (vWorldPos.x > uSliceHeightX));
        bool clipY = uSliceEnabledY && ((uInvertY == 1) ? (vWorldPos.y < uSliceHeightY) : (vWorldPos.y > uSliceHeightY));
        bool clipZ = uSliceEnabledZ && ((uInvertZ == 1) ? (vWorldPos.z < uSliceHeightZ) : (vWorldPos.z > uSliceHeightZ));
        clipped = clipX || clipY || clipZ;
    }
    bool filterScalar = uHasScalars && (vScalar < uFilterMin || vScalar > uFilterMax);
    clipped = clipped || filterScalar;

    if (clipped) {
        discard;
    }

    // ponytail: point sprites carved into shaded spheres via gl_PointCoord.
    // Build a camera-facing hemisphere normal; fed into the `norm` below.
    vec3 sphereNormal = vNormal;
    if (uIsPoint) {
        vec2 pc = gl_PointCoord * 2.0 - 1.0;
        float r2 = dot(pc, pc);
        if (r2 > 1.0) discard;
        sphereNormal = vec3(pc, sqrt(1.0 - r2));
    }

    // 2. Early optimization check: Skip lighting loops entirely if wireframe mode is true
    if (uWireframe) {
        FragColor = vec4(uMeshColor, 1.0);
        return;
    }

    // 3. Normal & View Vectors (world space)
    // vNormal is the world-space normal; uViewPos is the camera position in
    // world space. Lighting is computed in world space so the lights remain
    // fixed in the world as the camera orbits the mesh.
    vec3 norm = normalize(sphereNormal);
    if (!gl_FrontFacing) {
        norm = -norm; // Ensures correct shading on interior walls exposed by cutting planes
    }
    vec3 viewDir = normalize(uViewPos - vWorldPos);

    // 4. Lighting Accumulation (world-space, orbit-invariant)
    vec3 totalDiffuse = vec3(0.0);
    vec3 totalSpecular = vec3(0.0);

    lightContribution(uLightDir, norm, uKeyIntensity, uKeyColor, viewDir, totalDiffuse, totalSpecular);
    lightContribution(uLightFill, norm, uFillIntensity, uFillColor, viewDir, totalDiffuse, totalSpecular);
    lightContribution(uLightBack1, norm, uBackIntensity, uBackColor, viewDir, totalDiffuse, totalSpecular);
    lightContribution(uLightBack2, norm, uBackIntensity, uBackColor, viewDir, totalDiffuse, totalSpecular);
    lightContribution(uLightHead, norm, uHeadIntensity, uHeadColor, viewDir, totalDiffuse, totalSpecular);

    // 5. Color Mapping
    vec3 baseColor = uSurfaceColor;
    if (uHasScalars && (uScalarMin != uScalarMax)) {
        float t = clamp((vScalar - uScalarMin) / (uScalarMax - uScalarMin), 0.0, 1.0);
        baseColor = texture(uColormapLUT, t).rgb;
    }
    // ponytail: points may ignore the scalar colormap and use a solid color
    if (uIsPoint && !uPointUseScalar) {
        baseColor = uSurfaceColor;
    }

    // 6. Shading Combination
    vec3 ambientComponent = baseColor * uMatAmbient;
    vec3 diffuseComponent = baseColor * totalDiffuse * uMatDiffuse;
    vec3 specularComponent = totalSpecular * uMatSpecular;

    vec3 finalColor = ambientComponent + diffuseComponent + specularComponent;
    // ponytail: points get a slight emissive boost so spheres read as "lit"
    if (uIsPoint) {
        finalColor += baseColor * 0.15f;
        FragColor = vec4(finalColor, uPointOpacity);
    } else {
        FragColor = vec4(finalColor, uSurfaceOpacity);
    }
}