#version 460 core
// Wireframe vertex stage: project the cell-edge endpoint; the GS does the
// screen-space expansion. Plain uniform (not the shared UBO): uniform block
// declarations must match exactly across all stages of a program, and this
// program deliberately declares none.
layout(location = 0) in vec3 aPos;

uniform mat4 uMVP;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
}
