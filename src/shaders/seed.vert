#version 460 core
layout(location = 0) in vec3 aPos;
uniform mat4 uMVP;
uniform vec4 uColor;
uniform float uPointSize;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    gl_PointSize = uPointSize;
}
