#version 460 core

in vec2 vUV;
uniform sampler2D uLayer0;
uniform sampler2D uLayer1;
out vec4 FragColor;

void main() {
    ivec2 pix = ivec2(gl_FragCoord.xy);
    vec4 back  = texelFetch(uLayer1, pix, 0);
    vec4 front = texelFetch(uLayer0, pix, 0);

    // Non-premultiplied back-to-front compositing
    float outA = front.a + back.a * (1.0 - front.a);
    vec3 outRGB;
    if (outA > 0.0001)
        outRGB = (front.rgb * front.a + back.rgb * back.a * (1.0 - front.a)) / outA;
    else
        outRGB = vec3(0.0);
    FragColor = vec4(outRGB, outA);
}
