#version 330 core
in vec3 vNormal;
in vec3 vWorldPos;
in float vMag;

uniform vec3 uViewPos;
uniform vec3 uLightDir;       // key light direction (world space), matches mesh shader
uniform vec3 uColor;          // flat arrow color when colormap disabled
uniform bool uUseColormap;    // color arrows by magnitude via shared LUT
uniform sampler1D uColormapLUT;

out vec4 FragColor;

void main() {
    vec3 n = normalize(vNormal);
    vec3 L = normalize(uLightDir);
    // Two-sided: light arrow from both faces so backfaces aren't black.
    float diff = abs(dot(n, L));
    vec3 base = uUseColormap ? texture(uColormapLUT, vMag).rgb : uColor;
    vec3 col = base * (0.35 + 0.65 * diff);
    FragColor = vec4(col, 1.0);
}
