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
    vec4  uPlaneY_Pad;
    vec4  uFlags;
};

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
    float planeY = uPlaneY_Pad.x;
    float t = (planeY - uCamPos_Color.xyz.y) / rayDir.y;
    if (t <= 0.0 || isnan(t) || isinf(t) || t > 1e6) discard;

    vec3 worldPos = uCamPos_Color.xyz + t * rayDir;
    float dist = length(worldPos - uCamPos_Color.xyz);
    float fade = exp(-dist * uColorBg_Falloff.w);

    float minor = gridFactor(worldPos.xz, 1.0);
    float major = gridFactor(worldPos.xz, 10.0);
    float g = max(minor * 0.4, major);

    float alpha = g * fade;
    if (alpha < 0.005) discard;

    fragColor = vec4(uColorBg_Falloff.xyz, alpha);

    vec4 clip = uProj * uView * vec4(worldPos, 1.0);
    float ndcDepth = clip.z / clip.w;
    if (uFlags.x < 0.5) {
        ndcDepth = ndcDepth * 0.5 + 0.5;
    }
    gl_FragDepth = clamp(ndcDepth, 0.0, 1.0);
}