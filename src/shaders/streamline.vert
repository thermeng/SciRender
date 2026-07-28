#version 460 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in float aMag;
layout(location = 2) in vec3 aNormal;
layout(location = 3) in float aDashFlag;
layout(location = 4) in float aU;

layout(std140, binding = 0) uniform StreamlineUBO {
    mat4  uMVP;
    mat4  uModel;
    vec4  uViewPos;           // xyz = viewPos
    vec4  uLightDir;          // xyz = lightDir
    vec4  uTime_Opacity;      // x = uTime, y = opacity
    vec4  uColor_UseColormap; // xyz = color, w = useColormap(0/1)
    vec4  uMagRange;          // x = magMin, y = maxMag, zw = pad
    vec4  uMaterial;          // x = ambient, y = diffuse, z = specular, w = specularPower
    vec4  uRibbon;            // x = ribbonWidth, y = taperFactor, zw = pad
};

out vec3 vWorldPos;
out vec3 vNormal;
out float vDashFlag;
out float vU;
out float vMag;

void main() {
    vWorldPos = vec3(uModel * vec4(aPos, 1.0));
    vNormal = normalize(aNormal);
    vDashFlag = aDashFlag;
    vU = aU;
    vMag = aMag;
    gl_Position = uMVP * vec4(aPos, 1.0);
}