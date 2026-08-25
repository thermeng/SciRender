#version 460 core

in vec2 vNdc;

uniform sampler3D uVolumeTex;
uniform sampler1D uColormapLUT;
uniform vec3 uBoxMin;
uniform vec3 uBoxMax;
uniform vec3 uSafeExtent;
uniform float uBaseStepSize;
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
uniform float uPixelFootprintScale;
uniform float uFovY;
uniform int uOrtho;
uniform mat4 uInvView;
uniform mat4 uInvProj;
uniform vec3 uCamPos;

#define MAX_STEPS 512

out vec4 FragColor;

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

void main() {
    vec4 clipNear = vec4(vNdc, 0.0, 1.0);
    vec4 clipFar  = vec4(vNdc, 1.0, 1.0);

    vec4 viewNear = uInvProj * clipNear;
    viewNear /= (abs(viewNear.w) > 1e-8 ? viewNear.w : 1.0);
    vec4 viewFar = uInvProj * clipFar;
    viewFar /= (abs(viewFar.w) > 1e-8 ? viewFar.w : 1.0);

    vec4 worldNear = uInvView * viewNear;
    worldNear /= (abs(worldNear.w) > 1e-8 ? worldNear.w : 1.0);
    vec4 worldFar = uInvView * viewFar;
    worldFar /= (abs(worldFar.w) > 1e-8 ? worldFar.w : 1.0);

    vec3 rayOrigin = (uOrtho == 1) ? worldNear.xyz : uCamPos;
    vec3 rayDir = normalize(worldFar.xyz - rayOrigin);

    float tFarBox;
    float tNear = intersectBox(rayOrigin, rayDir, uBoxMin, uBoxMax, tFarBox);
    if (tNear < 0.0 || tFarBox <= tNear) discard;

    float maxDist = length(uBoxMax - uBoxMin);
    float safeRayLength = min(tFarBox - tNear, maxDist);
    tNear = max(tNear, 0.0);

    vec3 entry = rayOrigin + rayDir * tNear;

    vec3 accum = vec3(0.0);
    float accumA = 0.0;

    float diag = length(uSafeExtent);
    float minStep = diag / float(MAX_STEPS);

    // [P1] Loop invariants hoisted out of the march loop: scalar normalization,
    // gradient epsilon, slice enable/invert flags, and the three slice-plane
    // positions (previously re-mixed on every step).
    const float scalarRange = max(uScalarMax - uScalarMin, 1e-6);
    const vec3  gradEps = vec3(1.0 / 128.0);   // matches previous per-step epsilon
    const bool  clipOn   = (uClipEnabled == 1);
    const bool  sliceEnX = uSliceEn.x > 0.5;
    const bool  sliceEnY = uSliceEn.y > 0.5;
    const bool  sliceEnZ = uSliceEn.z > 0.5;
    const bool  invX = uInvert.x > 0.5;
    const bool  invY = uInvert.y > 0.5;
    const bool  invZ = uInvert.z > 0.5;
    const float slicePlaneX = mix(uBoxMin.x, uBoxMax.x, uSliceHeightX);
    const float slicePlaneY = mix(uBoxMin.y, uBoxMax.y, uSliceHeightY);
    const float slicePlaneZ = mix(uBoxMin.z, uBoxMax.z, uSliceHeightZ);

    float t = 0.0;
    int steps = 0;
    for (int i = 0; i < MAX_STEPS; ++i) {
        if (accumA > 0.9) break;
        if (t >= safeRayLength) break;
        steps = i;

        vec3 pos = entry + rayDir * t;
        pos = clamp(pos, uBoxMin, uBoxMax);

        // [P1] Slice test against precomputed planes.
        float sliceAlpha = 1.0;
        if (clipOn) {
            if (sliceEnX && (invX ? (pos.x < slicePlaneX) : (pos.x > slicePlaneX))) sliceAlpha = 0.0;
            if (sliceEnY && (invY ? (pos.y < slicePlaneY) : (pos.y > slicePlaneY))) sliceAlpha = 0.0;
            if (sliceEnZ && (invZ ? (pos.z < slicePlaneZ) : (pos.z > slicePlaneZ))) sliceAlpha = 0.0;
        }

        vec3 uvw = clamp((pos - uBoxMin) / uSafeExtent, 0.0, 1.0);
        float val = texture(uVolumeTex, uvw).r;

        float tVal = clamp((val - uScalarMin) / scalarRange, 0.0, 1.0);

        // [P1] Forward differences reuse the already-fetched center sample:
        // 4 texture fetches per step instead of 7 (central differences plus a
        // separate center fetch). The magnitude feeds only the adaptive
        // step-size heuristic, so the accuracy change is imperceptible.
        vec3 g;
        g.x = texture(uVolumeTex, uvw + vec3(gradEps.x, 0.0, 0.0)).r - val;
        g.y = texture(uVolumeTex, uvw + vec3(0.0, gradEps.y, 0.0)).r - val;
        g.z = texture(uVolumeTex, uvw + vec3(0.0, 0.0, gradEps.z)).r - val;
        float gradMag = length(g);
        float featureScale = 1.0 / (gradMag * 10.0 + 1.0);
        float currentDist = length(pos - uCamPos);
        float pixelFootprint = uOrtho == 1
            ? uPixelFootprintScale
            : uPixelFootprintScale * currentDist;
        float stepSize = max(min(pixelFootprint * max(0.1, featureScale), uBaseStepSize), minStep);

        if (sliceAlpha < 0.01) {
            t += stepSize;
            continue;
        }

        vec3 color;
        if (uVolumeUseColormap == 1) {
            color = texture(uColormapLUT, tVal).rgb;
        } else {
            color = vec3(tVal);
        }

        float density = tVal * uOpacity * sliceAlpha;
        float alpha = clamp(1.0 - exp(-density * stepSize), 0.0, 1.0);

        accum += alpha * color * (1.0 - accumA);
        accumA += alpha * (1.0 - accumA);

        if (alpha < 1e-4 && density < 1e-3 && float(steps) > 4.0) {
            float skipFactor = clamp(density < 1e-6 ? 8.0 : 4.0, 1.0, 8.0);
            t += stepSize * skipFactor;
        } else {
            t += stepSize;
        }
    }

    if (accumA < 0.005) discard;

    accum = pow(accum, vec3(1.0 / 2.2));

    FragColor = vec4(accum, accumA);
}
