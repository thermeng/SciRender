#version 460 core

layout(depth_unchanged) out float gl_FragDepth;

in vec3 vNear;
in vec3 vFar;

uniform mat4  uView;
uniform mat4  uProj;
uniform vec3  uCamPos;
uniform vec3  uColor;
uniform float uFalloff; // exp(-dist * falloff) horizon fade rate
uniform float uPlaneY;  // ground-plane height

out vec4 fragColor;

// AA line factor with sub-pixel moiré suppression
float gridFactor(vec2 coord, float scale) {
    vec2 c = coord / scale;
    vec2 d = fwidth(c);
    
    // Clamp derivative to prevent division-by-zero artifacts
    vec2 drawWidth = clamp(d, 0.0001, 0.5);
    vec2 g = abs(fract(c - 0.5) - 0.5) / drawWidth;
    float line = min(g.x, g.y);
    
    // Smoothly fade out lines when they become smaller than 1 screen pixel
    float maxDerivative = max(d.x, d.y);
    float subpixelFade = 1.0 - smoothstep(0.2, 1.0, maxDerivative);
    
    return (1.0 - min(line, 1.0)) * subpixelFade;
}

void main() {
    vec3 rayDir = normalize(vFar - vNear);
    
    // Raycast directly from camera position to plane y = uPlaneY
    float t = (uPlaneY - uCamPos.y) / rayDir.y;

    // Discard rays pointing away, parallel, or beyond reasonable bounds
    if (t <= 0.0 || isnan(t) || isinf(t) || t > 1e6) {
        discard;
    }

    vec3 worldPos = uCamPos + t * rayDir;

    // Exponential horizon fade
    float dist = length(worldPos - uCamPos);
    float fade = exp(-dist * uFalloff);

    // Compute grid pattern
    float minor = gridFactor(worldPos.xz, 1.0);
    float major = gridFactor(worldPos.xz, 10.0);
    float g = max(minor * 0.4, major);

    float alpha = g * fade;
    if (alpha < 0.005) discard;

    fragColor = vec4(uColor, alpha);

    // Calculate clip depth safely
    vec4 clip = uProj * uView * vec4(worldPos, 1.0);
    float ndcDepth = clip.z / clip.w;
    gl_FragDepth = clamp(0.5 * ndcDepth + 0.5, 0.0, 1.0);
}