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
    vec4  uTime_Opacity;
    vec4  uColor_UseColormap;
    vec4  uMagRange;
};

uniform sampler1D uColormapLUT;

out vec4 FragColor;

void main() {
    bool useColormap = uColor_UseColormap.w > 0.5;
    float magMin = uMagRange.x;
    float magMax = uMagRange.y;
    float span = max(magMax - magMin, 1e-6);
    float normMag = clamp((vMag - magMin) / span, 0.0, 1.0);
    vec3 baseColor = useColormap ? texture(uColormapLUT, normMag).rgb : uColor_UseColormap.xyz;

    vec3 N = normalize(vNormal);
    vec3 L = normalize(uLightDir.xyz);
    vec3 V = normalize(uViewPos.xyz - vWorldPos);
    vec3 R = reflect(-L, N);

    float diff = max(dot(N, L), 0.0);
    float spec = pow(max(dot(R, V), 0.0), 32.0);

    float ambient = 0.35;
    float diffuse = 0.55 * diff;
    float specular = 0.25 * spec;

    float dash = step(0.5, fract(vTexcoord.x * 8.0 - uTime_Opacity.x * 1.5));
    float alpha = mix(1.0, dash, 0.5) * uTime_Opacity.y;

    vec3 color = baseColor * (ambient + diffuse) + vec3(1.0) * specular;
    FragColor = vec4(color, alpha);
}