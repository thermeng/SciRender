#version 460 core

in vec3 vWorldPos;

uniform vec4 uColor;
uniform vec4 uLightDir;

out vec4 FragColor;

void main() {
    vec2 pc = gl_PointCoord * 2.0 - 1.0;
    float r2 = dot(pc, pc);
    if (r2 > 1.0) discard;

    vec3 normal = vec3(pc, sqrt(1.0 - r2));

    vec3 lightDir = normalize(uLightDir.xyz);
    vec3 viewDir  = vec3(0.0, 0.0, 1.0);
    vec3 halfVec  = normalize(lightDir + viewDir);

    float diff = max(dot(normal, lightDir), 0.0);
    float spec = pow(max(dot(normal, halfVec), 0.0), 32.0);

    vec3 ambient  = uColor.rgb * 0.25;
    vec3 diffuse  = uColor.rgb * diff * 0.60;
    vec3 specular = vec3(1.0) * spec * 0.15;

    FragColor = vec4(ambient + diffuse + specular, uColor.a);
}
