#version 460 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in float aMag;
layout(location = 2) in float aT;
layout(location = 3) in vec3 aTangent;
layout(location = 4) in float aRadius;

uniform mat4 uMVP;
uniform float uPointSize;
uniform float uSizeRefW;   // camera->focal distance (1.0 for ortho)
uniform float uTubeBaseWidth;

out float vMag;
out float vT;

void main() {
    vMag = aMag;
    vT = aT;
    gl_Position = uMVP * vec4(aPos, 1.0);

    gl_PointSize = clamp(uPointSize, 1.0, 64.0);
}