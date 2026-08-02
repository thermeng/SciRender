#version 460 core

layout(std140) uniform GlyphUBO {
    mat4  uMVP;
    vec4  uScale_MagMin_MagMax_ScaleByMag;
    vec4  uMeshExtent_MagTransform_ViewPosY_ColorR;
    vec4  uLightDir_ColorGB;
    vec4  uColorB_UseColormap;
    vec4  uPBR;              // x = matRoughness, y = matMetallic, z = pad, w = pad
};

in vec3 vNormal;
in vec3 vWorldPos;
in float vMag;

uniform vec3 uViewPos;
uniform sampler1D uColormapLUT;

out vec4 FragColor;

float uMatAmbient()  { return 0.35; }
float uMatDiffuse()  { return 0.65; }
float uMatSpecular() { return 0.15; }
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
    vec3 n = normalize(vNormal);
    if (!gl_FrontFacing) n = -n;            // preserve legacy double-sided shading
    vec3 L = normalize(uLightDir_ColorGB.xyz);
    vec3 color = vec3(uMeshExtent_MagTransform_ViewPosY_ColorR.w, uLightDir_ColorGB.w, uColorB_UseColormap.x);
    bool useColormap = uColorB_UseColormap.y > 0.5;
    vec3 baseColor = useColormap ? texture(uColormapLUT, vMag).rgb : color;
    vec3 viewDir = normalize(uViewPos - vWorldPos);

    vec3 totalDiffuse = vec3(0.0);
    vec3 totalSpecular = vec3(0.0);
    lightContributionPBR(L, n, 1.0, vec3(1.0), viewDir, baseColor, totalDiffuse, totalSpecular);

    vec3 col = baseColor * uMatAmbient() + totalDiffuse + totalSpecular;
    FragColor = vec4(col, 1.0);
}
