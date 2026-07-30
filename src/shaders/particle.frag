#version 460 core

in float vMag;

uniform sampler1D uColormapLUT;
uniform vec4 uColor;
uniform int uUseColormap;
uniform vec2 uParticleMagRange; // x=min, y=max

out vec4 FragColor;

void main() {
    vec2 coord = gl_PointCoord * 2.0 - 1.0;
    float r = dot(coord, coord);
    if (r > 1.0) discard;

    float alpha = 1.0 - r * r;

    vec3 color;
    if (uUseColormap > 0) {
        float span = max(uParticleMagRange.y - uParticleMagRange.x, 1e-6);
        float norm = clamp((vMag - uParticleMagRange.x) / span, 0.0, 1.0);
        color = texture(uColormapLUT, norm).rgb;
    } else {
        color = uColor.rgb;
    }

    FragColor = vec4(color, alpha);
}
