#version 460 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in float aMag;

layout(std140, binding = 3) uniform StreamlineUBO {
    mat4  uMVP;
    mat4  uModel;
    vec4  uViewPos;
    vec4  uLightDir;
    vec4  uTime_Opacity;
    vec4  uColor_UseColormap;
    vec4  uMagRange;
    vec4  uMaterial;
    vec4  uRibbon;
    vec4  uArrowParams;
};

uniform float uPointSize;

out float vMag;

void main() {
    vMag = aMag;
    gl_Position = uMVP * vec4(aPos, 1.0);
    gl_PointSize = uPointSize;
}
