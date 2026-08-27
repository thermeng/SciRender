#include "render/passes/DepthPeelPass.h"
#include "render/foundation/renderer.h"
#include "render/foundation/shader_utils.h"
#include "render/passes/ColormapManager.h"
#include <QDebug>
#include <algorithm>
#include <cmath>

void DepthPeelPass::init(const ShaderSources& sources) {
    if (!sources.depthPeelVert.empty() && !sources.depthPeelFrag.empty()) {
        const std::string fragFull = injectPbrCommon(sources.depthPeelFrag.c_str(), sources.pbrFragCommon);
        m_peelProgram.reset(compileProgram(sources.depthPeelVert.c_str(), fragFull.c_str(), "DepthPeel"));
        if (m_peelProgram.has()) {
            m_peelPrevDepthLoc = glGetUniformLocation(m_peelProgram, "uPrevDepth");
            m_peelLayerLoc     = glGetUniformLocation(m_peelProgram, "uLayerIndex");
            m_peelLutLoc       = glGetUniformLocation(m_peelProgram, "uLUT");
            if (m_peelLutLoc < 0) m_peelLutLoc = glGetUniformLocation(m_peelProgram, "uColormapLUT");
            m_peelNumBandsLoc  = glGetUniformLocation(m_peelProgram, "uNumBands");
        }
    }
    if (!sources.compositeVert.empty() && !sources.compositeFrag.empty()) {
        m_compositeProgram.reset(compileProgram(sources.compositeVert.c_str(), sources.compositeFrag.c_str(), "Composite"));
        if (m_compositeProgram.has()) {
            m_peelNumLayersLoc = glGetUniformLocation(m_compositeProgram, "uNumLayers");
            if (m_peelNumLayersLoc < 0) m_peelNumLayersLoc = glGetUniformLocation(m_compositeProgram, "numLayers");
        }
    }
}

void DepthPeelPass::shutdown() {
    destroyPeelFbos();
    m_peelProgram.reset();
    m_compositeProgram.reset();
    m_peelPrevDepthLoc = m_peelLayerLoc = m_peelLutLoc = m_peelNumBandsLoc = m_peelNumLayersLoc = -1;
}

void DepthPeelPass::reinitForNewContext() {
    m_peelProgram.reset(); m_compositeProgram.reset();
    m_peelPrevDepthLoc = m_peelLayerLoc = m_peelLutLoc = m_peelNumBandsLoc = m_peelNumLayersLoc = -1;
    for (auto& f : m_peelFbo) f.reset();
    for (auto& t : m_peelColorTex) t.reset();
    for (auto& t : m_peelDepthTex) t.reset();
    m_peelMainDepth.reset(); m_peelDummyVao.reset();
    m_peelFboW = m_peelFboH = m_peelSamples = 0;
}

