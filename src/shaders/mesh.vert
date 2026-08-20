#version 460 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in float aScalar;

layout(std140) uniform MeshUBO {
    mat4  uMVP;
    mat4  uModel;
    vec4  uViewPos_PS;      // xyz = viewPos, w = pointSize
    vec4  uMeshColor_Wire;  // xyz = meshColor, w = wireframe
    vec4  uSurfaceColor_Op; // xyz = surfaceColor, w = surfaceOpacity
    vec4  uPointClip;       // x = isPoint, y = pointUseScalar, z = pointOpacity, w = clipEnabled
    vec4  uLightDir;
    vec4  uLightFill;
    vec4  uLightBack1;
    vec4  uLightBack2;
    vec4  uLightHead;
    vec4  uKeyColor;
    vec4  uFillColor;
    vec4  uBackColor;
    vec4  uHeadColor;
    vec4  uScalars;         // x = scalarMin, y = scalarMax, z = hasScalars(0/1), w = 0
    vec4  uSliceY;          // x = sliceHeightX, y = sliceHeightY, z = sliceHeightZ, w = 0
    vec4  uSliceEn;         // x = sliceEnabledX, y = sliceEnabledY, z = sliceEnabledZ, w = 0
    vec4  uInvert;          // x = invertX, y = invertY, z = invertZ, w = 0
    vec4  uFilter;          // x = filterMin, y = filterMax, z = 0, w = 0
    vec4  uMaterial;        // x = matAmbient, y = matDiffuse, z = matSpecular
    vec4  uIntensities;     // x = keyIntensity, y = fillIntensity, z = backIntensity, w = headIntensity
    vec4  uPBR;             // x = matRoughness, y = matMetallic, z = pad, w = pad
    vec4  uShadingMode;     // x = 0.0 smooth, 1.0 flat; y = 0.0 normal clip, 1.0 crinkle clip
};

out MeshVarying {
    vec3 vWorldPos;
    vec3 vNormal;
    float vScalar;
} mv;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    gl_PointSize = uViewPos_PS.w;
    mv.vWorldPos = vec3(uModel * vec4(aPos, 1.0));
    mv.vNormal = aNormal;
    mv.vScalar = aScalar;
}
