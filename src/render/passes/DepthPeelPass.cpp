#include "render/passes/DepthPeelPass.h"
#include "render/foundation/renderer.h"
#include "render/foundation/shader_utils.h"
#include "render/passes/ColormapManager.h"
#include <QDebug>
#include <algorithm>
#include <cmath>

void DepthPeelPass::init(const ShaderSources& sources) {
    // Query texture unit limit once; fallback iterative composite if needed for 64 layers
    GLint maxUnits = 0;
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &maxUnits);
    m_maxTextureUnits = maxUnits > 0 ? maxUnits : 32;

    if (!sources.depthPeelVert.empty() && !sources.depthPeelFrag.empty()) {
        const std::string fragFull = injectPbrCommon(sources.depthPeelFrag.c_str(), sources.pbrFragCommon);
        m_peelProgram.reset(compileProgram(sources.depthPeelVert.c_str(), fragFull.c_str(), "DepthPeel"));
        if (m_peelProgram.has()) {
            GLuint idx = glGetUniformBlockIndex(m_peelProgram, "MeshUBO");
            if (idx != GL_INVALID_INDEX) glUniformBlockBinding(m_peelProgram, idx, 0);
            m_peelUboIndex = static_cast<GLint>(idx);
            m_peelPrevDepthLoc = glGetUniformLocation(m_peelProgram, "uPrevDepth");
            m_peelLayerLoc     = glGetUniformLocation(m_peelProgram, "uLayerIndex");
            m_peelLutLoc       = glGetUniformLocation(m_peelProgram, "uLUT");
            if (m_peelLutLoc < 0) m_peelLutLoc = glGetUniformLocation(m_peelProgram, "uColormapLUT");
            m_peelNumBandsLoc  = glGetUniformLocation(m_peelProgram, "uNumBands");
        }
        // Crinkle-clip variant: same vert/frag but with mesh_clip.geo to cull whole triangles
        if (!sources.meshClipGeo.empty()) {
            m_peelClipProgram.reset(compileProgramWithGS(
                sources.depthPeelVert.c_str(), sources.meshClipGeo.c_str(), fragFull.c_str(), "DepthPeelCrinkle"));
            if (m_peelClipProgram.has()) {
                GLuint idx = glGetUniformBlockIndex(m_peelClipProgram, "MeshUBO");
                if (idx != GL_INVALID_INDEX) glUniformBlockBinding(m_peelClipProgram, idx, 0);
                m_peelClipUboIndex = static_cast<GLint>(idx);
                m_peelClipPrevDepthLoc = glGetUniformLocation(m_peelClipProgram, "uPrevDepth");
                m_peelClipLayerLoc     = glGetUniformLocation(m_peelClipProgram, "uLayerIndex");
                m_peelClipLutLoc       = glGetUniformLocation(m_peelClipProgram, "uLUT");
                if (m_peelClipLutLoc < 0) m_peelClipLutLoc = glGetUniformLocation(m_peelClipProgram, "uColormapLUT");
                m_peelClipNumBandsLoc  = glGetUniformLocation(m_peelClipProgram, "uNumBands");
            }
        }
    }
    if (!sources.compositeVert.empty() && !sources.compositeFrag.empty()) {
        m_compositeProgram.reset(compileProgram(sources.compositeVert.c_str(), sources.compositeFrag.c_str(), "Composite"));
        if (m_compositeProgram.has()) {
            m_peelNumLayersLoc = glGetUniformLocation(m_compositeProgram, "uNumLayers");
            if (m_peelNumLayersLoc < 0) m_peelNumLayersLoc = glGetUniformLocation(m_compositeProgram, "numLayers");
            m_compositeLayersLoc = glGetUniformLocation(m_compositeProgram, "uLayers[0]");
            if (m_compositeLayersLoc < 0) m_compositeLayersLoc = glGetUniformLocation(m_compositeProgram, "uLayers");
        }
    }
    // Iterative blit program for >maxUnits layers (avoids sampler array limit)
    {
        const char* vertSrc = sources.compositeVert.empty() ? nullptr : sources.compositeVert.c_str();
        // Fallback vert if compositeVert not yet loaded (should not happen in normal init)
        const char* fallbackVert = "#version 460 core\n out vec2 vUV; void main(){ float x=float(gl_VertexID &1)*4.0-1.0; float y=float((gl_VertexID>>1)&1)*4.0-1.0; vUV=vec2(x,y)*0.5+0.5; gl_Position=vec4(x,y,1.0,1.0); }";
        if (!vertSrc) vertSrc = fallbackVert;
        const char* blitFrag = "#version 460 core\n uniform sampler2D uLayer; in vec2 vUV; out vec4 FragColor; void main(){ vec4 c=texture(uLayer,vUV); FragColor=vec4(c.rgb*c.a,c.a); }";
        m_blitProgram.reset(compileProgram(vertSrc, blitFrag, "CompositeBlit"));
        if (m_blitProgram.has()) {
            m_blitLayerLoc = glGetUniformLocation(m_blitProgram, "uLayer");
        }
    }
}

