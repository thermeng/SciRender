#version 460 core
layout(location = 0) in vec3 aPos;
layout(std140) uniform ShadowUBO {
    mat4 uLightMVP;
};
void main() {
    gl_Position = uLightMVP * vec4(aPos, 1.0);
}
