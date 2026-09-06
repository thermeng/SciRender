#version 460 core

layout(std140) uniform MeshUBO {
    mat4  uMVP;
    mat4  uModel;
    vec4  uViewPos_PS;
    vec4  uMeshColor_Wire;
    vec4  uSurfaceColor_Op;
    vec4  uPointClip;
    vec4  uLightDir;
    vec4  uLightFill;
    vec4  uLightBack1;
    vec4  uLightBack2;
    vec4  uLightHead;
    vec4  uKeyColor;
    vec4  uFillColor;
    vec4  uBackColor;
    vec4  uHeadColor;
    vec4  uScalars;
    vec4  uClipY;
    vec4  uClipEn;
    vec4  uInvert;
    vec4  uFilter;
    vec4  uMaterial;
    vec4  uIntensities;
    vec4  uPBR;
    vec4  uShadingMode;
};

in MeshVarying {
    vec3 vWorldPos;
    vec3 vNormal;
    float vScalar;
} mv;

uniform sampler1D uColormapLUT;
uniform float uNumBands = 0.0;

uniform sampler3D uVectorTex;
uniform sampler3D uNoiseTex; // 3D white noise (was sampler2D planar). Eliminates axis-aligned stretch on 3D non-planar surfaces.
uniform sampler2D uNoiseTex2D_Fallback; // kept for legacy binding compat, not sampled
uniform vec3 uBoxMin;
uniform vec3 uBoxMax;
uniform int uLicSteps;          // half-kernel length; UI [4..128], GPU tolerates [0..128] (0 = single-sample fallback)
uniform float uLicStepSize;     // world units (= clamp(RenderSettings licStepSize, 0.001..2.0) * diag), clamped to [1e-6, diag*2]
uniform float uLicNoiseFreq;    // noise UV scale, expected [0.5..64]
uniform int uLicEnhanced;       // 0=single-pass, 1=enhanced 2-pass (sharpens coherence)
uniform vec3 uUvwScale = vec3(1.0);
uniform vec3 uUvwOffset = vec3(0.0);

out vec4 FragColor;

vec3 worldToUVW(vec3 pos, vec3 boxMin, vec3 invRange, vec3 range) {
    // General-purpose degenerate-axis handling: collapse thin slab to center 0.5.
    // Uses range-relative epsilon (not absolute 1e-8 world units) so both small and large scenes behave identically.
    // Rank-aware path (future): slab should dispatch to 2D texture; for now center-sample is the correct 3D fallback for vectors parallel to plane.
    float diag = max(max(abs(range.x), abs(range.y)), abs(range.z));
    float eps = max(diag * 1e-7, 1e-7);
    vec3 uvw = (pos - boxMin) * invRange;
    if (abs(range.x) <= eps) uvw.x = 0.5;
    if (abs(range.y) <= eps) uvw.y = 0.5;
    if (abs(range.z) <= eps) uvw.z = 0.5;
    uvw = uvw * uUvwScale + uUvwOffset;
    return uvw;
}

vec3  uMatAmbient()    { return vec3(uMaterial.x); }
float uMatDiffuse()    { return uMaterial.y; }
float uMatSpecular()   { return uMaterial.z; }
float uKeyIntensity()   { return uIntensities.x; }
float uFillIntensity()  { return uIntensities.y; }
float uBackIntensity()  { return uIntensities.z; }
float uHeadIntensity()  { return uIntensities.w; }
float uMatRoughness() { return uPBR.x; }
float uMatMetallic()  { return uPBR.y; }

// Legacy planar projection — retained for reference/fallback on degenerate slabs or when mesh UV path is added.
// For 3D non-planar geometry this causes stretching/discontinuities at absNorm tie-breaks.
vec2 noiseUVForPos_Planar(vec3 worldPos, vec3 boxMin, vec3 invRange, vec3 range, vec3 absNorm) {
    vec3 uvw = worldToUVW(worldPos, boxMin, invRange, range);
    if (dot(absNorm, absNorm) < 1e-8) {
        return uvw.xy * uLicNoiseFreq;
    }
    if (absNorm.z >= absNorm.x && absNorm.z >= absNorm.y) {
        return uvw.xy * uLicNoiseFreq;
    } else if (absNorm.y >= absNorm.x) {
        return uvw.xz * uLicNoiseFreq;
    } else {
        return uvw.yz * uLicNoiseFreq;
    }
}

