#version 460 core

layout(location = 0) in vec3 aPos;

uniform mat4 uMVP;

out vec3 vWorldPos;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vWorldPos = aPos;
}
