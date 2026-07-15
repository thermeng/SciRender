#version 330 core
// procedural infinite ground grid via screen-space ray cast to y=0.
// Grid lines are drawn in world space with fwidth() AA so line width stays
// constant regardless of distance. Depth is written manually so the grid
// occludes/is-occluded by meshes correctly. Fades to background at the horizon.
in vec3 vNear;
in vec3 vFar;

uniform mat4  uView;
uniform mat4  uProj;
uniform vec3  uCamPos;
uniform vec3  uColor;
uniform vec3  uBg;
uniform float uFalloff; // exp(-dist*falloff) horizon fade rate
uniform float uPlaneY;  // ground-plane height (mesh y-min), default 0

out vec4 fragColor;

// AA line factor for a grid of given world-space scale.
float gridFactor(vec2 coord, float scale) {
    vec2 c = coord / scale;
    vec2 d = fwidth(c);
    vec2 g = abs(fract(c - 0.5) - 0.5) / d;
    float line = min(g.x, g.y);
    return 1.0 - min(line, 1.0);
}

void main() {
    vec3 rayDir = normalize(vFar - vNear);
    if (abs(rayDir.y) < 1e-5) discard;       // looking along the plane: no hit
    float t = (uPlaneY - vNear.y) / rayDir.y;
    if (t < 0.0) discard;                    // plane behind the camera
    vec3 worldPos = vNear + t * rayDir;

    float dist = length(worldPos - uCamPos);
    float fade = exp(-dist * uFalloff);

    float minor = gridFactor(worldPos.xz, 1.0);
    float major = gridFactor(worldPos.xz, 10.0);
    float g = max(minor * 0.5, major);
    float alpha = g * fade;
    if (alpha < 0.01) discard;

    vec3 col = mix(uBg, uColor, g);
    fragColor = vec4(col, alpha);

    // write true depth so the grid sits correctly in the depth buffer.
    // clip.z/clip.w is NDC in [-1,1]; gl_FragDepth needs window depth [0,1].
    vec4 clip = uProj * uView * vec4(worldPos, 1.0);
    gl_FragDepth = 0.5 * (clip.z / clip.w) + 0.5;
}
