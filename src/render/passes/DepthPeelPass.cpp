#include "render/passes/DepthPeelPass.h"
#include "render/foundation/renderer.h"
#include "render/foundation/shader_utils.h"
#include "render/passes/ColormapManager.h"
#include <QDebug>
#include <algorithm>

// ---------------------------------------------------------------------------
// Dual depth peeling.
//
// Per frame, the whole sequence is:
//
//   1. Seed both depth-range ping-pong buffers from the opaque scene depth.
//      Pixels with no opaque geometry are discarded, leaving the (-1,-1)
//      sentinel that means "nothing translucent resolved yet".
//
//   2. INITIALIZE pass: draw translucent geometry once with blending off.
//      Each pixel keeps the nearest translucent depth (and the opaque depth as
//      the far bound if the surface is occluded). Establishes the first
//      outstanding range.
//
//   3. PEELING loop, N iterations:
//        a. clear backTemp to zero, clear the front destination to zero, and
//           reset the depth-range destination to the (-1,-1) sentinel
//        b. draw translucent geometry with GL_BLEND + GL_MAX into
//           {backTemp, frontDest, depthDest}. Because every destination was
//           just cleared to zero (or -1) and all written values are
//           non-negative, MAX(x, 0) == x -- so this is effectively a plain
//           write, while the negated near depth turns "nearest wins" into MAX.
//        c. blend backTemp into back with GL_ONE / GL_ONE_MINUS_SRC_ALPHA.
//           Over-blend on premultiplied colour, so this is back-to-front
//           accumulation done by fixed function.
//        d. swap the front and depth ping-pongs
//
//   4. ALPHA mop-up: fragments still inside the outstanding range get one
//      plain alpha-blended pass into `back`. Deferring this to the end is
//      cheaper than running it on the first pass that writes nothing, and is
//      equivalent because those fragments are blended in the same order
//      regardless of when the pass runs.
//
//   5. Resolve: blit `back` onto the caller's FBO with the same blend state.
//
// The user-facing alpha is LINEAR per layer. Layer accumulation is inherently
// 1-(1-a)^N; that is the algorithm, not a bug.
// ---------------------------------------------------------------------------

void DepthPeelPass::queryUniforms(bool isClip) {
    GlProgram& prog = isClip ? m_peelClipProgram : m_peelProgram;
    GLint& opaque   = isClip ? m_peelClipOpaqueDepthLoc : m_peelOpaqueDepthLoc;
    GLint& depthSrc = isClip ? m_peelClipDepthSourceLoc : m_peelDepthSourceLoc;
    GLint& frontSrc = isClip ? m_peelClipFrontSourceLoc : m_peelFrontSourceLoc;
    GLint& lut      = isClip ? m_peelClipLutLoc         : m_peelLutLoc;
    GLint& bands    = isClip ? m_peelClipNumBandsLoc    : m_peelNumBandsLoc;
    GLint& stage    = isClip ? m_peelClipStageLoc       : m_peelStageLoc;

    if (!prog.has()) {
        opaque = depthSrc = frontSrc = lut = bands = stage = -1;
        return;
    }
    opaque   = glGetUniformLocation(prog, "uOpaqueDepth");
    depthSrc = glGetUniformLocation(prog, "uDepthSource");
    frontSrc = glGetUniformLocation(prog, "uFrontSource");
    lut      = glGetUniformLocation(prog, "uLUT");
    if (lut < 0) lut = glGetUniformLocation(prog, "uColormapLUT");
    bands    = glGetUniformLocation(prog, "uNumBands");
    stage    = glGetUniformLocation(prog, "uStage");
}

