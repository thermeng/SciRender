#version 460 core

in vec3 vRayOrigin;
in vec3 vRayDir;

uniform sampler3D uVolumeTex;
uniform sampler1D uColormapLUT;
uniform vec3 uBoxMin;
uniform vec3 uBoxMax;
uniform float uStepSize;
uniform float uOpacity;
uniform float uScalarMin;
uniform float uScalarMax;
uniform int uVolumeUseColormap;
uniform int uClipEnabled;
uniform float uSliceHeightX;
uniform float uSliceHeightY;
uniform float uSliceHeightZ;
uniform vec3 uSliceEn;
uniform vec3 uInvert;

#define MAX_STEPS 512

out vec4 FragColor;

vec3 computeGradient(vec3 uvw) {
    vec3 epsilon = vec3(1.0 / 128.0, 1.0 / 128.0, 1.0 / 128.0);
    float gx = texture(uVolumeTex, uvw + vec3(epsilon.x, 0.0, 0.0)).r - 
               texture(uVolumeTex, uvw - vec3(epsilon.x, 0.0, 0.0)).r;
    float gy = texture(uVolumeTex, uvw + vec3(0.0, epsilon.y, 0.0)).r - 
               texture(uVolumeTex, uvw - vec3(0.0, epsilon.y, 0.0)).r;
    float gz = texture(uVolumeTex, uvw + vec3(0.0, 0.0, epsilon.z)).r - 
               texture(uVolumeTex, uvw - vec3(0.0, 0.0, epsilon.z)).r;
    return vec3(gx, gy, gz);
}

float intersectBox(vec3 rayOrigin, vec3 rayDir, vec3 boxMin, vec3 boxMax, out float tFar) {
    vec3 safeDir = rayDir;
    if (abs(rayDir.x) < 1e-8) safeDir.x = (rayDir.x >= 0.0 ? 1.0 : -1.0) * 1e-8;
    if (abs(rayDir.y) < 1e-8) safeDir.y = (rayDir.y >= 0.0 ? 1.0 : -1.0) * 1e-8;
    if (abs(rayDir.z) < 1e-8) safeDir.z = (rayDir.z >= 0.0 ? 1.0 : -1.0) * 1e-8;
    vec3 invDir = 1.0 / safeDir;
    vec3 t0s = (boxMin - rayOrigin) * invDir;
    vec3 t1s = (boxMax - rayOrigin) * invDir;
    vec3 tsmaller = min(t0s, t1s);
    vec3 tbigger = max(t0s, t1s);
    float tNear = max(max(tsmaller.x, tsmaller.y), tsmaller.z);
    tFar = min(min(tbigger.x, tbigger.y), tbigger.z);
    if (tNear > tFar || tFar < 0.0) return -1.0;
    return max(tNear, 0.0);
}

float computeSliceAlpha(vec3 pos) {
    float alpha = 1.0;
    if (uClipEnabled == 1) {
        if (uSliceEn.x > 0.5) {
            float sliceX = mix(uBoxMin.x, uBoxMax.x, uSliceHeightX);
            bool invert = uInvert.x > 0.5;
            bool inside = invert ? (pos.x < sliceX) : (pos.x > sliceX);
            if (inside) alpha = 0.0;
        }
        if (uSliceEn.y > 0.5) {
            float sliceY = mix(uBoxMin.y, uBoxMax.y, uSliceHeightY);
            bool invert = uInvert.y > 0.5;
            bool inside = invert ? (pos.y < sliceY) : (pos.y > sliceY);
            if (inside) alpha = 0.0;
        }
        if (uSliceEn.z > 0.5) {
            float sliceZ = mix(uBoxMin.z, uBoxMax.z, uSliceHeightZ);
            bool invert = uInvert.z > 0.5;
            bool inside = invert ? (pos.z < sliceZ) : (pos.z > sliceZ);
            if (inside) alpha = 0.0;
        }
    }
    return alpha;
}

void main() {
    vec3 rayDir = normalize(vRayDir);
    float tFar;
    float tNear = intersectBox(vRayOrigin, rayDir, uBoxMin, uBoxMax, tFar);
    if (tNear < 0.0) discard;

    vec3 entry = vRayOrigin + rayDir * tNear;
    float rayLength = tFar - tNear;

    vec3 accum = vec3(0.0);
    float accumA = 0.0;
    float t = 0.0;

    for (int i = 0; i < MAX_STEPS; ++i) {
        if (accumA > 0.95) break;
        if (t > rayLength) break;

        vec3 pos = entry + rayDir * t;
        pos = clamp(pos, uBoxMin, uBoxMax);

        float sliceAlpha = computeSliceAlpha(pos);

        vec3 uvw = (pos - uBoxMin) / (uBoxMax - uBoxMin);
        float val = texture(uVolumeTex, uvw).r;

        float scalarRange = max(uScalarMax - uScalarMin, 1e-6);
        float tVal = clamp((val - uScalarMin) / scalarRange, 0.0, 1.0);

        if (sliceAlpha < 0.01) {
            t += uStepSize;
            continue;
        }

        // Gradient-based adaptive step size
        vec3 grad = computeGradient(uvw);
        float gradMag = length(grad);
        float featureScale = 1.0 / (gradMag * 10.0 + 1.0);
        float adaptiveStep = uStepSize * max(0.1, featureScale);

        vec3 color;
        if (uVolumeUseColormap == 1) {
            color = texture(uColormapLUT, tVal).rgb;
        } else {
            color = vec3(tVal);
        }

        float alpha = tVal * uOpacity * 0.1f * sliceAlpha;
        alpha = clamp(alpha, 0.0, 1.0);

        accum += alpha * color * (1.0 - accumA);
        accumA += alpha * (1.0 - accumA);

        t += adaptiveStep;
    }

    if (accumA < 0.005) discard;

    FragColor = vec4(accum, accumA);
}