void DepthPeelPass::shutdown() {
    destroyPeelFbos();
    m_peelDummyVao.reset();
    m_peelProgram.reset();
    m_peelClipProgram.reset();
    m_compositeProgram.reset();
    m_blitProgram.reset();
    m_peelPrevDepthLoc = m_peelClipPrevDepthLoc = m_peelLayerLoc = m_peelClipLayerLoc = -1;
    m_peelLutLoc = m_peelClipLutLoc = m_peelNumBandsLoc = m_peelClipNumBandsLoc = -1;
    m_peelUboIndex = m_peelClipUboIndex = -1;
    m_peelNumLayersLoc = m_compositeLayersLoc = m_blitLayerLoc = -1;
    m_maxTextureUnits = 0;
}

void DepthPeelPass::reinitForNewContext() {
    m_peelProgram.reset(); m_peelClipProgram.reset(); m_compositeProgram.reset(); m_blitProgram.reset();
    m_peelPrevDepthLoc = m_peelClipPrevDepthLoc = m_peelLayerLoc = m_peelClipLayerLoc = -1;
    m_peelLutLoc = m_peelClipLutLoc = m_peelNumBandsLoc = m_peelClipNumBandsLoc = -1;
    m_peelUboIndex = m_peelClipUboIndex = -1;
    m_peelNumLayersLoc = m_compositeLayersLoc = m_blitLayerLoc = -1;
    m_maxTextureUnits = 0;
    for (auto& f : m_peelFbo) f.reset();
    for (auto& t : m_peelColorTex) t.reset();
    for (auto& t : m_peelDepthTex) t.reset();
    m_peelMainDepth.reset(); m_peelDummyVao.reset();
    m_peelFboW = m_peelFboH = m_peelSamples = 0; m_peelAllocated = 0;
}

