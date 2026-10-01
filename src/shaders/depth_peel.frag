#version 460 core

// Dual depth peeling for order-independent transparency.
//
// Differences from single-layer peeling, and why they matter:
//
//  * Two surfaces per pass. Each iteration resolves BOTH the next front peel
//    and the next back peel, so N iterations resolve up to 2N layers for the
//    same number of geometry passes. Same result, half the rasterisation.
//
//  * No depth attachment at all. Depth ranges live in an RG32F colour
//    attachment holding (-nearest, farthest) per pixel, accumulated with
//    GL_MAX blending on negated near depth. This sidesteps depth-buffer
//    quantisation AND the classic bug where a fragment's depth must be
//    compared against a *previous pass's* depth attachment -- which forces
//    either an epsilon or a separate depth resolve, and where a mismatched
//    depth format silently poisons every layer.
//
//  * Single epsilon, applied symmetrically to both peel boundaries. Tiles
//    rasterize large triangles in pieces, and the same surface can land on
//    very slightly different depths on two passes. Without a minimum
//    separation a single surface gets counted twice, so sub-pixel noise
//    accumulates as spurious layers. 1e-7 in NDC depth is micrometre-scale in
//    world space at any sane camera distance, far below the tolerance that
//    would swallow genuinely distinct layers.
//
// Stage semantics (uStage):
//   0 INITIALIZE - seed the depth range with the first translucent surface
//   1 PEELING    - one iteration: front peel + back peel
//   2 ALPHA      - mop up leftover fragments with plain alpha blending
//
// Draw buffers are bound by the C++ per stage (0..2 as declared below).

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
    vec4  uPBR;             // x = matRoughness, y = matMetallic, z = pad, w = pad
    vec4  uShadingMode;     // x = 0.0 smooth, 1.0 flat; y = 0.0 normal clip, 1.0 crinkle clip
};

in MeshVarying {
    vec3 vWorldPos;
    vec3 vNormal;
    float vScalar;
} mv;

uniform sampler2D uOpaqueDepth;   // opaque scene depth, single-sample
uniform sampler2D uDepthSource;   // RG32F: (-nearest, farthest) from previous pass
uniform sampler2D uFrontSource;   // accumulated front color, alpha stores (1-alpha)
uniform sampler1D uColormapLUT;
uniform float uNumBands = 0.0;
uniform int uStage = 0;

// 1e-7: see header comment. Minimum separation that stops one surface from
// being counted twice; micrometre-scale in world space.
const float kPeelEps = 1e-7;

layout(location = 0) out vec4 outBack;     // back peel accumulator (premultiplied)
layout(location = 1) out vec4 outFront;    // front peel accumulator (alpha = 1-a)
layout(location = 2) out vec4 outDepth;    // RG32F depth range for the next pass

vec3  uMatAmbient()    { return vec3(uMaterial.x); }
float uMatDiffuse()    { return uMaterial.y; }
float uMatSpecular()   { return uMaterial.z; }
float uKeyIntensity()   { return uIntensities.x; }
float uFillIntensity()  { return uIntensities.y; }
float uBackIntensity()  { return uIntensities.z; }
float uHeadIntensity()  { return uIntensities.w; }
float uMatRoughness() { return uPBR.x; }
float uMatMetallic()  { return uPBR.y; }

// lightContributionPBR injected from pbr_common.glsl at compile time.

// Full PBR shade, evaluated only once we know the fragment is on a peel.
// Straight (non-premultiplied) alpha: outBack premultiplies explicitly,
// outFront keeps the (1-alpha) convention needed for MAX blending.
vec4 shadeFragment(bool isPoint)
{
    // Screen-space cross(dFdx, dFdy) normals are view-oriented regardless of
    // winding; attribute normals are not, so only they need the back-face flip.
    vec3 norm;
    if (uShadingMode.x > 0.5) {
        norm = normalize(cross(dFdx(mv.vWorldPos), dFdy(mv.vWorldPos)));
    } else {
        norm = normalize(mv.vNormal);
        if (!gl_FrontFacing) norm = -norm;
    }
    vec3 viewDir = normalize(uViewPos_PS.xyz - mv.vWorldPos);

    bool hasScalars = uScalars.z > 0.5;
    vec3 baseColor = uSurfaceColor_Op.xyz;
    if (hasScalars && (uScalars.x != uScalars.y)) {
        float t = clamp((mv.vScalar - uScalars.x) / (uScalars.y - uScalars.x), 0.0, 1.0);
        if (uNumBands > 1.0) {
            t = floor(t * uNumBands) / (uNumBands - 1.0);
        }
        baseColor = textureLod(uColormapLUT, t, 0.0).rgb;
    }

    // [S1] Microfacet parameters shared by every light in the kit.
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

    // [S1] baseColor and material scalars fold in once after accumulation.
    vec3 finalColor = baseColor * uMatAmbient()
                    + baseColor * totalDiffuse * uMatDiffuse()
                    + totalSpecular * uMatSpecular();
    if (isPoint) finalColor += baseColor * 0.15;

    // Linear alpha, passed straight through. Blending already happens in
    // linear light, so a perceptual remap here would double-correct and make
    // the slider disagree with what is on screen. Layer accumulation is
    // inherently 1-(1-a)^N; that is the algorithm, not something to paper over.
    return vec4(finalColor, isPoint ? uPointClip.z : uSurfaceColor_Op.w);
}