void DepthPeelPass::init(const ShaderSources& sources) {
    // The peel fragment shader is injected with the shared PBR library.
    const std::string fragFull = injectPbrCommon(sources.depthPeelFrag.c_str(), sources.pbrFragCommon);

    if (!sources.depthPeelVert.empty() && !sources.depthPeelFrag.empty()) {
        m_peelProgram.reset(compileProgram(sources.depthPeelVert.c_str(), fragFull.c_str(), "DepthPeel"));
        queryUniforms(false);

        // Crinkle-clip variant: same vert/frag, plus mesh_clip.geo to cull
        // whole triangles instead of per-fragment.
        if (!sources.meshClipGeo.empty()) {
            m_peelClipProgram.reset(compileProgramWithGS(
                sources.depthPeelVert.c_str(), sources.meshClipGeo.c_str(), fragFull.c_str(), "DepthPeelCrinkle"));
            queryUniforms(true);
        }
    }

    // Fullscreen helper: seeds depth ranges and resolves the back buffer.
    if (!sources.compositeVert.empty() && !sources.peelBlitFrag.empty()) {
        m_blitProgram.reset(compileProgram(
            sources.compositeVert.c_str(), sources.peelBlitFrag.c_str(), "PeelBlit"));
        if (m_blitProgram.has()) {
            m_blitSrcLoc = glGetUniformLocation(m_blitProgram, "uSrc");
            m_blitBackLoc = glGetUniformLocation(m_blitProgram, "uBack");
            m_blitModeLoc = glGetUniformLocation(m_blitProgram, "uMode");
            m_blitClearValueLoc = glGetUniformLocation(m_blitProgram, "uClearValue");
        }
    }

    if (!m_peelProgram.has() && !m_peelClipProgram.has()) {
        qWarning() << "[DepthPeel] no peel program compiled; renderer will fall back";
    }
}

void DepthPeelPass::shutdown() {
    destroyTargets();
    m_dummyVao.reset();
    m_peelProgram.reset();
    m_peelClipProgram.reset();
    m_blitProgram.reset();
}

void DepthPeelPass::reinitForNewContext() {
    destroyTargets();
    m_peelProgram.reset(); m_peelClipProgram.reset(); m_blitProgram.reset();
    // GL objects are context-owned; the query is recreated lazily in
    // ensureTargets, so just drop the handle here.
    m_queryId = 0;
}

