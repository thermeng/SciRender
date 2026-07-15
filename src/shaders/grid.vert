#version 330 core
// full-screen quad proxy; NDC position flows straight to gl_Position.
// near/far world points are reconstructed via inverse view/proj so the fragment
// shader can cast a ray to the ground plane at height uPlaneY.
layout(location = 0) in vec2 aPos;

uniform mat4 uInvView;
uniform mat4 uInvProj;

out vec3 vNear;
out vec3 vFar;

void main() {
    vec4 nearH = uInvView * uInvProj * vec4(aPos, -1.0, 1.0);
    vec4 farH  = uInvView * uInvProj * vec4(aPos,  1.0, 1.0);
    vNear = nearH.xyz / nearH.w;
    vFar  = farH.xyz  / farH.w;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