void DepthPeelPass::ensurePeelFbos(int w, int h, int samples) {
    if (m_peelFboW == w && m_peelFboH == h && m_peelSamples == samples && m_peelFbo[0].has()) return;
    destroyPeelFbos();
    m_peelFboW = w; m_peelFboH = h; m_peelSamples = samples;
    auto createStorage = [&](GLenum target, GLenum internalFormat, GLsizei ww, GLsizei hh) {
        glTextureStorage2D(target, 1, internalFormat, ww, hh);
        glTextureParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTextureParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    GLenum depthFormat = GLAD_GL_ARB_depth_buffer_float ? GL_DEPTH32F_STENCIL8 : GL_DEPTH24_STENCIL8;
    for (int i = 0; i < kMaxPeelLayers; ++i) {
        glCreateFramebuffers(1, m_peelFbo[i].ptr());
        glCreateTextures(GL_TEXTURE_2D, 1, m_peelColorTex[i].ptr());
        createStorage(m_peelColorTex[i], GL_RGBA8, w, h);
        glCreateTextures(GL_TEXTURE_2D, 1, m_peelDepthTex[i].ptr());
        createStorage(m_peelDepthTex[i], depthFormat, w, h);
        glNamedFramebufferTexture(m_peelFbo[i], GL_COLOR_ATTACHMENT0, m_peelColorTex[i], 0);
        glNamedFramebufferTexture(m_peelFbo[i], GL_DEPTH_STENCIL_ATTACHMENT, m_peelDepthTex[i], 0);
        if (glCheckNamedFramebufferStatus(m_peelFbo[i], GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            qWarning() << "Peel FBO" << i << "incomplete: 0x" << Qt::hex << glCheckNamedFramebufferStatus(m_peelFbo[i], GL_FRAMEBUFFER);
        }
    }
    glCreateTextures(GL_TEXTURE_2D, 1, m_peelMainDepth.ptr());
    createStorage(m_peelMainDepth, depthFormat, w, h);
    if (!m_peelDummyVao.has()) glCreateVertexArrays(1, m_peelDummyVao.ptr());
}

void DepthPeelPass::destroyPeelFbos() {
    for (int i = 0; i < kMaxPeelLayers; ++i) {
        m_peelFbo[i].reset(); m_peelDepthTex[i].reset(); m_peelColorTex[i].reset();
    }
    m_peelMainDepth.reset(); m_peelDummyVao.reset();
    m_peelFboW = m_peelFboH = 0;
}

void DepthPeelPass::renderTransparent(int vpW, int vpH,
                                      const RenderRenderState& state,
                                      GLuint meshUbo,
                                      const ColormapManager& colormap,
                                      const std::vector<std::pair<GLuint,int>>& transparentMeshes) {
    if (!m_peelProgram.has() || !m_compositeProgram.has() || transparentMeshes.empty()) return;
    GLint samples = 0; glGetIntegerv(GL_SAMPLES, &samples);
    ensurePeelFbos(vpW, vpH, samples);
    GLint prevFbo; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    {
        GlFramebuffer tempFbo; glCreateFramebuffers(1, tempFbo.ptr());
        glNamedFramebufferTexture(tempFbo, GL_DEPTH_STENCIL_ATTACHMENT, m_peelMainDepth, 0);
        glNamedFramebufferDrawBuffer(tempFbo, GL_NONE);
        glNamedFramebufferReadBuffer(tempFbo, GL_NONE);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, tempFbo);
        glBlitFramebuffer(0,0,vpW,vpH,0,0,vpW,vpH, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
    }
    glDisable(GL_CULL_FACE); glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS); glDepthMask(GL_TRUE);
    glDisable(GL_FRAMEBUFFER_SRGB);
    int numLayers = std::clamp(state.maxPeelLayers, 1, kMaxPeelLayers);
    if (state.maxPeelLayers > 1) {
        float resolutionFactor = std::sqrt(static_cast<float>(vpW * vpH)) / 2000.0f;
        int maxByRes = std::max(1, static_cast<int>(kMaxPeelLayers / resolutionFactor));
        int dynamicCap = std::min(state.maxPeelLayers, maxByRes);
        numLayers = std::max(1, std::min(numLayers, dynamicCap));
    }
    glUseProgram(m_peelProgram);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, meshUbo);
    glUniform1i(m_peelPrevDepthLoc, 0);
    if (m_peelNumBandsLoc != -1) glUniform1f(m_peelNumBandsLoc, static_cast<float>(state.scalarColorBands));
    if (state.meshHasScalars && state.meshUseScalarColor && colormap.scalarTexture() != 0) {
        glBindTextureUnit(2, colormap.scalarTexture());
        glUniform1i(m_peelLutLoc, 2);
    }
    for (int layer = 0; layer < numLayers; ++layer) {
        glUniform1i(m_peelLayerLoc, layer);
        glBindFramebuffer(GL_FRAMEBUFFER, m_peelFbo[layer]);
        glViewport(0,0,vpW,vpH);
        glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (layer == 0) glBindTextureUnit(0, m_peelMainDepth);
        else glBindTextureUnit(0, m_peelDepthTex[layer-1]);
        for (auto &mesh : transparentMeshes) { glBindVertexArray(mesh.first); glDrawElements(GL_TRIANGLES, mesh.second, GL_UNSIGNED_INT, 0); }
    }
    glBindVertexArray(0); glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
    glViewport(0,0,vpW,vpH);
    glEnable(GL_FRAMEBUFFER_SRGB); glDisable(GL_DEPTH_TEST); glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(m_compositeProgram);
    GLint texUnits[kMaxPeelLayers];
    for (int i=0;i<numLayers;++i){ glBindTextureUnit(i, m_peelColorTex[i]); texUnits[i]=i; }
    glUniform1iv(glGetUniformLocation(m_compositeProgram, "uLayers[0]"), numLayers, texUnits);
    glUniform1i(m_peelNumLayersLoc, numLayers);
    glDepthMask(GL_FALSE); glBindVertexArray(m_peelDummyVao); glDrawArrays(GL_TRIANGLES,0,3); glBindVertexArray(0); glDepthMask(GL_TRUE);
    glUseProgram(0); glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS); glDisable(GL_CULL_FACE);
    glViewport(0,0,vpW,vpH);
}
