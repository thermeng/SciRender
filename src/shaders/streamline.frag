#version 460 core

in vec3 vWorldPos;
in vec3 vNormal;
in float vDashFlag;
in float vU;
in float vMag;

layout(std140, binding = 3) uniform StreamlineUBO {
    mat4  uMVP;
    mat4  uModel;
    vec4  uViewPos;
    vec4  uLightDir;
    vec4  uTime_Opacity;       // x: time, y: opacity
    vec4  uColor_UseColormap;  // xyz: fallback color, w: use colormap flag (0.0 or 1.0)
    vec4  uMagRange;           // x: minMag, y: maxMag
    vec4  uMaterial;           // x: ambient, y: diffuse, z: specular, w: specularPower
    vec4  uRibbon;            // x = ribbonWidth, y = taperFactor, z = dashEnabled, w = dashSpeed
    vec4  uArrowParams;       // x = arrowAnimSpeed, yzw = pad
    vec4  uPBR;               // x = matRoughness, y = matMetallic, z = pad, w = pad
};

uniform sampler1D uColormapLUT;

out vec4 FragColor;

float uMatAmbient()  { return uMaterial.x; }
float uMatDiffuse()  { return uMaterial.y; }
float uMatSpecular() { return uMaterial.z; }
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
    float a  = clamp(uMatRoughness(), 0.04, 1.0);
    float a2 = a * a;
    float d  = NdotH * NdotH * (a2 - 1.0) + 1.0;
    float D  = a2 / (3.14159265 * d * d);
    float k  = (a + 1.0) * (a + 1.0) / 24.0;
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
    // 1. Color evaluation
    bool useColormap = uColor_UseColormap.w > 0.5;
    float magMin = uMagRange.x;
    float magMax = uMagRange.y;
    float span = max(magMax - magMin, 1e-6);
    float normMag = clamp((vMag - magMin) / span, 0.0, 1.0);

    vec3 baseColor = useColormap ? texture(uColormapLUT, normMag).rgb : uColor_UseColormap.xyz;

    // 2. Lighting (microfacet GGX, camera-relative key light)
    vec3 N = normalize(vNormal);
    vec3 L = normalize(uLightDir.xyz);
    vec3 V = normalize(uViewPos.xyz - vWorldPos);
    if (!gl_FrontFacing) N = -N;

    vec3 totalDiffuse = vec3(0.0);
    vec3 totalSpecular = vec3(0.0);
    lightContributionPBR(L, N, 1.0, vec3(1.0), V, baseColor, totalDiffuse, totalSpecular);

    // Dashing/Striping control
    float dash = 1.0;
    if (uRibbon.z > 0.5 && vDashFlag > 0.5) {
        dash = step(0.5, fract(vU * 20.0 - uTime_Opacity.x * uRibbon.w * 1.5));
    }

    float dashAlpha = mix(1.0, dash, 0.7);
    float alpha = dashAlpha * uTime_Opacity.y;
    vec3 color = baseColor * uMatAmbient() + totalDiffuse + totalSpecular;

    FragColor = vec4(color, alpha);
}