void DepthPeelPass::ensurePeelFbos(int w, int h, int samples, int needLayers) {
    needLayers = std::clamp(needLayers, 1, kMaxPeelLayers);
    if (m_peelFboW == w && m_peelFboH == h && m_peelSamples == samples && m_peelAllocated >= needLayers && m_peelFbo[0].has() && m_peelDummyVao.has()) return;
    // Need realloc: destroy all and recreate up to needLayers (lazy allocation)
    for (int i = 0; i < kMaxPeelLayers; ++i) {
        m_peelFbo[i].reset(); m_peelDepthTex[i].reset(); m_peelColorTex[i].reset();
    }
    m_peelMainDepth.reset();
    m_peelFboW = w; m_peelFboH = h; m_peelSamples = samples; m_peelAllocated = needLayers;
    // VRAM estimate: color RGBA8 (4B) + depth32F (4+1B) ~8B per pixel per layer
    {
        double mb = double(w) * double(h) * double(needLayers) * 8.0 / (1024.0*1024.0);
        if (mb > 512.0) {
            qWarning() << "[DepthPeel] large allocation:" << needLayers << "layers at" << w << "x" << h << "~" << int(mb) << "MB";
        }
    }
    auto createStorage = [&](GLuint tex, GLenum internalFormat, GLsizei ww, GLsizei hh) {
        glTextureStorage2D(tex, 1, internalFormat, ww, hh);
        glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    GLenum depthFormat = GLAD_GL_ARB_depth_buffer_float ? GL_DEPTH32F_STENCIL8 : GL_DEPTH24_STENCIL8;
    for (int i = 0; i < needLayers; ++i) {
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
    m_peelMainDepth.reset();
    // Do not destroy m_peelDummyVao here - it is cheap and needed across resizes;
    // only shutdown()/reinitForNewContext() destroys it.
    m_peelFboW = m_peelFboH = 0; m_peelAllocated = 0;
}

void DepthPeelPass::renderTransparent(int vpW, int vpH,
                                      const RenderRenderState& state,
                                      GLuint meshUbo,
                                      const ColormapManager& colormap,
                                      const std::vector<std::pair<GLuint,int>>& transparentMeshes) {
    if ((!m_peelProgram.has() && !m_peelClipProgram.has()) || (!m_compositeProgram.has() && !m_blitProgram.has()) || transparentMeshes.empty()) return;
    // Guard entire peel+composite to restore cull/blend/depth/sRGB/viewport even on early return
    GLStateGuard guard;
    GLint samples = 0; glGetIntegerv(GL_SAMPLES, &samples);
    // Compute required layers early to allow lazy FBO allocation
    int numLayers = std::clamp(state.maxPeelLayers, 1, kMaxPeelLayers);
    if (state.maxPeelLayers > 1) {
        float resolutionFactor = std::sqrt(static_cast<float>(vpW * vpH)) / 2000.0f;
        int maxByRes = std::max(1, static_cast<int>(kMaxPeelLayers / resolutionFactor));
        int dynamicCap = std::min(state.maxPeelLayers, maxByRes);
        int capped = std::max(1, std::min(numLayers, dynamicCap));
        if (capped != numLayers) {
            qDebug() << "[DepthPeel] layers capped" << numLayers << "->" << capped
                     << "at" << vpW << "x" << vpH << "(MSAA samples" << samples << ")";
        }
        numLayers = capped;
    }
    ensurePeelFbos(vpW, vpH, samples, numLayers);
    GLint prevFbo; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    // Capture opaque depth into m_peelMainDepth (single-sample). If default is MS, blit resolves.
    {
        GlFramebuffer tempFbo; glCreateFramebuffers(1, tempFbo.ptr());
        glNamedFramebufferTexture(tempFbo, GL_DEPTH_STENCIL_ATTACHMENT, m_peelMainDepth, 0);
        glNamedFramebufferDrawBuffer(tempFbo, GL_NONE);
        glNamedFramebufferReadBuffer(tempFbo, GL_NONE);
        if (glCheckNamedFramebufferStatus(tempFbo, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            qWarning() << "Peel tempFBO incomplete:" << Qt::hex << glCheckNamedFramebufferStatus(tempFbo, GL_FRAMEBUFFER);
        } else {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, tempFbo);
            glBlitFramebuffer(0,0,vpW,vpH,0,0,vpW,vpH, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    }
    // Peel draws are single-sample: disable multisample to avoid per-sample waste and ensure
    // deterministic depth for the 1e-5 epsilon test. Restored by GLStateGuard not covering
    // ENABLE_BIT for MULTISAMPLE, so save/restore manually.
    GLboolean wasMultisample = glIsEnabled(GL_MULTISAMPLE);
    if (wasMultisample) glDisable(GL_MULTISAMPLE);

    // Respect cull mode from state (MeshPass::drawOpaque does the same)
    if (state.cullMode != 0) {
        glEnable(GL_CULL_FACE);
        glCullFace(state.cullMode == 2 ? GL_FRONT : GL_BACK);
    } else {
        glDisable(GL_CULL_FACE);
    }
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS); glDepthMask(GL_TRUE);
    glDisable(GL_FRAMEBUFFER_SRGB);

    const bool useCrinkle = state.crinkleClipMode && state.clipEnabled && m_peelClipProgram.has();
    GlProgram& peelProg = useCrinkle ? m_peelClipProgram : m_peelProgram;
    GLint prevDepthLoc = useCrinkle ? m_peelClipPrevDepthLoc : m_peelPrevDepthLoc;
    GLint layerLoc     = useCrinkle ? m_peelClipLayerLoc     : m_peelLayerLoc;
    GLint lutLoc       = useCrinkle ? m_peelClipLutLoc       : m_peelLutLoc;
    GLint numBandsLoc  = useCrinkle ? m_peelClipNumBandsLoc  : m_peelNumBandsLoc;

    glUseProgram(peelProg);
    // UBO binding already set via glUniformBlockBinding but need buffer base
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, meshUbo);
    if (prevDepthLoc != -1) glUniform1i(prevDepthLoc, 0);
    if (numBandsLoc != -1) glUniform1f(numBandsLoc, static_cast<float>(state.scalarColorBands));
    if (state.meshHasScalars && state.meshUseScalarColor && colormap.scalarTexture() != 0) {
        glBindTextureUnit(2, colormap.scalarTexture());
        if (lutLoc != -1) glUniform1i(lutLoc, 2);
    }
    for (int layer = 0; layer < numLayers; ++layer) {
        if (layerLoc != -1) glUniform1i(layerLoc, layer);
        glBindFramebuffer(GL_FRAMEBUFFER, m_peelFbo[layer]);
        glViewport(0,0,vpW,vpH);
        glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (layer == 0) glBindTextureUnit(0, m_peelMainDepth);
        else glBindTextureUnit(0, m_peelDepthTex[layer-1]);
        for (auto &mesh : transparentMeshes) { glBindVertexArray(mesh.first); glDrawElements(GL_TRIANGLES, mesh.second, GL_UNSIGNED_INT, 0); }
    }
    glBindVertexArray(0); glUseProgram(0);

    // Composite back to display FBO (prevFbo). StateGuard saved viewport/program/blend etc,
    // but we need to re-establish composite state explicitly.
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    glViewport(0,0,vpW,vpH);
    glEnable(GL_FRAMEBUFFER_SRGB);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    if (wasMultisample) glEnable(GL_MULTISAMPLE);

    if (!m_peelDummyVao.has()) glCreateVertexArrays(1, m_peelDummyVao.ptr());
    glDepthMask(GL_FALSE);

    // Use single-pass composite when within sampler limit (composite shader declares 32).
    // Beyond 32, use iterative blit to avoid exceeding GL_MAX_TEXTURE_IMAGE_UNITS (typically 32).
    const int kCompositeCapacity = 32;
    const bool useSinglePass = (numLayers <= kCompositeCapacity) && (numLayers <= m_maxTextureUnits) && m_compositeProgram.has();
    if (useSinglePass) {
        glUseProgram(m_compositeProgram);
        GLint texUnits[kMaxPeelLayers];
        for (int i=0;i<numLayers;++i){ glBindTextureUnit(i, m_peelColorTex[i]); texUnits[i]=i; }
        if (m_compositeLayersLoc != -1) glUniform1iv(m_compositeLayersLoc, numLayers, texUnits);
        else {
            GLint loc = glGetUniformLocation(m_compositeProgram, "uLayers[0]");
            if (loc != -1) glUniform1iv(loc, numLayers, texUnits);
        }
        if (m_peelNumLayersLoc != -1) glUniform1i(m_peelNumLayersLoc, numLayers);
        glBindVertexArray(m_peelDummyVao); glDrawArrays(GL_TRIANGLES,0,3); glBindVertexArray(0);
        glUseProgram(0);
    } else {
        // Iterative back-to-front blend: each layer premultiplied, blended with ONE / ONE_MINUS_SRC_ALPHA.
        // No sampler array limit; works for any N up to 64.
        if (m_blitProgram.has()) {
            glUseProgram(m_blitProgram);
            if (m_blitLayerLoc != -1) glUniform1i(m_blitLayerLoc, 0);
            for (int i = numLayers - 1; i >= 0; --i) {
                glBindTextureUnit(0, m_peelColorTex[i]);
                glBindVertexArray(m_peelDummyVao); glDrawArrays(GL_TRIANGLES,0,3);
            }
            glBindVertexArray(0);
            glUseProgram(0);
        } else if (m_compositeProgram.has()) {
            // Fallback to single-pass even if over limit (may fail on low-end drivers)
            glUseProgram(m_compositeProgram);
            GLint texUnits[kMaxPeelLayers];
            for (int i=0;i<numLayers;++i){ glBindTextureUnit(i, m_peelColorTex[i]); texUnits[i]=i; }
            if (m_compositeLayersLoc != -1) glUniform1iv(m_compositeLayersLoc, numLayers, texUnits);
            if (m_peelNumLayersLoc != -1) glUniform1i(m_peelNumLayersLoc, numLayers);
            glBindVertexArray(m_peelDummyVao); glDrawArrays(GL_TRIANGLES,0,3); glBindVertexArray(0);
            glUseProgram(0);
        }
    }
    glDepthMask(GL_TRUE);
    // GLStateGuard destructor restores cull/blend/depth/sRGB/viewport/program on exit
}
