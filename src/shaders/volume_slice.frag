#version 460 core

in vec3 vWorldPos;

uniform sampler3D uVolumeTex;
uniform sampler3D uOccupancyTex;
uniform int uHasOccupancy = 0; // 1 = discard samples outside the mesh
uniform sampler1D uColormapLUT;
uniform float uNumBands = 0.0; // 0 = continuous, >1 = discrete bands
uniform vec3 uBoxMin;
uniform vec3 uBoxMax;
uniform float uScalarMin;
uniform float uScalarMax;
uniform int uUseColormap;
uniform float uAlpha;

out vec4 FragColor;

void main() {
    vec3 uvw = (vWorldPos - uBoxMin) / (uBoxMax - uBoxMin);
    // Unstructured meshes: the slice quad spans the mesh AABB, so discard where
    // no cell exists instead of showing a blank rectangle.
    
    if (uHasOccupancy == 1 && texture(uOccupancyTex, uvw).r == 0.0) discard;
    float val = texture(uVolumeTex, uvw).r;

    float scalarRange = max(uScalarMax - uScalarMin, 1e-6);
    float tVal = clamp((val - uScalarMin) / scalarRange, 0.0, 1.0);
    if (uNumBands > 1.0) {
        tVal = floor(tVal * uNumBands) / (uNumBands - 1.0);
    }

    vec3 color;
    if (uUseColormap == 1) {
        color = texture(uColormapLUT, tVal).rgb;
    } else {
        color = vec3(tVal);
    }

    FragColor = vec4(color, uAlpha);
}
