#version 460 core

layout(std140) uniform GlyphUBO {
    mat4  uMVP;
    vec4  uScale_MagMin_MagMax_ScaleByMag;
    vec4  uMeshExtent_MagTransform_ViewPosY_ColorR;
    vec4  uLightDir_ColorGB;
    vec4  uColorB_UseColormap;
};

in vec3 vNormal;
in vec3 vWorldPos;
in float vMag;

uniform vec3 uViewPos;
uniform sampler1D uColormapLUT;

out vec4 FragColor;

void main() {
    vec3 n = normalize(vNormal);
    vec3 L = normalize(uLightDir_ColorGB.xyz);
    float diff = abs(dot(n, L));
    vec3 color = vec3(uColorB_UseColormap.x, uColorB_UseColormap.y, uLightDir_ColorGB.w);
    bool useColormap = uColorB_UseColormap.y > 0.5;
    vec3 base = useColormap ? texture(uColormapLUT, vMag).rgb : color;
    vec3 col = base * (0.35 + 0.65 * diff);
    FragColor = vec4(col, 1.0);
}
