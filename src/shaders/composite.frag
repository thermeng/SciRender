#version 460 core

in vec2 vUV;
uniform sampler2D uLayers[8];
uniform int uNumLayers;
out vec4 FragColor;

void main() {
    ivec2 pix = ivec2(gl_FragCoord.xy);

    // Back-to-front compositing: iterate from the backmost layer (highest index)
    // to the frontmost (index 0), accumulating alpha and color.
    vec4 acc = vec4(0.0, 0.0, 0.0, 0.0);

    for (int i = uNumLayers - 1; i >= 0; --i) {
        vec4 layer = texelFetch(uLayers[i], pix, 0);
        float outA = layer.a + acc.a * (1.0 - layer.a);
        vec3 outRGB;
        if (outA > 0.0001)
            outRGB = (layer.rgb * layer.a + acc.rgb * acc.a * (1.0 - layer.a)) / outA;
        else
            outRGB = vec3(0.0);
        acc = vec4(outRGB, outA);
    }

    // Convert to premultiplied alpha for GL_ONE / GL_ONE_MINUS_SRC_ALPHA blend.
    FragColor = vec4(acc.rgb * acc.a, acc.a);
}