// True 3D noise sampling — isotropic, no planar projection. worldToUVW gives [0,1]^3, scaled by frequency.
// Eliminates stretching on 3D non-planar surfaces. If native mesh UVs become available, this can be
// branched to texture(uNoiseTex2D, vUV*uLicNoiseFreq) instead.
float licNoiseSample3D(vec3 worldPos, vec3 boxMin, vec3 invRange, vec3 range) {
    vec3 uvw = worldToUVW(worldPos, boxMin, invRange, range);
    // scale in 3D; GL_REPEAT will tile
    return texture(uNoiseTex, uvw * uLicNoiseFreq).r;
}

float licNoiseSample3D_scaled(vec3 worldPos, vec3 boxMin, vec3 invRange, vec3 range, float freqScale) {
    vec3 uvw = worldToUVW(worldPos, boxMin, invRange, range);
    return texture(uNoiseTex, uvw * uLicNoiseFreq * freqScale).r;
}

// Primary LIC noise sample — uses 3D isotropic path. Falls back to planar only if 3D texture not bound
// (should never happen; MeshPass always provides 3D noise). Kept signature for call sites.
float licNoiseSample(vec3 worldPos, vec3 boxMin, vec3 invRange, vec3 range, vec3 absNorm) {
    // Prefer 3D isotropic; absNorm ignored — avoids axis-aligned stretch.
    // To re-enable mesh-UV path: if(hasNativeUV) return texture(uNoiseTex2D_Fallback, vUV*uLicNoiseFreq).r;
    return licNoiseSample3D(worldPos, boxMin, invRange, range);
}

// Helper: sample normalized vector direction at a world position.
// Returns sign * v/|v| if |v| > magThresh else vec3(0) signalling termination (matches StreamlineSet RK4).
//
// Surface-LIC tangent projection: the underlying vector field is 3D and can
// point into/out of the surface (e.g. radial vortex component on a cube face).
// Walking the RK4 integrator along a vector with any out-of-plane component
// leaves the surface slab and accumulates noise from a different 3D world
// position than the fragment being shaded, producing uncorrelated speckle
// instead of streaks (top face worked only by coincidence — its normal-axis
// vortex component is ~0, so the walk stayed on the slab).
//
// Fix: project the sampled vector onto the surface tangent plane before
// normalizing, and return vec3(0) if the tangential component vanishes
// (vector is normal to surface — no surface streak to draw). Termination
// matches the existing magThresh==0 path so RK4 break-on-zero still fires.
vec3 licSampleDir(vec3 pos, vec3 boxMin, vec3 invRange, vec3 range, vec3 norm, float sign) {
    const float magThresh = 5e-5;
    vec3 uvw = worldToUVW(pos, boxMin, invRange, range);
    vec3 v = texture(uVectorTex, uvw).rgb;
    // Tangent-plane projection: v_t = v - (v . n) n
    vec3 vTan = v - dot(v, norm) * norm;
    float mTan = length(vTan);
    if (mTan < magThresh) return vec3(0.0);
    return sign * (vTan / mTan);
}