void main() {
    // [S6] Fused clip predicate, cheapest test first (mirrors mesh.frag).
    bool hasScalars = uScalars.z > 0.5;
    // Same epsilon as mesh.frag -- avoids pinholes from perspective
    // interpolation sub-ULP excursions when filter bounds snap to data range.
    float filterEps = 1e-5 * abs(uScalars.y - uScalars.x) + 1e-9;
    bool clipped = (uFilter.z > 0.5) && hasScalars &&
                   (mv.vScalar < uFilter.x - filterEps || mv.vScalar > uFilter.y + filterEps);
    bool crinkleMode = uShadingMode.y > 0.5;
    // Per-fragment slice clip only for non-crinkle. Crinkle uses the GS path
    // which culls whole triangles; fragment path must skip to match the
    // opaque MeshCrinkleClip behaviour.
    if (!clipped && uPointClip.w > 0.5 && !crinkleMode) {
        bool clipX = bool(uClipEn.x) && ((uInvert.x > 0.5) ? (mv.vWorldPos.x < uClipY.x) : (mv.vWorldPos.x > uClipY.x));
        bool clipY = bool(uClipEn.y) && ((uInvert.y > 0.5) ? (mv.vWorldPos.y < uClipY.y) : (mv.vWorldPos.y > uClipY.y));
        bool clipZ = bool(uClipEn.z) && ((uInvert.z > 0.5) ? (mv.vWorldPos.z < uClipY.z) : (mv.vWorldPos.z > uClipY.z));
        clipped = clipX || clipY || clipZ;
    }
    if (clipped) discard;

    ivec2 pix = ivec2(gl_FragCoord.xy);
    float fragDepth = gl_FragCoord.z;
    vec4 frag = shadeFragment(uPointClip.x > 0.5);

    if (uStage == 0) {
        // INITIALIZE: seed the depth range in a single MAX-blended pass. Every
        // fragment covering this pixel writes (-depth, depth), so MAX over the
        // negated near component selects the NEAREST translucent surface and MAX
        // over the far component the FARTHEST -- the full initial range at
        // once. This pass therefore REQUIRES GL_MAX blending; with it disabled
        // the range collapses to whichever fragment drew last and a deep stack
        // resolves as a single layer.
        //
        // outBack/outFront are unused here but must be written explicitly: with
        // MAX blending an undefined value could win against the clear sentinel.
        outBack  = vec4(0.0);
        outFront = vec4(0.0);

        float oDepth = texelFetch(uOpaqueDepth, pix, 0).r;
        if (oDepth < 1.0 && fragDepth > oDepth) {
            // Occluded by opaque geometry: keep the opaque depth as the far
            // bound so nothing behind it can ever contribute. near = -1 loses
            // to any real (negative) fragment under MAX, so an occluded pixel
            // correctly resolves to the opaque surface.
            outDepth = vec4(-1.0, oDepth, 0.0, 0.0);
        } else {
            outDepth = vec4(-fragDepth, fragDepth, 0.0, 0.0);
        }
        return;
    }

    if (uStage == 2) {
        // ALPHA mop-up: everything still inside the outstanding depth range
        // gets plain alpha-blended. Only the back buffer is written.
        vec2 range = texelFetch(uDepthSource, pix, 0).xy;
        float minDepth = -range.x;
        float maxDepth = range.y;
        if (fragDepth < minDepth || fragDepth > maxDepth) discard;
        outBack = vec4(frag.rgb * frag.a, frag.a);
        return;
    }

    // ---- PEELING -------------------------------------------------------
    vec2 range = texelFetch(uDepthSource, pix, 0).xy;
    float minDepth = -range.x;
    float maxDepth = range.y;
    vec4 front = texelFetch(uFrontSource, pix, 0);

    // Defaults: no contribution this iteration.
    outBack  = vec4(0.0);
    outFront = front;
    outDepth = vec4(-1.0);

    // Outside the outstanding range entirely.
    if (fragDepth < minDepth - kPeelEps || fragDepth > maxDepth + kPeelEps)
        return;

    // Strictly interior: a future peel. Record the candidate depth range and
    // stop -- this is what makes dual peeling resolve 2 layers per pass.
    if (fragDepth > minDepth + kPeelEps && fragDepth < maxDepth - kPeelEps) {
        outDepth = vec4(-fragDepth, fragDepth, 0.0, 0.0);
        return;
    }

    if (fragDepth >= minDepth - kPeelEps && fragDepth <= minDepth + kPeelEps) {
        // FRONT peel: this is the outermost surface not yet resolved.
        // Accumulate with under-blending. front.a is STORED as (1-alpha),
        // which is what lets GL_MAX do the work: MAX over (1-alpha) keeps the
        // most transparent value, i.e. the correct accumulate-on-write.
        // It therefore has to be converted BACK to alpha before use.
        float aAccum = 1.0 - front.a;
        outFront.rgb = aAccum * frag.a * frag.rgb + front.rgb;
        outFront.a   = 1.0 - (aAccum * (1.0 - frag.a));
        outBack      = vec4(0.0);
    } else {
        // BACK peel: outermost surface behind the outstanding range. Premultiply
        // and hand straight to the back accumulator, which is itself blended
        // back-to-front with ONE / ONE_MINUS_SRC_ALPHA.
        outBack  = vec4(frag.rgb * frag.a, frag.a);
        outFront = front;
        outDepth = vec4(-1.0);
    }
}