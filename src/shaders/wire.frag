#version 460 core
// Wireframe fragment: AA coverage from the screen-space distance interpolated
// by the GS, colored via the uWireColor uniform.
noperspective in float vDist;
noperspective in float vHalfW;
uniform vec4 uWireColor;

out vec4 frag;

void main() {
    float aa = fwidth(vDist);
    float alpha = 1.0 - smoothstep(vHalfW - aa, vHalfW + aa, abs(vDist));
    if (alpha < 0.004) discard;
    frag = vec4(uWireColor.rgb, alpha * uWireColor.a);
}
