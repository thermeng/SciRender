#include "render/passes/SurfaceLicPass.h"
#include "render/foundation/renderer.h"
#include "render/foundation/shader_utils.h"
#include "render/passes/MeshGLManager.h"

#include <glad/gl.h>
#include <algorithm>
#include <random>
#include <vector>

void SurfaceLicPass::init(const ShaderSources& sources) {
    if (sources.meshVert.empty() || sources.licFlowFrag.empty() || sources.licConvolveFrag.empty()) return;

    m_flowProgram.reset(compileProgram(sources.meshVert.c_str(), sources.licFlowFrag.c_str(), "LICFlow"));
    if (m_flowProgram.has()) {
        m_flowUboIndex = glGetUniformBlockIndex(m_flowProgram, "MeshUBO");
        if (m_flowUboIndex != GL_INVALID_INDEX) glUniformBlockBinding(m_flowProgram, m_flowUboIndex, 0);
        m_flowVectorTexLoc = glGetUniformLocation(m_flowProgram, "uVectorTex");
        m_flowBoxMinLoc = glGetUniformLocation(m_flowProgram, "uBoxMin");
        m_flowBoxMaxLoc = glGetUniformLocation(m_flowProgram, "uBoxMax");
        m_flowUvwScaleLoc = glGetUniformLocation(m_flowProgram, "uUvwScale");
        m_flowUvwOffsetLoc = glGetUniformLocation(m_flowProgram, "uUvwOffset");
        m_flowMagThreshLoc = glGetUniformLocation(m_flowProgram, "uLicMagThresh");
        m_flowViewportLoc = glGetUniformLocation(m_flowProgram, "uViewport");
    }

    m_convolveProgram.reset(compileProgram(sources.compositeVert.c_str(), sources.licConvolveFrag.c_str(), "LICConvolve"));
    if (m_convolveProgram.has()) {
        glUseProgram(m_convolveProgram);
        m_convFlowTexLoc = glGetUniformLocation(m_convolveProgram, "uFlowTex");
        m_convNoiseTexLoc = glGetUniformLocation(m_convolveProgram, "uNoiseTex");
        m_convTexelSizeLoc = glGetUniformLocation(m_convolveProgram, "uTexelSize");
        m_convStepsLoc = glGetUniformLocation(m_convolveProgram, "uSteps");
        m_convStepSizeLoc = glGetUniformLocation(m_convolveProgram, "uStepSize");
        m_convNoiseFreqLoc = glGetUniformLocation(m_convolveProgram, "uNoiseFreq");
        m_convEnhancedLoc = glGetUniformLocation(m_convolveProgram, "uEnhanced");
        if (m_convFlowTexLoc != -1) glUniform1i(m_convFlowTexLoc, 0);
        if (m_convNoiseTexLoc != -1) glUniform1i(m_convNoiseTexLoc, 1);
        glUseProgram(0);
    }

    if (!m_dummyVao.has()) glCreateVertexArrays(1, m_dummyVao.ptr());
}

