#version 460 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in float aMag;
layout(location = 2) in vec3 aComp;
layout(location = 3) in vec3 aNormal;

layout(std140, binding = 3) uniform StreamlineUBO {
    mat4  uMVP;
    mat4  uModel;
    vec4  uViewPos;           // xyz = viewPos
    vec4  uLightDir;          // xyz = lightDir
    vec4  uTime_Opacity;      // x = uTime, y = opacity
    vec4  uColor_UseColormap; // xyz = color, w = useColormap(0/1)
    vec4  uMagRange;          // x = magMin, y = magMax, zw = pad
    vec4  uCompMin;           // xyz = compMin X,Y,Z, w = pad
    vec4  uCompMax;           // xyz = compMax X,Y,Z, w = pad
    vec4  uColorMode;         // x = colorMode(0-4), yzw = pad
    vec4  uMaterial;          // x = ambient, y = diffuse, z = specular, w = specularPower
    vec4  uRibbon;            // x = ribbonWidth, y = taperFactor, zw = pad
    vec4  uArrowParams;       // x = arrowAnimSpeed, yzw = pad
    vec4  uPBR;               // x = matRoughness, y = matMetallic, zw = pad
};
out vec3 vWorldPos;
out vec3 vNormal;
out float vMag;
out float vColorScalar;

void main() {
    vWorldPos = vec3(uModel * vec4(aPos, 1.0));
    vNormal = normalize(mat3(uModel) * aNormal);
    vMag = aMag;

    float magMin = uMagRange.x;
    float magMax = uMagRange.y;
    float span = max(magMax - magMin, 1e-6);
    float normMag = clamp((aMag - magMin) / span, 0.0, 1.0);

    vec3 cMin = uCompMin.xyz;
    vec3 cMax = uCompMax.xyz;
    vec3 cSpan = max(cMax - cMin, vec3(1e-6));
    vec3 vComp = clamp((aComp - cMin) / cSpan, 0.0, 1.0);

    int colorMode = int(uColorMode.x);
    if (colorMode == 1)      vColorScalar = normMag;
    else if (colorMode == 2) vColorScalar = vComp.x;
    else if (colorMode == 3) vColorScalar = vComp.y;
    else if (colorMode == 4) vColorScalar = vComp.z;
    else                     vColorScalar = 0.0;

    gl_Position = uMVP * vec4(aPos, 1.0);
}
