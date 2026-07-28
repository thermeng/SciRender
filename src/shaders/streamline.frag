#version 460 core

in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vTexcoord;
in float vMag;

layout(std140, binding = 0) uniform StreamlineUBO {
    mat4  uMVP;
    mat4  uModel;
    vec4  uViewPos;
    vec4  uLightDir;
    vec4  uTime_Opacity;       // x: time, y: opacity
    vec4  uColor_UseColormap;  // xyz: fallback color, w: use colormap flag (0.0 or 1.0)
    vec4  uMagRange;           // x: minMag, y: maxMag
    vec4  uMaterial;           // x: ambient, y: diffuse, z: specular, w: specularPower
    vec4  uRibbon;             // x: ribbonWidth, y: taperFactor, zw: pad
};

uniform sampler1D uColormapLUT;

out vec4 FragColor;

void main() {
    // 1. Color evaluation
    bool useColormap = uColor_UseColormap.w > 0.5;
    float magMin = uMagRange.x;
    float magMax = uMagRange.y;
    float span = max(magMax - magMin, 1e-6);
    float normMag = clamp((vMag - magMin) / span, 0.0, 1.0);
    
    vec3 baseColor = useColormap ? texture(uColormapLUT, normMag).rgb : uColor_UseColormap.xyz;

    // 2. Lighting setup
    vec3 N = normalize(vNormal);
    vec3 L = normalize(uLightDir.xyz);
    vec3 V = normalize(uViewPos.xyz - vWorldPos);

    // Double-sided lighting fix for thin ribbon quads
    float diff = abs(dot(N, L)); 
    vec3 R = reflect(-L, N);
    float spec = pow(max(dot(R, V), 0.0), uMaterial.w);

    float ambient = uMaterial.x;
    float diffuse = uMaterial.y * diff;
    float specular = uMaterial.z * spec;

    // Dashing/Striping control
    float dash = 1.0;
    if (uRibbon.z > 0.5) {
        dash = step(0.5, fract(vTexcoord.x * 20.0 - uTime_Opacity.x * uRibbon.w * 1.5));
    }

    float alpha = mix(1.0, dash, 0.5) * uTime_Opacity.y;
    vec3 color = baseColor * (ambient + diffuse) + vec3(1.0) * specular;

    FragColor = vec4(color, alpha);
}