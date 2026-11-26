#version 460 core

in vec2 vUV;
out float FragColor;

uniform sampler2D uFlowTex;   // RGBA16F: flowDir.xy (screen px normalized), valid, speedNorm
uniform sampler2D uNoiseTex;  // R8
uniform vec2 uTexelSize;      // 1/viewport
uniform int uSteps;           // half-kernel taps per direction
uniform float uStepSize;      // pixels per step
uniform float uNoiseFreq;     // noise scale
uniform int uEnhanced;        // 0/1

float noiseAt(vec2 uv, float freqScale) {
    float n = texture(uNoiseTex, uv * uNoiseFreq * freqScale).r;
    return n < 0.0 ? 0.5 : n;
}

vec2 flowDir(vec4 s) {
    if (s.z <= 0.5) return vec2(0.0);
    vec2 f = s.xy;
    float l2 = dot(f, f);
    if (!(l2 > 1e-12)) return vec2(0.0);
    return f * inversesqrt(l2);
}

float traceDir(vec2 startUV, vec2 dir, vec4 startS, int steps, float stepPx, float freqScale) {
    float acc = 0.5 * noiseAt(startUV, freqScale);
    float w = 0.5;
    vec2 uv = startUV;
    vec4 s = startS;
    for (int i = 0; i < steps; ++i) {
        vec2 f = flowDir(s);
        if (dot(f, f) < 1e-12) break;
        vec2 stepDir = (dot(f, dir) >= 0.0) ? f : -f;
        uv += stepDir * (stepPx * uTexelSize);
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) break;
        s = texture(uFlowTex, uv);
        if (s.z <= 0.5) break;
        acc += noiseAt(uv, freqScale);
        w += 1.0;
    }
    return acc / max(w, 0.5);
}

vec2 traceDir2(vec2 startUV, vec2 dir, vec4 startS, int steps, float stepPx) {
    float n1 = noiseAt(startUV, 1.0);
    float n2 = noiseAt(startUV, 1.35);
    float acc1 = 0.5 * n1, acc2 = 0.5 * n2;
    float w = 0.5;
    vec2 uv = startUV;
    vec4 s = startS;
    for (int i = 0; i < steps; ++i) {
        vec2 f = flowDir(s);
        if (dot(f, f) < 1e-12) break;
        vec2 stepDir = (dot(f, dir) >= 0.0) ? f : -f;
        uv += stepDir * (stepPx * uTexelSize);
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) break;
        s = texture(uFlowTex, uv);
        if (s.z <= 0.5) break;
        acc1 += noiseAt(uv, 1.0);
        acc2 += noiseAt(uv, 1.35);
        w += 1.0;
    }
    float invW = 1.0 / max(w, 0.5);
    return vec2(acc1 * invW, acc2 * invW);
}

float licGrayAt(vec2 uv, int steps, float stepPx) {
    vec4 s = texture(uFlowTex, uv);
    if (s.z <= 0.5) return -1.0; // invalid
    vec2 dir = flowDir(s);
    if (dot(dir, dir) < 1e-12) return clamp((noiseAt(uv, 1.0) - 0.5) * 1.8 + 0.5, 0.0, 1.0);

    if (uEnhanced != 0) {
        vec2 fwd = traceDir2(uv, dir, s, steps, stepPx);
        vec2 bwd = traceDir2(uv, -dir, s, steps, stepPx);
        float g1 = (fwd.x + bwd.x) * 0.5;
        float g2 = (fwd.y + bwd.y) * 0.5;
        float fused = g1 + 0.55 * (g1 - g2);
        return clamp((fused - 0.5) * 2.15 + 0.5, 0.0, 1.0);
    }
    float fwd = traceDir(uv, dir, s, steps, stepPx, 1.0);
    float bwd = traceDir(uv, -dir, s, steps, stepPx, 1.0);
    float g = (fwd + bwd) * 0.5;
    return clamp((g - 0.5) * 1.8 + 0.5, 0.0, 1.0);
}

void main() {
    int steps = clamp(uSteps, 0, 128);
    float stepPx = clamp(uStepSize, 0.001, 20.0);
    FragColor = licGrayAt(vUV, steps, stepPx);
}
