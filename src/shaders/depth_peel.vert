#version 460 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in float aScalar;

layout(std140) uniform MeshUBO {
    mat4  uMVP;
    mat4  uModel;
    vec4  uViewPos_PS;
    vec4  uMeshColor_Wire;
    vec4  uSurfaceColor_Op;
    vec4  uPointClip;
    vec4  uLightDir;
    vec4  uLightFill;
    vec4  uLightBack1;
    vec4  uLightBack2;
    vec4  uLightHead;
    vec4  uKeyColor;
    vec4  uFillColor;
    vec4  uBackColor;
    vec4  uHeadColor;
    vec4  uScalars;
    vec4  uSliceY;
    vec4  uSliceEn;
    vec4  uInvert;
    vec4  uFilter;
    vec4  uMaterial;
    vec4  uIntensities;
    vec4  uPBR;             // x = matRoughness, y = matMetallic, z = pad, w = pad
};

out vec3 vNormal;
out vec3 vWorldPos;
out float vScalar;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vWorldPos = vec3(uModel * vec4(aPos, 1.0));
    vNormal = aNormal;
    vScalar = aScalar;
}
