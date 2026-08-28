#version 460 core

layout(location = 0) in vec3 aPos;

uniform mat4 uMVP;
uniform int uAxis;

out vec3 vWorldPos;

const float kDepthBias = 1e-4;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    gl_Position.z += kDepthBias * float(uAxis + 1);
    vWorldPos = aPos;
}