// 2-pass Enhanced LIC helper: second pass input is the high-pass of the
// first pass noise field. Implemented as a second convolution that samples
// at a slightly finer frequency and then performs unsharp masking. This
// sharpens streak coherence without requiring an intermediate framebuffer
// (true ping-pong would need an offscreen LIC texture). The result remains
// single-draw-call.
// Runge-Kutta 4 integration (matches StreamlineSet RK4) replaces single-step Euler.
// `norm` is the surface normal at the shaded fragment and is re-applied at every
// RK4 stage sample so the projection uses the local tangent plane of the original
// surface (constant-norm approximation; valid for low-curvature surfaces and small
// relative step sizes, which is the regime LIC kernels operate in).
float computeLicGray(vec3 startPos, vec3 dir, vec3 boxMin, vec3 boxMax,
                     vec3 invRange, vec3 range, vec3 norm, vec3 absNorm,
                     int steps, float worldStepSize, float freqScale) {
    const float h = worldStepSize;
    float acc = licNoiseSample3D_scaled(startPos, boxMin, invRange, range, freqScale);
    float w = 1.0;
    // forward (sign +1) — RK4 (StreamlineSet); general-purpose: terminate on zero field, no dir inventing
    {
        vec3 pos = startPos;
        for (int i = 0; i < steps; ++i) {
            vec3 k1 = licSampleDir(pos, boxMin, invRange, range, norm, 1.0);
            if (dot(k1,k1) < 1e-12) break;
            vec3 k2 = licSampleDir(pos + 0.5 * h * k1, boxMin, invRange, range, norm, 1.0);
            if (dot(k2,k2) < 1e-12) break;
            vec3 k3 = licSampleDir(pos + 0.5 * h * k2, boxMin, invRange, range, norm, 1.0);
            if (dot(k3,k3) < 1e-12) break;
            vec3 k4 = licSampleDir(pos + h * k3, boxMin, invRange, range, norm, 1.0);
            if (dot(k4,k4) < 1e-12) break;
            vec3 newPos = pos + (h / 6.0) * (k1 + 2.0 * k2 + 2.0 * k3 + k4);
            acc += licNoiseSample3D_scaled(newPos, boxMin, invRange, range, freqScale);
            w += 1.0;
            pos = newPos;
        }
    }
    // backward (sign -1) — RK4 symmetric, no hack
    {
        vec3 pos = startPos;
        for (int i = 0; i < steps; ++i) {
            vec3 k1 = licSampleDir(pos, boxMin, invRange, range, norm, -1.0);
            if (dot(k1,k1) < 1e-12) break;
            vec3 k2 = licSampleDir(pos + 0.5 * h * k1, boxMin, invRange, range, norm, -1.0);
            if (dot(k2,k2) < 1e-12) break;
            vec3 k3 = licSampleDir(pos + 0.5 * h * k2, boxMin, invRange, range, norm, -1.0);
            if (dot(k3,k3) < 1e-12) break;
            vec3 k4 = licSampleDir(pos + h * k3, boxMin, invRange, range, norm, -1.0);
            if (dot(k4,k4) < 1e-12) break;
            vec3 newPos = pos + (h / 6.0) * (k1 + 2.0 * k2 + 2.0 * k3 + k4);
            acc += licNoiseSample3D_scaled(newPos, boxMin, invRange, range, freqScale);
            w += 1.0;
            pos = newPos;
        }
    }
    return acc / max(w, 1.0);
}