void SurfaceLicPass::ensureTargets(int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (m_w == w && m_h == h && m_flowFbo.has() && m_licFbo.has()) return;
    m_w = w; m_h = h;

    if (m_flowTex.has()) m_flowTex.reset();
    if (m_flowDepth.has()) m_flowDepth.reset();
    if (m_flowFbo.has()) m_flowFbo.reset();
    if (m_licTex.has()) m_licTex.reset();
    if (m_licFbo.has()) m_licFbo.reset();

    glCreateTextures(GL_TEXTURE_2D, 1, m_flowTex.ptr());
    glTextureStorage2D(m_flowTex, 1, GL_RGBA16F, w, h);
    glTextureParameteri(m_flowTex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(m_flowTex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(m_flowTex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_flowTex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glCreateRenderbuffers(1, m_flowDepth.ptr());
    glNamedRenderbufferStorage(m_flowDepth, GL_DEPTH24_STENCIL8, w, h);

    glCreateFramebuffers(1, m_flowFbo.ptr());
    glNamedFramebufferTexture(m_flowFbo, GL_COLOR_ATTACHMENT0, m_flowTex, 0);
    glNamedFramebufferRenderbuffer(m_flowFbo, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_flowDepth);

    glCreateTextures(GL_TEXTURE_2D, 1, m_licTex.ptr());
    glTextureStorage2D(m_licTex, 1, GL_R16F, w, h);
    glTextureParameteri(m_licTex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(m_licTex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(m_licTex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_licTex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glCreateFramebuffers(1, m_licFbo.ptr());
    glNamedFramebufferTexture(m_licFbo, GL_COLOR_ATTACHMENT0, m_licTex, 0);
}

void SurfaceLicPass::ensureNoise(int grain) {
    if (grain <= 0) grain = 64;
    if (grain > 128) grain = 128;
    if (m_noiseTex.has() && m_noiseGrain == grain) return;
    if (m_noiseTex.has()) m_noiseTex.reset();
    std::vector<unsigned char> noise(static_cast<size_t>(grain) * grain);
    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> dist(0, 255);
    for (auto& n : noise) n = static_cast<unsigned char>(dist(rng));
    glCreateTextures(GL_TEXTURE_2D, 1, m_noiseTex.ptr());
    glTextureStorage2D(m_noiseTex, 1, GL_R8, grain, grain);
    glTextureSubImage2D(m_noiseTex, 0, 0, 0, grain, grain, GL_RED, GL_UNSIGNED_BYTE, noise.data());
    glTextureParameteri(m_noiseTex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(m_noiseTex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(m_noiseTex, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTextureParameteri(m_noiseTex, GL_TEXTURE_WRAP_T, GL_REPEAT);
    m_noiseGrain = grain;
}

void SurfaceLicPass::drawFullscreen() {
    glBindVertexArray(m_dummyVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

GLuint SurfaceLicPass::render(const RenderRenderState& state,
                              const std::vector<std::pair<GLuint, int>>& drawList,
                              const MeshGLManager& meshManager,
                              GLuint meshUbo,
                              GLuint defaultFbo,
                              const Resources& res) {
    if (!hasProgram() || !res.valid() || drawList.empty() || !meshManager.hasMeshes()) return 0;
    GLint vp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, vp);
    const int w = vp[2], h = vp[3];
    if (w <= 0 || h <= 0) return 0;
    ensureTargets(w, h);
    ensureNoise(state.licNoiseGrain);
    if (!m_flowFbo.has() || !m_licFbo.has() || !m_noiseTex.has()) return 0;

    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    const GLuint srcFbo = defaultFbo ? defaultFbo : static_cast<GLuint>(prevFbo);

    // --- Flow pass ---
    glBindFramebuffer(GL_FRAMEBUFFER, m_flowFbo);
    glViewport(0, 0, w, h);
    const float clearFlow[4] = {0.f, 0.f, 0.f, 0.f};
    glClearBufferfv(GL_COLOR, 0, clearFlow);
    glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    // Copy scene depth so flow is only produced for visible surface fragments.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_flowFbo);
    glBlitFramebuffer(vp[0], vp[1], vp[0] + w, vp[1] + h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, m_flowFbo);

    glUseProgram(m_flowProgram);
    if (meshUbo != 0) glBindBufferBase(GL_UNIFORM_BUFFER, 0, meshUbo);
    if (m_flowVectorTexLoc != -1) { glBindTextureUnit(2, res.vectorTex); glUniform1i(m_flowVectorTexLoc, 2); }
    if (m_flowBoxMinLoc != -1) glUniform3fv(m_flowBoxMinLoc, 1, &res.boxMin.x);
    if (m_flowBoxMaxLoc != -1) glUniform3fv(m_flowBoxMaxLoc, 1, &res.boxMax.x);
    glm::vec3 uvwScale(1.0f), uvwOffset(0.0f);
    auto axisScaleOffset = [&](int n, float& s, float& o) {
        if (n <= 1) { s = 1.0f; o = 0.0f; return; }
        float inv = 1.0f / static_cast<float>(n);
        s = float(n - 1) * inv; o = 0.5f * inv;
    };
    axisScaleOffset(res.texDimX, uvwScale.x, uvwOffset.x);
    axisScaleOffset(res.texDimY, uvwScale.y, uvwOffset.y);
    axisScaleOffset(res.texDimZ, uvwScale.z, uvwOffset.z);
    if (m_flowUvwScaleLoc != -1) glUniform3fv(m_flowUvwScaleLoc, 1, &uvwScale.x);
    if (m_flowUvwOffsetLoc != -1) glUniform3fv(m_flowUvwOffsetLoc, 1, &uvwOffset.x);
    const float relMax = res.maxMagnitude > 0.0f ? res.maxMagnitude * 1e-6f : 0.0f;
    const float relFloor = res.robustFloor > 0.0f ? res.robustFloor * 1e-3f : 0.0f;
    if (m_flowMagThreshLoc != -1) glUniform1f(m_flowMagThreshLoc, std::max({relMax, relFloor, 1e-30f}));
    if (m_flowViewportLoc != -1) glUniform2f(m_flowViewportLoc, static_cast<float>(w), static_cast<float>(h));

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE); // don't modify scene depth
    glDisable(GL_BLEND);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1.0f, 1.0f);
    for (const auto& dl : drawList) {
        glBindVertexArray(dl.first);
        const bool cull = state.cullMode != 0;
        if (cull) { glEnable(GL_CULL_FACE); glCullFace(state.cullMode == 2 ? GL_FRONT : GL_BACK); }
        else glDisable(GL_CULL_FACE);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glDrawElements(GL_TRIANGLES, dl.second, GL_UNSIGNED_INT, 0);
        if (cull) glDisable(GL_CULL_FACE);
    }
    glBindVertexArray(0);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);

    // --- Convolve pass ---
    glBindFramebuffer(GL_FRAMEBUFFER, m_licFbo);
    glViewport(0, 0, w, h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    const float clearLic[4] = {-1.f, -1.f, -1.f, -1.f};
    glClearBufferfv(GL_COLOR, 0, clearLic);
    glUseProgram(m_convolveProgram);
    glBindTextureUnit(0, m_flowTex);
    glBindTextureUnit(1, m_noiseTex);
    if (m_convTexelSizeLoc != -1) glUniform2f(m_convTexelSizeLoc, 1.0f / static_cast<float>(w), 1.0f / static_cast<float>(h));
    int taps = std::clamp(state.licSteps, LicLimits::StepsMin, LicLimits::StepsMax);
    const int frequencyPasses = state.licEnhanced ? 2 : 1;
    const int budget = std::max(1, LicLimits::MaxTapsPerDirection / frequencyPasses);
    taps = std::max(LicLimits::StepsMin, std::min(taps, budget));
    if (m_convStepsLoc != -1) glUniform1i(m_convStepsLoc, taps);
    if (m_convStepSizeLoc != -1) glUniform1f(m_convStepSizeLoc, std::clamp(state.licStepSize, LicLimits::StepSizeMin, LicLimits::StepSizeMax));
    if (m_convNoiseFreqLoc != -1) glUniform1f(m_convNoiseFreqLoc, std::clamp(state.licNoiseFreq, LicLimits::NoiseFreqMin, LicLimits::NoiseFreqMax));
    if (m_convEnhancedLoc != -1) glUniform1i(m_convEnhancedLoc, state.licEnhanced ? 1 : 0);
    drawFullscreen();

    glBindFramebuffer(GL_FRAMEBUFFER, srcFbo);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glEnable(GL_DEPTH_TEST);
    glUseProgram(0);
    return m_licTex.get();
}

void SurfaceLicPass::shutdown() {
    m_flowProgram.reset();
    m_convolveProgram.reset();
    m_flowFbo.reset(); m_flowTex.reset(); m_flowDepth.reset();
    m_licFbo.reset(); m_licTex.reset();
    m_noiseTex.reset(); m_dummyVao.reset();
    m_w = m_h = 0; m_noiseGrain = 0;
}
