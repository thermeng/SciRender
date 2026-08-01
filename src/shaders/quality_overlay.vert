#version 460 core
layout(location = 0) in vec3 aPos;
uniform mat4 uMVP;
uniform float uDepthBias;
void main() {
    vec4 pos = uMVP * vec4(aPos, 1.0);
    pos.z += uDepthBias * pos.w;
    gl_Position = pos;
}
