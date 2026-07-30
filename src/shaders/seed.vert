#version 460 core
layout(location = 0) in vec3 aPos;
uniform mat4 uMVP;
uniform mat4 uModel;
uniform vec4 uColor;
uniform float uPointSize;
out vec3 vWorldPos;
void main() {
    vWorldPos = vec3(uModel * vec4(aPos, 1.0));
    gl_Position = uMVP * vec4(aPos, 1.0);
    gl_PointSize = uPointSize;
}
