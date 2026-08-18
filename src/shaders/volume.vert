#version 460 core

layout(location = 0) in vec2 aPos;

uniform mat4 uInvView;
uniform mat4 uInvProj;
uniform vec3 uCamPos;
uniform vec3 uForward;
uniform int uOrtho;

out vec3 vRayOrigin;
out vec3 vRayDir;

void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vRayOrigin = uCamPos;

    if (uOrtho == 1) {
        vec4 nearWorld = uInvView * uInvProj * vec4(aPos, -1.0, 1.0);
        nearWorld /= nearWorld.w;
        vRayOrigin = nearWorld.xyz;
        vRayDir = uForward;
    } else {
        vec4 viewFar = uInvProj * vec4(aPos, 1.0, 1.0);
        viewFar /= viewFar.w;
        vec4 worldFar = uInvView * vec4(viewFar.xyz, 1.0);
        worldFar /= worldFar.w;

        vRayDir = worldFar.xyz - uCamPos;
    }
}
