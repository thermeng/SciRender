#version 460 core

in vec3 vWorldPos;

uniform sampler3D uVolumeTex;
uniform sampler1D uColormapLUT;
uniform vec3 uBoxMin;
uniform vec3 uBoxMax;
uniform float uScalarMin;
uniform float uScalarMax;
uniform int uUseColormap;
uniform float uAlpha;

out vec4 FragColor;

void main() {
    vec3 uvw = (vWorldPos - uBoxMin) / (uBoxMax - uBoxMin);
    float val = texture(uVolumeTex, uvw).r;

    float scalarRange = max(uScalarMax - uScalarMin, 1e-6);
    float tVal = clamp((val - uScalarMin) / scalarRange, 0.0, 1.0);

    vec3 color;
    if (uUseColormap == 1) {
        color = texture(uColormapLUT, tVal).rgb;
    } else {
        color = vec3(tVal);
    }

    FragColor = vec4(color, uAlpha);
}
