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

// ?? NEW UNIFORMS FOR INVERSION ??
uniform int uInvertX; // 0 = Keep Left,   1 = Keep Right
uniform int uInvertY; // 0 = Keep Bottom, 1 = Keep Top
uniform int uInvertZ; // 0 = Keep Back,   1 = Keep Front

uniform float uFilterMin;
uniform float uFilterMax;

uniform sampler1D uColormapLUT;

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

// diffuse-only from fixed world-space lights; no specular term, so
// the highlight never tracks the camera as it orbits.
void lightContribution(vec3 rawLightDir, vec3 norm, float intensity,
                       vec3 lightColor, inout vec3 diffuse) {
    vec3 L = normalize(rawLightDir);
    float diff = max(dot(norm, L), 0.0);
    diffuse += lightColor * diff * intensity;
}

void main() {
    // 1. Unified Slicing & Isolation Filtering
    // Evaluates all clipping and scalar isolation states together to optimize branch prediction
    bool clipped = false;
    if (uClipEnabled) {
        bool clipX = (uInvertX == 1) ? (vWorldPos.x < uSliceHeightX) : (vWorldPos.x > uSliceHeightX);
        bool clipY = (uInvertY == 1) ? (vWorldPos.y < uSliceHeightY) : (vWorldPos.y > uSliceHeightY);
        bool clipZ = (uInvertZ == 1) ? (vWorldPos.z < uSliceHeightZ) : (vWorldPos.z > uSliceHeightZ);
        bool filterScalar = uHasScalars && (vScalar < uFilterMin || vScalar > uFilterMax);
        clipped = clipX || clipY || clipZ || filterScalar;
    }

    if (clipped) {
        discard;
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
    vec3 norm = normalize(vNormal);
    if (!gl_FrontFacing) {
        norm = -norm; // Ensures correct shading on interior walls exposed by cutting planes
    }

    // 4. Lighting Accumulation (world-space, orbit-invariant)
    vec3 totalDiffuse = vec3(0.0);

    lightContribution(uLightDir, norm, uKeyIntensity, uKeyColor, totalDiffuse);
    lightContribution(uLightFill, norm, uFillIntensity, uFillColor, totalDiffuse);
    lightContribution(uLightBack1, norm, uBackIntensity, uBackColor, totalDiffuse);
    lightContribution(uLightBack2, norm, uBackIntensity, uBackColor, totalDiffuse);
    lightContribution(uLightHead, norm, uHeadIntensity, uHeadColor, totalDiffuse);

    // 5. Color Mapping
    vec3 baseColor = uSurfaceColor;
    if (uHasScalars && (uScalarMin != uScalarMax)) {
        float t = clamp((vScalar - uScalarMin) / (uScalarMax - uScalarMin), 0.0, 1.0);
        baseColor = texture(uColormapLUT, t).rgb;
    }

    // 6. Shading Combination
    vec3 ambientComponent = baseColor * uMatAmbient;
    vec3 diffuseComponent = baseColor * totalDiffuse * uMatDiffuse;

    vec3 finalColor = ambientComponent + diffuseComponent;
    FragColor = vec4(finalColor, 1.0);
}