#version 460 core
// Wireframe geometry stage: expand each GL_LINES segment into a screen-space
// camera-facing quad of 2*uHalfWidth device pixels, carrying the signed pixel
// distance to the centerline for fragment AA.
//
// glLineWidth() is not a viable thickness mechanism here: the core profile
// only guarantees support for width 1.0 and common Windows drivers clamp
// higher values, so the wireframe thickness setting silently did nothing.
// Expanding in the GS makes thickness driver-independent.

layout(lines) in;
layout(triangle_strip, max_vertices = 4) out;

uniform vec2  uViewport;   // drawable size in device pixels
uniform float uHalfWidth;  // half line width in device pixels

noperspective out float vDist;
noperspective out float vHalfW;

void emitCorner(vec2 screenPx, float w, float zNdc, float dist, float hw) {
    vec4 c;
    c.x = (screenPx.x / uViewport.x * 2.0 - 1.0) * w;
    c.y = (screenPx.y / uViewport.y * 2.0 - 1.0) * w;
    c.z = zNdc * w;
    c.w = w;
    gl_Position = c;
    vDist = dist;
    vHalfW = hw;
    EmitVertex();
}

void main() {
    vec4 ca = gl_in[0].gl_Position;
    vec4 cb = gl_in[1].gl_Position;

    // Near-plane clip so perspective segments crossing w<=0 don't blow up.
    if (ca.w <= 0.0 && cb.w <= 0.0) return;
    if (ca.w <= 0.0 || cb.w <= 0.0) {
        float t = (1e-3 - ca.w) / (cb.w - ca.w);
        vec4 c = mix(ca, cb, clamp(t, 0.0, 1.0));
        if (ca.w <= 0.0) ca = c; else cb = c;
    }

    vec3 na = ca.xyz / ca.w;
    vec3 nb = cb.xyz / cb.w;
    vec2 sa = (na.xy * 0.5 + 0.5) * uViewport;
    vec2 sb = (nb.xy * 0.5 + 0.5) * uViewport;

    vec2 d = sb - sa;
    float len = length(d);
    if (len < 1e-4) return;  // degenerate: both endpoints project to one pixel
    vec2 nrm = vec2(-d.y, d.x) / len * uHalfWidth;

    float hw = uHalfWidth;
    emitCorner(sa - nrm, ca.w, na.z, -hw, hw);
    emitCorner(sb - nrm, cb.w, nb.z, -hw, hw);
    emitCorner(sa + nrm, ca.w, na.z,  hw, hw);
    emitCorner(sb + nrm, cb.w, nb.z,  hw, hw);
    EndPrimitive();
}
