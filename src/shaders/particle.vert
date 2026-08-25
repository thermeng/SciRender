#version 460 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in float aMag;
layout(location = 2) in float aT;

uniform mat4 uMVP;
uniform float uPointSize;
uniform float uSizeRefW;   // camera->focal distance (1.0 for ortho)

out float vMag;
out float vT;

void main() {
    vMag = aMag;
    vT = aT;
    gl_Position = uMVP * vec4(aPos, 1.0);
    // [V2] Perspective attenuation: slider value = px at the focal plane.
    // Ortho passes uSizeRefW = 1 so the slider is the exact pixel size.
    float sizePx = uPointSize * uSizeRefW / max(gl_Position.w, 1e-6);
    gl_PointSize = clamp(sizePx, uPointSize * 0.25, uPointSize * 4.0);
}
