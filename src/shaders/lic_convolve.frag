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
    return texture(uNoiseTex, uv * uNoiseFreq * freqScale).r;
}

vec2 flowAt(vec2 uv) {
    vec4 s = texture(uFlowTex, uv);
    if (s.z <= 0.5) return vec2(0.0);
    vec2 f = s.xy;
    float l = length(f);
    if (!(l > 1e-6)) return vec2(0.0);
    return f / l;
}

float traceDir(vec2 startUV, vec2 dir, int steps, float stepPx, float freqScale) {
    float acc = 0.5 * noiseAt(startUV, freqScale);
    float w = 0.5;
    vec2 uv = startUV;
    for (int i = 0; i < steps; ++i) {
        vec2 f = flowAt(uv);
        if (dot(f, f) < 1e-6) break;
        // follow local flow but keep initial direction sign
        vec2 stepDir = (dot(f, dir) >= 0.0) ? f : -f;
        uv += stepDir * (stepPx * uTexelSize);
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) break;
        vec4 s = texture(uFlowTex, uv);
        if (s.z <= 0.5) break;
        acc += noiseAt(uv, freqScale);
        w += 1.0;
    }
    return acc / max(w, 0.5);
}

float licGrayAt(vec2 uv, int steps, float stepPx) {
    vec4 s = texture(uFlowTex, uv);
    if (s.z <= 0.5) return -1.0; // invalid
    vec2 dir = flowAt(uv);
    if (dot(dir, dir) < 1e-6) return clamp((noiseAt(uv, 1.0) - 0.5) * 1.8 + 0.5, 0.0, 1.0);

    if (uEnhanced != 0) {
        float f1 = traceDir(uv, dir, steps, stepPx, 1.0);
        float b1 = traceDir(uv, -dir, steps, stepPx, 1.0);
        float f2 = traceDir(uv, dir, steps, stepPx, 1.35);
        float b2 = traceDir(uv, -dir, steps, stepPx, 1.35);
        float g1 = (f1 + b1) * 0.5;
        float g2 = (f2 + b2) * 0.5;
        float fused = g1 + 0.55 * (g1 - g2);
        return clamp((fused - 0.5) * 2.15 + 0.5, 0.0, 1.0);
    }
    float fwd = traceDir(uv, dir, steps, stepPx, 1.0);
    float bwd = traceDir(uv, -dir, steps, stepPx, 1.0);
    float g = (fwd + bwd) * 0.5;
    return clamp((g - 0.5) * 1.8 + 0.5, 0.0, 1.0);
}

void main() {
    int steps = clamp(uSteps, 0, 128);
    float stepPx = clamp(uStepSize, 0.001, 20.0);
    float g = licGrayAt(vUV, steps, stepPx);
    FragColor = g; // negative means invalid/no LIC
}
