#version 460 core

in vec2 vUV;
uniform sampler2D uLayers[32];
uniform int uNumLayers;
out vec4 FragColor;

void main() {
    ivec2 pix = ivec2(gl_FragCoord.xy);

    // Back-to-front compositing in premultiplied space (direct, no divide).
    // acc is premultiplied at all times; final acc is ready for
    // GL_ONE/GL_ONE_MINUS_SRC_ALPHA blend onto the opaque framebuffer.
    vec4 acc = vec4(0.0);
    for (int i = uNumLayers - 1; i >= 0; --i) {
        vec4 layer = texelFetch(uLayers[i], pix, 0);
        vec3 layerPremul = layer.rgb * layer.a;
        acc.rgb = layerPremul + acc.rgb * (1.0 - layer.a);
        acc.a   = layer.a + acc.a * (1.0 - layer.a);
    }
    FragColor = acc;
}
