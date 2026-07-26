#version 460 core
layout(location = 0) in vec2 aPos;

layout(std140) uniform GridUBO {
    mat4  uInvView;
    mat4  uInvProj;
    mat4  uView;
    mat4  uProj;
    vec4  uCamPos_Color;
    vec4  uColorBg_Falloff;
    vec4  uPlaneY_Pad;
};

out vec3 vNear;
out vec3 vFar;

void main() {
    vec4 nearH = uInvView * uInvProj * vec4(aPos, -1.0, 1.0);
    vec4 farH  = uInvView * uInvProj * vec4(aPos,  1.0, 1.0);
    vNear = nearH.xyz / nearH.w;
    vFar  = farH.xyz  / farH.w;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
