#version 460 core

in float vMag;
in float vT;

uniform sampler1D uColormapLUT;
uniform vec4 uColor;
uniform int uUseColormap;
uniform vec2 uParticleMagRange; // x=min, y=max

out vec4 FragColor;

void main() {
    vec2 coord = gl_PointCoord * 2.0 - 1.0;
    float r = dot(coord, coord);
    if (r > 1.0) discard;

    // [V1] Soft luminous sprite: hot core + halo falloff, analytic AA edge
    // instead of a hard discard cut.
    float core = exp(-r * 4.0);
    float halo = 1.0 - r;
    float alpha = (core * 0.75 + halo * 0.25) * smoothstep(1.0, 0.8, r);
    // Fade near both path ends so the wrap-around teleport is imperceptible.
    alpha *= smoothstep(0.0, 0.05, vT) * (1.0 - smoothstep(0.95, 1.0, vT));

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
