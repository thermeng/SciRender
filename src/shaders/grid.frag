#version 460 core

layout(depth_any) out float gl_FragDepth;

in vec3 vNear;
in vec3 vFar;

layout(std140) uniform GridUBO {
    mat4  uInvView;
    mat4  uInvProj;
    mat4  uView;
    mat4  uProj;
    vec4  uCamPos_Color;
    vec4  uColorBg_Falloff;
    vec4  uGridAxis_planePos;
    vec4  uFlags;
    mat4  uLightMVP;
    vec4  uShadowParams;
};

uniform sampler2D uShadowMap;

out vec4 fragColor;

float gridFactor(vec2 coord, float scale) {
    vec2 c = coord / scale;
    vec2 d = fwidth(c);
    vec2 drawWidth = clamp(d, 0.0001, 0.5);
    vec2 g = abs(fract(c - 0.5) - 0.5) / drawWidth;
    float line = min(g.x, g.y);
    float maxDerivative = max(d.x, d.y);
    float subpixelFade = 1.0 - smoothstep(0.2, 1.0, maxDerivative);
    return (1.0 - min(line, 1.0)) * subpixelFade;
}

void main() {
    vec3 rayDir = normalize(vFar - vNear);
    int axis = int(uGridAxis_planePos.x + 0.5);
    float planePos = uGridAxis_planePos.y;
    float t;
    vec3 worldPos;
    vec2 gridCoord;
    if (axis == 0) {
        t = (planePos - uCamPos_Color.x) / rayDir.x;
        worldPos = uCamPos_Color.xyz + t * rayDir;
        gridCoord = worldPos.yz;
    } else if (axis == 1) {
        t = (planePos - uCamPos_Color.y) / rayDir.y;
        worldPos = uCamPos_Color.xyz + t * rayDir;
        gridCoord = worldPos.xz;
    } else {
        t = (planePos - uCamPos_Color.z) / rayDir.z;
        worldPos = uCamPos_Color.xyz + t * rayDir;
        gridCoord = worldPos.xy;
    }
    if (t <= 0.0 || isnan(t) || isinf(t) || t > 1e6) discard;

    float dist = length(worldPos - uCamPos_Color.xyz);
    float fade = exp(-dist * uColorBg_Falloff.w);

    float minor = gridFactor(gridCoord, 1.0);
    float major = gridFactor(gridCoord, 10.0);
    float gridLine = max(minor * 0.4, major) * fade;

    float shadowFactor = 1.0;
    if (uShadowParams.x > 0.5) {
        vec4 sc = uLightMVP * vec4(worldPos, 1.0);
        sc.xyz /= sc.w;
        sc.xy = sc.xy * 0.5 + 0.5;
        if (sc.x >= 0.0 && sc.x <= 1.0 && sc.y >= 0.0 && sc.y <= 1.0 && sc.z >= 0.0 && sc.z <= 1.0) {
            float sd = texture(uShadowMap, sc.xy).r;
            shadowFactor = (sc.z - uShadowParams.y > sd) ? 0.25 : 1.0;
        }
    }

    vec3 groundBase = uColorBg_Falloff.xyz * 0.45 * shadowFactor;
    vec3 finalColor = mix(groundBase, uColorBg_Falloff.xyz, gridLine);

    fragColor = vec4(finalColor, 1.0);

    vec4 clip = uProj * uView * vec4(worldPos, 1.0);
    float ndcDepth = clip.z / clip.w;
    float depth = ndcDepth * 0.5 + 0.5;
    gl_FragDepth = clamp(depth, 0.0, 1.0);
}