void DepthPeelPass::ensureTargets(int w, int h) {
    if (m_width == w && m_height == h && m_back.has()) return;
    destroyTargets();

    glCreateFramebuffers(1, m_fbo.ptr());
    for (int i = 0; i < 2; ++i) {
        glCreateTextures(GL_TEXTURE_2D, 1, m_depthRange[i].ptr());
        glTextureStorage2D(m_depthRange[i], 1, GL_RG32F, w, h);
        glTextureParameteri(m_depthRange[i], GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(m_depthRange[i], GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTextureParameteri(m_depthRange[i], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(m_depthRange[i], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glCreateTextures(GL_TEXTURE_2D, 1, m_front[i].ptr());
        glTextureStorage2D(m_front[i], 1, GL_RGBA8, w, h);
        glTextureParameteri(m_front[i], GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(m_front[i], GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTextureParameteri(m_front[i], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(m_front[i], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glCreateTextures(GL_TEXTURE_2D, 1, m_back.ptr());
    glTextureStorage2D(m_back, 1, GL_RGBA8, w, h);
    glTextureParameteri(m_back, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(m_back, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTextureParameteri(m_back, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_back, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glCreateTextures(GL_TEXTURE_2D, 1, m_backTemp.ptr());
    glTextureStorage2D(m_backTemp, 1, GL_RGBA8, w, h);
    glTextureParameteri(m_backTemp, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(m_backTemp, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTextureParameteri(m_backTemp, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_backTemp, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Opaque depth: single-sample. The display FBO may be multisampled; a
    // NEAREST depth blit is the only resolve GL offers.
    GLenum depthFormat = GLAD_GL_ARB_depth_buffer_float ? GL_DEPTH32F_STENCIL8 : GL_DEPTH24_STENCIL8;
    glCreateTextures(GL_TEXTURE_2D, 1, m_opaqueDepth.ptr());
    glTextureStorage2D(m_opaqueDepth, 1, depthFormat, w, h);
    glTextureParameteri(m_opaqueDepth, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(m_opaqueDepth, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTextureParameteri(m_opaqueDepth, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_opaqueDepth, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // Must stay GL_NONE: this is sampled as a normal depth texture.
    glTextureParameteri(m_opaqueDepth, GL_TEXTURE_COMPARE_MODE, GL_NONE);

    glCreateVertexArrays(1, m_dummyVao.ptr());
    if (!m_queryId) glGenQueries(1, &m_queryId);
    m_width = w; m_height = h;
}

void DepthPeelPass::destroyTargets() {
    m_fbo.reset();
    m_back.reset(); m_backTemp.reset(); m_opaqueDepth.reset();
    for (int i = 0; i < 2; ++i) { m_depthRange[i].reset(); m_front[i].reset(); }
    m_width = m_height = 0;
}

void DepthPeelPass::attachTargets(const GLuint* textures, int count) {
    for (int i = 0; i < 4; ++i) {
        if (i < count) glNamedFramebufferTexture(m_fbo, GL_COLOR_ATTACHMENT0 + i, textures[i], 0);
        else glNamedFramebufferTexture(m_fbo, GL_COLOR_ATTACHMENT0 + i, 0, 0);
    }
    const GLenum bufs[4] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1,
                             GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3 };
    glNamedFramebufferDrawBuffers(m_fbo, 4, bufs);
}

void DepthPeelPass::clearTargets(const GLuint* textures, int count) {
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    attachTargets(textures, count);
    glDisable(GL_BLEND);
    for (int i = 0; i < count; ++i) {
        // Depth-range sentinel: near = -1 (nothing resolved), far = -1.
        if (textures[i] == m_depthRange[0] || textures[i] == m_depthRange[1]) {
            const GLfloat sentinel[4] = { -1.0f, -1.0f, 0.0f, 0.0f };
            glClearBufferfv(GL_COLOR, i, sentinel);
        } else {
            glClearBufferfv(GL_COLOR, i, (const GLfloat[4]){ 0, 0, 0, 0 });
        }
    }
}

void DepthPeelPass::drawFullscreen() {
    glBindVertexArray(m_dummyVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void DepthPeelPass::drawTranslucent(GLuint vao, int indexCount) {
    glBindVertexArray(vao);
    glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, 0);
}

void DepthPeelPass::renderTransparent(int vpW, int vpH,
                                      const RenderRenderState& state,
                                      GLuint meshUbo,
                                      const ColormapManager& colormap,
                                      const std::vector<std::pair<GLuint,int>>& transparentMeshes) {
    if (transparentMeshes.empty()) return;
    const bool havePeel = m_peelProgram.has() || m_peelClipProgram.has();
    if (!havePeel || !m_blitProgram.has()) {
        qWarning() << "[DepthPeel] programs unavailable; renderer falls back to "
                      "inline alpha blending so the surface is never dropped";
        return;
    }

    GLStateGuard guard;

    // Peel count is in ITERATIONS; each resolves up to 2 surfaces.
    int maxIters = std::clamp(state.maxPeelLayers, 1, kMaxPeelIterations);
    ensureTargets(vpW, vpH);
    if (!m_back.has()) return;   // allocation failed; bail rather than draw garbage

    GLint prevFbo;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);

    const bool useCrinkle = state.crinkleClipMode && state.clipEnabled && m_peelClipProgram.has();
    GlProgram& prog = useCrinkle ? m_peelClipProgram : m_peelProgram;
    const GLint opaqueLoc   = useCrinkle ? m_peelClipOpaqueDepthLoc   : m_peelOpaqueDepthLoc;
    const GLint depthSrcLoc = useCrinkle ? m_peelClipDepthSourceLoc   : m_peelDepthSourceLoc;
    const GLint frontSrcLoc = useCrinkle ? m_peelClipFrontSourceLoc   : m_peelFrontSourceLoc;
    const GLint lutLoc      = useCrinkle ? m_peelClipLutLoc           : m_peelLutLoc;
    const GLint bandsLoc    = useCrinkle ? m_peelClipNumBandsLoc      : m_peelNumBandsLoc;
    const GLint stageLoc    = useCrinkle ? m_peelClipStageLoc         : m_peelStageLoc;

    // ---- 1. Capture the opaque scene depth -------------------------------
    {
        GlFramebuffer tempFbo;
        glCreateFramebuffers(1, tempFbo.ptr());
        glNamedFramebufferTexture(tempFbo, GL_DEPTH_STENCIL_ATTACHMENT, m_opaqueDepth, 0);
        glNamedFramebufferDrawBuffer(tempFbo, GL_NONE);
        glNamedFramebufferReadBuffer(tempFbo, GL_NONE);
        if (glCheckNamedFramebufferStatus(tempFbo, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            qWarning() << "Peel tempFBO incomplete";
        } else {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, tempFbo);
            while (glGetError() != GL_NO_ERROR) {}   // drain prior-pass errors
            glBlitFramebuffer(0, 0, vpW, vpH, 0, 0, vpW, vpH, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
            const GLenum err = glGetError();
            if (err != GL_NO_ERROR) {
                qWarning() << "Peel opaque-depth blit failed (0x" << Qt::hex << err << Qt::dec
                           << ") - transparency will ignore opaque occlusion";
            }
        }
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    }

    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glViewport(0, 0, vpW, vpH);
    glDisable(GL_DEPTH_TEST);       // depth lives in RG32F colour, not a depth buffer
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);         // both sides are valid peel candidates

    glUseProgram(prog);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, meshUbo);
    if (bandsLoc != -1) glUniform1f(bandsLoc, static_cast<float>(state.scalarColorBands));

    GLuint lutTex = (state.meshHasScalars && state.meshUseScalarColor) ? colormap.scalarTexture() : 0;
    if (lutTex) {
        glBindTextureUnit(3, lutTex);
        if (lutLoc != -1) glUniform1i(lutLoc, 3);
    } else {
        if (lutLoc != -1) glUniform1i(lutLoc, 3);
    }
    if (opaqueLoc != -1) glUniform1i(opaqueLoc, 0);
    if (depthSrcLoc != -1) glUniform1i(depthSrcLoc, 1);
    if (frontSrcLoc != -1) glUniform1i(frontSrcLoc, 2);

    // ---- Seed both depth-range buffers from the opaque depth -------------
    // The seed blit DISCARDS pixels with no opaque geometry, so those pixels
    // would keep stale range data from the previous frame. Reset both to the
    // sentinel first: near = -1 ("nothing translucent resolved"), far = -1.
    {
        const GLuint targets[2] = { m_depthRange[kA], m_depthRange[kB] };
        glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
        attachTargets(targets, 2);
        glDisable(GL_BLEND);
        const GLfloat sentinel[4] = { -1.0f, -1.0f, 0.0f, 0.0f };
        glClearBufferfv(GL_COLOR, 0, sentinel);
        glClearBufferfv(GL_COLOR, 1, sentinel);

        glUseProgram(m_blitProgram);
        if (m_blitSrcLoc != -1) glUniform1i(m_blitSrcLoc, 0);
        if (m_blitModeLoc != -1) glUniform1i(m_blitModeLoc, 0);   // SEED_DEPTH
        if (m_blitClearValueLoc != -1) glUniform1f(m_blitClearValueLoc, 1.0f);
        glBindTextureUnit(0, m_opaqueDepth);
        drawFullscreen();
    }

    int srcSlot = kA, dstSlot = kB;

    // ---- 2. INITIALIZE ---------------------------------------------------
    {
        // All three attachments are bound even though only outDepth is
        // written: the shader's outputs have fixed locations, so binding fewer
        // would send location 2 nowhere.
        const GLuint targets[3] = { m_backTemp, m_front[dstSlot], m_depthRange[dstSlot] };
        attachTargets(targets, 3);

        // GL_MAX is REQUIRED here, not just in the peel loop. Every fragment
        // covering this pixel writes (-depth, depth), so MAX over the negated
        // near component selects the NEAREST and over the far component the
        // FARTHEST -- giving the full initial range in a single pass. With
        // blending disabled the result is whichever fragment happened to be
        // drawn last, which collapses a 20-plane stack to a single layer and
        // makes transparency look completely dead.
        glEnable(GL_BLEND);
        glBlendEquation(GL_MAX);

        glUseProgram(prog);
        if (stageLoc != -1) glUniform1i(stageLoc, kInitialize);
        if (opaqueLoc != -1) glUniform1i(opaqueLoc, 0);
        if (depthSrcLoc != -1) glUniform1i(depthSrcLoc, 1);
        if (frontSrcLoc != -1) glUniform1i(frontSrcLoc, 2);
        glBindTextureUnit(0, m_opaqueDepth);
        glBindTextureUnit(1, m_depthRange[srcSlot]);
        glBindTextureUnit(2, m_front[srcSlot]);
        for (const auto& m : transparentMeshes) drawTranslucent(m.first, m.second);
        std::swap(srcSlot, dstSlot);
    }

    // Clear the accumulators for the peel loop.
    {
        const GLuint clears[3] = { m_back, m_front[kA], m_front[kB] };
        clearTargets(clears, 3);
    }

    // ---- 3. PEELING loop -------------------------------------------------
    // Each iteration resolves up to two surfaces (nearest front + farthest
    // back), so maxIters iterations cover roughly 2*maxIters layers.
    //
    // The loop also terminates ITSELF via occlusion query (3d), so maxIters
    // is only a safety ceiling -- the user never has to tune it.
    //
    // peelOcclusionRatio is the fraction of the viewport a pass must still
    // cover to justify another pass. 0 means "never stop early" (exact,
    // slowest). A small value stops once remaining layers are sub-pixel.
    const float kOcclusionRatio = std::clamp(state.peelOcclusionRatio, 0.0f, 0.5f);
    const GLuint occlusionThreshold =
        static_cast<GLuint>(static_cast<double>(vpW) * static_cast<double>(vpH) * kOcclusionRatio);
    bool peelingDone = false;

    for (int pass = 0; pass < maxIters; ++pass) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
        glViewport(0, 0, vpW, vpH);

        // 3a. clear the per-iteration targets.
        {
            const GLuint clears[3] = { m_backTemp, m_front[dstSlot], m_depthRange[dstSlot] };
            attachTargets(clears, 3);
            glDisable(GL_BLEND);
            const GLfloat sentinel[4] = { -1.0f, -1.0f, 0.0f, 0.0f };
            glClearBufferfv(GL_COLOR, 0, (const GLfloat[4]){ 0, 0, 0, 0 });
            glClearBufferfv(GL_COLOR, 1, (const GLfloat[4]){ 0, 0, 0, 0 });
            glClearBufferfv(GL_COLOR, 2, sentinel);
        }

        // 3b. peel with GL_MAX blending on negated near depth.
        //     Wrapped in a SAMPLES_PASSED occlusion query: this counts the
        //     fragments that actually survived, which is how we know when
        //     further passes can no longer change the image (see 3d).
        {
            const GLuint targets[3] = { m_backTemp, m_front[dstSlot], m_depthRange[dstSlot] };
            attachTargets(targets, 3);
            glEnable(GL_BLEND);
            glBlendEquation(GL_MAX);
            glUseProgram(prog);
            if (stageLoc != -1) glUniform1i(stageLoc, kPeeling);
            if (opaqueLoc != -1) glUniform1i(opaqueLoc, 0);
            if (depthSrcLoc != -1) glUniform1i(depthSrcLoc, 1);
            if (frontSrcLoc != -1) glUniform1i(frontSrcLoc, 2);
            glBindTextureUnit(0, m_opaqueDepth);
            glBindTextureUnit(1, m_depthRange[srcSlot]);
            glBindTextureUnit(2, m_front[srcSlot]);
            glBeginQuery(GL_SAMPLES_PASSED, m_queryId);
            for (const auto& m : transparentMeshes) drawTranslucent(m.first, m.second);
            glEndQuery(GL_SAMPLES_PASSED);
        }

        // 3c. blend the back peel into the back accumulator.
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        {
            const GLuint target[1] = { m_back };
            attachTargets(target, 1);
            glUseProgram(m_blitProgram);
            if (m_blitSrcLoc != -1) glUniform1i(m_blitSrcLoc, 0);
            if (m_blitModeLoc != -1) glUniform1i(m_blitModeLoc, 1);   // ACCUM_BACK
            glBindTextureUnit(0, m_backTemp);
            drawFullscreen();
        }

        std::swap(srcSlot, dstSlot);

        // 3d. automatic termination.
        //     Read the PREVIOUS pass's result: by now a whole pass has been
        //     queued behind it, so this normally does not stall the pipeline.
        //     Once a pass contributes fewer than ratio*viewport pixels, the
        //     remaining layers are sub-pixel -- resolving them further cannot
        //     change a visible pixel, so stop and let the ALPHA mop-up blend
        //     the tail. This is why the user never has to pick a pass count.
        if (m_queryId && pass > 0) {
            GLuint written = 0;
            glGetQueryObjectuiv(m_queryId, GL_QUERY_RESULT, &written);
            if (written <= occlusionThreshold) {
                peelingDone = true;
            }
        }
        if (peelingDone) break;
    }

    // ---- 4. ALPHA mop-up ------------------------------------------------
    // Anything still inside the outstanding depth range was never resolved by
    // the peel loop (we ran out of passes). Give it one plain alpha-blended
    // pass into `back` rather than dropping it -- without this, surfaces
    // deeper than maxPeelIterations simply vanish, which is exactly the
    // "transparency isn't working" failure mode.
    {
        glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
        glViewport(0, 0, vpW, vpH);
        const GLuint target[1] = { m_back };
        attachTargets(target, 1);
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(prog);
        if (stageLoc != -1) glUniform1i(stageLoc, kAlpha);
        if (depthSrcLoc != -1) glUniform1i(depthSrcLoc, 1);
        if (frontSrcLoc != -1) glUniform1i(frontSrcLoc, 2);
        glBindTextureUnit(0, m_opaqueDepth);
        glBindTextureUnit(1, m_depthRange[srcSlot]);
        glBindTextureUnit(2, m_front[srcSlot]);
        for (const auto& m : transparentMeshes) drawTranslucent(m.first, m.second);
    }

    glBindVertexArray(0);
    glUseProgram(0);

    // ---- 5. Resolve onto the caller's FBO --------------------------------
    // Under-blend `back` beneath the front accumulator to form the translucent
    // layer, then blend that OVER the opaque scene. Both halves are required:
    // the front accumulator holds the nearest resolved peels and the back
    // buffer the farthest ones, so resolving only the back buffer would drop
    // the front half and read far too opaque.
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    glViewport(0, 0, vpW, vpH);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(m_blitProgram);
    if (m_blitSrcLoc != -1) glUniform1i(m_blitSrcLoc, 0);
    if (m_blitBackLoc != -1) glUniform1i(m_blitBackLoc, 1);
    if (m_blitModeLoc != -1) glUniform1i(m_blitModeLoc, 2);   // FINAL
    glBindTextureUnit(0, m_front[srcSlot]);   // front accumulator
    glBindTextureUnit(1, m_back);             // back accumulator
    drawFullscreen();
    glBindVertexArray(0);
    glUseProgram(0);
    // GLStateGuard restores blend/depth/cull/sRGB/clear colour on exit.
}