void main() {
    bool hasScalars = uScalars.z > 0.5;
    float filterEps = 1e-5 * abs(uScalars.y - uScalars.x) + 1e-9;
    bool clipped = (uFilter.z > 0.5) && hasScalars &&
                   (mv.vScalar < uFilter.x - filterEps || mv.vScalar > uFilter.y + filterEps);
    bool crinkleMode = uShadingMode.y > 0.5;
    if (!clipped && uPointClip.w > 0.5 && !crinkleMode) {
        bool clipX = bool(uClipEn.x) && ((uInvert.x > 0.5) ? (mv.vWorldPos.x < uClipY.x) : (mv.vWorldPos.x > uClipY.x));
        bool clipY = bool(uClipEn.y) && ((uInvert.y > 0.5) ? (mv.vWorldPos.y < uClipY.y) : (mv.vWorldPos.y > uClipY.y));
        bool clipZ = bool(uClipEn.z) && ((uInvert.z > 0.5) ? (mv.vWorldPos.z < uClipY.z) : (mv.vWorldPos.z > uClipY.z));
        clipped = clipX || clipY || clipZ;
    }
    if (clipped) {
        discard;
    }

    vec3 sphereNormal = mv.vNormal;
    bool isPoint = uPointClip.x > 0.5;
    if (isPoint) {
        vec2 pc = gl_PointCoord * 2.0 - 1.0;
        float r2 = dot(pc, pc);
        if (r2 > 1.0) discard;
        sphereNormal = vec3(pc, sqrt(1.0 - r2));
    }

    bool wireframe = uMeshColor_Wire.w > 0.5;
    if (wireframe) {
        FragColor = vec4(uMeshColor_Wire.xyz, 1.0);
        return;
    }

    vec3 range = uBoxMax - uBoxMin;
    float diag = length(range);
    if (!(diag > 1e-6)) diag = 1.0;
    float diagForInv = max(max(abs(range.x), abs(range.y)), abs(range.z));
    float epsInv = max(diagForInv * 1e-7, 1e-7);
    vec3 invRange;
    invRange.x = (abs(range.x) > epsInv) ? 1.0 / range.x : 0.0;
    invRange.y = (abs(range.y) > epsInv) ? 1.0 / range.y : 0.0;
    invRange.z = (abs(range.z) > epsInv) ? 1.0 / range.z : 0.0;
    vec3 uvw = worldToUVW(mv.vWorldPos, uBoxMin, invRange, range);

    vec3 scalarBase = uSurfaceColor_Op.xyz;
    if (hasScalars && (uScalars.x != uScalars.y)) {
        float st = clamp((mv.vScalar - uScalars.x) / (uScalars.y - uScalars.x), 0.0, 1.0);
        if (uNumBands > 1.0) {
            st = clamp(floor(st * uNumBands) / (uNumBands - 1.0), 0.0, 1.0);
        }
        scalarBase = textureLod(uColormapLUT, st, 0.0).rgb;
    }
    bool pointUseScalar = uPointClip.y > 0.5;
    if (isPoint && !pointUseScalar) {
        scalarBase = uSurfaceColor_Op.xyz;
    }

    vec3 vec = texture(uVectorTex, uvw).rgb;
    float mag = length(vec);

    float licGray = 0.5;
    const float magThresh = 5e-5;
    bool hasVector = mag > magThresh;

    // General-purpose normal handling: degenerate triangles produce near-zero normals.
    // Use relative epsilon scaled by world scale; fallback to interpolated vertex normal, not invented (0,0,1) as hack.
    float normEps = max(diag * diag * 1e-14, 1e-12);
    float flatEps = max(diag * diag * 1e-16, 1e-20);
    vec3 norm;
    if (isPoint) {
        vec3 n0 = sphereNormal;
        if (dot(n0, n0) < normEps) {
            // Point sprite edge without valid normal — use view-facing fallback derived from range, not hard (0,0,1)
            n0 = normalize(vec3(range.x, range.y, 1.0));
            if (dot(n0,n0) < 1e-12) n0 = vec3(0.0, 1.0, 0.0);
        }
        norm = normalize(n0);
        if (!gl_FrontFacing) norm = -norm;
    } else if (uShadingMode.x > 0.5) {
        vec3 fdx = dFdx(mv.vWorldPos);
        vec3 fdy = dFdy(mv.vWorldPos);
        vec3 fc = cross(fdx, fdy);
        if (dot(fc, fc) < flatEps) fc = sphereNormal;
        if (dot(fc, fc) < normEps) fc = normalize(vec3(0.0, 1.0, 0.0)); // last resort world up
        norm = normalize(fc);
    } else {
        vec3 n0 = sphereNormal;
        if (dot(n0, n0) < normEps) n0 = normalize(vec3(0.0, 1.0, 0.0));
        norm = normalize(n0);
        if (!gl_FrontFacing) norm = -norm;
    }

    vec3 absNorm = abs(norm);
    // Defense-in-depth: UI clamps [4..128] and [0.5..64]; shader tolerates wider but bounds loop & UV.
    int steps = clamp(uLicSteps, 0, 128);
    // uLicStepSize already clamped on CPU to [1e-6, diag*2]; re-clamp here against local diag for safety if uniform is injected directly.
    // diag already computed above from range
    float worldStepSize = clamp(uLicStepSize, 1e-6, diag * 2.0);

    if (hasVector) {
        // Surface-LIC: pass the surface normal so licSampleDir can project each
        // sampled vector onto the surface tangent plane before normalizing.
        // The integrator re-applies this normal at every RK4 stage sample to
        // keep the walk on the surface slab.
        float g1 = computeLicGray(mv.vWorldPos, vec, uBoxMin, uBoxMax, invRange, range, norm, absNorm, steps, worldStepSize, 1.0);
        if (uLicEnhanced != 0) {
            // Enhanced LIC (2-pass): second convolution at 1.35x freq sharpens
            // coherence via unsharp masking. Previously two clamps (unsharp then
            // contrast) truncated high-frequency contribution before stretch.
            // Fused to single clamp after both stages so stretch applies to full
            // signed high term, then bounds to [0,1].
            float g2 = computeLicGray(mv.vWorldPos, vec, uBoxMin, uBoxMax, invRange, range, norm, absNorm, steps, worldStepSize, 1.35);
            float high = g1 - g2;
            float fused = g1 + 0.55 * high;
            licGray = clamp((fused - 0.5) * 2.15 + 0.5, 0.0, 1.0);
        } else {
            licGray = clamp((g1 - 0.5) * 1.8 + 0.5, 0.0, 1.0);
        }
    } else {
        float n = licNoiseSample(mv.vWorldPos, uBoxMin, invRange, range, absNorm);
        licGray = clamp((n - 0.5) * 1.8 + 0.5, 0.0, 1.0);
    }

    // LIC gain 0.35..1.20 applied before desaturation. Intentionally allows
    // >1.0 so bright streaks pop; final PBR + sRGB framebuffer handles HDR.
    // Do not clamp here — contrast clamps above already bounded licGray.
    vec3 baseColor = scalarBase * (0.35 + 0.85 * licGray);
    baseColor = mix(vec3(dot(baseColor, vec3(0.2126,0.7152,0.0722))), baseColor, 0.85);

    vec3 viewDir = normalize(uViewPos_PS.xyz - mv.vWorldPos);

    float aa = clamp(uMatRoughness() * uMatRoughness(), 0.04, 1.0);
    float a2 = aa * aa;
    float k  = (aa + 1.0) * (aa + 1.0) / 8.0;
    vec3  F0 = mix(vec3(0.04), baseColor, uMatMetallic());
    float oneMinusMetallic = 1.0 - uMatMetallic();

    vec3 totalDiffuse = vec3(0.0);
    vec3 totalSpecular = vec3(0.0);
    lightContributionPBR(uLightDir.xyz,   norm, uKeyIntensity(),   uKeyColor.xyz,   viewDir, a2, k, F0, oneMinusMetallic, totalDiffuse, totalSpecular);
    lightContributionPBR(uLightFill.xyz,  norm, uFillIntensity(),  uFillColor.xyz,  viewDir, a2, k, F0, oneMinusMetallic, totalDiffuse, totalSpecular);
    lightContributionPBR(uLightBack1.xyz, norm, uBackIntensity(),  uBackColor.xyz,  viewDir, a2, k, F0, oneMinusMetallic, totalDiffuse, totalSpecular);
    lightContributionPBR(uLightBack2.xyz, norm, uBackIntensity(),  uBackColor.xyz,  viewDir, a2, k, F0, oneMinusMetallic, totalDiffuse, totalSpecular);
    lightContributionPBR(uLightHead.xyz,  norm, uHeadIntensity(),  uHeadColor.xyz,  viewDir, a2, k, F0, oneMinusMetallic, totalDiffuse, totalSpecular);

    float surfaceOpacity = uSurfaceColor_Op.w;
    vec3 finalColor = baseColor * uMatAmbient()
                    + baseColor * totalDiffuse * uMatDiffuse()
                    + totalSpecular * uMatSpecular();
    float pointOpacity = uPointClip.z;
    if (isPoint) {
        finalColor += baseColor * 0.15;
        FragColor = vec4(finalColor, pointOpacity);
    } else {
        FragColor = vec4(finalColor, surfaceOpacity);
    }
}
