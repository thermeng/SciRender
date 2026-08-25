// Shared PBR lighting kit for SciRender fragment shaders.
// Injected verbatim after the #version line by injectPbrCommon() in
// shader_utils.h. The function is intentionally dependency-free: every input,
// including all material terms, arrives as a parameter, so this chunk can be
// injected into any fragment shader regardless of its uniform-block layout.
//
// Microfacet BRDF: GGX normal distribution + Smith geometry + Schlick Fresnel,
// energy-conserving. [S1] Every term invariant across the 5-light kit (a2, k,
// F0, oneMinusMetallic) plus the material scalars are supplied by main(); this
// body computes only genuinely per-light quantities and accumulates WITHOUT
// baseColor/matDiffuse/matSpecular, which fold in once after the kit runs.
// [S2] Schlick factor uses an expanded polynomial instead of pow().
void lightContributionPBR(vec3 rawLightDir, vec3 norm, float intensity,
                          vec3 lightColor, vec3 viewDir,
                          float a2, float k, vec3 F0, float oneMinusMetallic,
                          inout vec3 diffuse, inout vec3 specular) {
    vec3 L = normalize(rawLightDir);
    float NdotL = max(dot(norm, L), 0.0);
    float NdotV = max(dot(norm, viewDir), 0.0);
    if (NdotL <= 0.0) return;

    vec3 H = normalize(L + viewDir);
    float NdotH = max(dot(norm, H), 0.0);
    float VdotH = max(dot(viewDir, H), 0.0);

    float d  = NdotH * NdotH * (a2 - 1.0) + 1.0;
    float D  = a2 / (3.14159265 * d * d);

    float Gl = NdotL / (NdotL * (1.0 - k) + k);
    float Gv = NdotV / (NdotV * (1.0 - k) + k);
    float G  = Gl * Gv;

    // [S2] (1 - VdotH)^5 expanded: f5 = f^2 * f^2 * f
    float f  = 1.0 - VdotH;
    float f2 = f * f;
    float f5 = f2 * f2 * f;
    vec3  F  = F0 + (1.0 - F0) * f5;

    float specFactor = D * G / (4.0 * NdotL * NdotV + 1e-4);
    specular += lightColor * F * specFactor * intensity;

    vec3 kD = (1.0 - F) * oneMinusMetallic;
    diffuse += lightColor * kD * NdotL * intensity;
}
