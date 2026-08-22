#include "render/overlays/screenshot_capture.h"

#include <QImage>
#include <QFileInfo>
#include <QDebug>
#include <cstring>
#include <vector>

#include "render/foundation/renderer.h"

// ---------------------------------------------------------------------------
// Persistent FBO helpers
// ---------------------------------------------------------------------------
void ScreenshotCapture::ensureDisplayFbo(int w, int h, int samples) {
    if (w <= 0 || h <= 0) return;

    if (m_displayW == w && m_displayH == h && m_displaySamples == samples && m_displayFbo.has())
        return;

    m_displayFbo.reset();
    m_displayColor.reset();
    m_displayDepth.reset();

    m_displayW = w;
    m_displayH = h;
    m_displaySamples = samples;

    // Use 32F depth on both display and peel so Blit depth src/dst formats
    // match (spec requires same internalFormat, otherwise INVALID_OPERATION
    // and prevDepth samples 0 → layer 0 discards everything).
    GLenum depthFormat = GLAD_GL_ARB_depth_buffer_float ? GL_DEPTH32F_STENCIL8 : GL_DEPTH24_STENCIL8;

    glGenRenderbuffers(1, m_displayColor.ptr());
    glBindRenderbuffer(GL_RENDERBUFFER, m_displayColor);
    if (samples > 0)
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, w, h);
    else
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);

    glGenRenderbuffers(1, m_displayDepth.ptr());
    glBindRenderbuffer(GL_RENDERBUFFER, m_displayDepth);
    if (samples > 0)
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, depthFormat, w, h);
    else
        glRenderbufferStorage(GL_RENDERBUFFER, depthFormat, w, h);

    glGenFramebuffers(1, m_displayFbo.ptr());
    glBindFramebuffer(GL_FRAMEBUFFER, m_displayFbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_displayColor);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_displayDepth);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        qWarning() << "Display FBO incomplete: 0x" << Qt::hex << glCheckFramebufferStatus(GL_FRAMEBUFFER);
        m_displayFbo.reset();
        m_displayColor.reset();
        m_displayDepth.reset();
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void ScreenshotCapture::ensureScreenshotFbo(int w, int h, int samples) {
    if (w <= 0 || h <= 0) return;

    if (m_screenshotW == w && m_screenshotH == h && m_screenshotSamples == samples && m_screenshotFbo.has())
        return;

    m_screenshotFbo.reset();
    m_screenshotColor.reset();
    m_screenshotDepth.reset();

    m_screenshotW = w;
    m_screenshotH = h;
    m_screenshotSamples = samples;

    GLenum depthFormat = GLAD_GL_ARB_depth_buffer_float ? GL_DEPTH32F_STENCIL8 : GL_DEPTH24_STENCIL8;

    glGenRenderbuffers(1, m_screenshotColor.ptr());
    glBindRenderbuffer(GL_RENDERBUFFER, m_screenshotColor);
    if (samples > 0)
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, w, h);
    else
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);

    glGenRenderbuffers(1, m_screenshotDepth.ptr());
    glBindRenderbuffer(GL_RENDERBUFFER, m_screenshotDepth);
    if (samples > 0)
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, depthFormat, w, h);
    else
        glRenderbufferStorage(GL_RENDERBUFFER, depthFormat, w, h);

    glGenFramebuffers(1, m_screenshotFbo.ptr());
    glBindFramebuffer(GL_FRAMEBUFFER, m_screenshotFbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_screenshotColor);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_screenshotDepth);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        qWarning() << "Screenshot FBO incomplete: 0x" << Qt::hex << glCheckFramebufferStatus(GL_FRAMEBUFFER);
        m_screenshotFbo.reset();
        m_screenshotColor.reset();
        m_screenshotDepth.reset();
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void ScreenshotCapture::ensureResolveFbo(int w, int h) {
    if (w <= 0 || h <= 0) return;

    if (m_resolveW == w && m_resolveH == h && m_resolveFbo.has())
        return;

    m_resolveFbo.reset();
    m_resolveColor.reset();

    m_resolveW = w;
    m_resolveH = h;

    glGenRenderbuffers(1, m_resolveColor.ptr());
    glBindRenderbuffer(GL_RENDERBUFFER, m_resolveColor);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);

    glGenFramebuffers(1, m_resolveFbo.ptr());
    glBindFramebuffer(GL_FRAMEBUFFER, m_resolveFbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_resolveColor);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        qWarning() << "Resolve FBO incomplete: 0x" << Qt::hex << glCheckFramebufferStatus(GL_FRAMEBUFFER);
        m_resolveFbo.reset();
        m_resolveColor.reset();
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// ---------------------------------------------------------------------------
// PBO double-buffer for async readback
// ---------------------------------------------------------------------------
void ScreenshotCapture::ensurePbos(size_t bytes) {
    if (m_pboCapacity >= bytes && m_pboIds[0] && m_pboIds[1])
        return;

    destroyPbos();
    m_pboCapacity = bytes;
    m_pboReady = false;

    glGenBuffers(2, m_pboIds);
    for (int i = 0; i < 2; ++i) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pboIds[i]);
        glBufferData(GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(bytes), nullptr, GL_STREAM_READ);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}

void ScreenshotCapture::destroyPbos() {
    if (m_pboIds[0] || m_pboIds[1]) {
        glDeleteBuffers(2, m_pboIds);
        m_pboIds[0] = m_pboIds[1] = 0;
    }
    m_pboCapacity = 0;
    m_pboReady = false;
}

// ---------------------------------------------------------------------------
// GL state save/restore (complete)
// ---------------------------------------------------------------------------
ScreenshotCapture::GlState ScreenshotCapture::saveState() {
    GlState s;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &s.readFboBinding);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s.drawFboBinding);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s.fboBinding);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s.activeTexture);
    glGetIntegerv(GL_READ_BUFFER, &s.readBuffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &s.packAlignment);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &s.pboBinding);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.textureBinding2D);
    glGetIntegerv(GL_VIEWPORT, s.viewport);
    glGetIntegerv(GL_SCISSOR_BOX, s.scissorBox);
    s.depthTest = glIsEnabled(GL_DEPTH_TEST);
    s.blend = glIsEnabled(GL_BLEND);
    s.cullFace = glIsEnabled(GL_CULL_FACE);
    s.scissorTest = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_DEPTH_FUNC, &s.depthFunc);
    glGetIntegerv(GL_DEPTH_WRITEMASK, &s.depthMask);
    glGetIntegerv(GL_BLEND_SRC, &s.blendSrc);
    glGetIntegerv(GL_BLEND_DST, &s.blendDst);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &s.blendEquationRgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &s.blendEquationAlpha);
    glGetIntegerv(GL_POLYGON_MODE, s.polygonMode);
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.currentProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s.vertexArrayBinding);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &s.elementArrayBuffer);
    s.stencilTest = glIsEnabled(GL_STENCIL_TEST);
    s.framebufferSrgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
    glGetIntegerv(GL_DEPTH_FUNC, &s.depthFunc);
    glGetIntegerv(GL_CLIP_ORIGIN, &s.clipControlLower);
    glGetIntegerv(GL_CLIP_DEPTH_MODE, &s.clipControlDepth);
    return s;
}

void ScreenshotCapture::restoreState(const GlState& s) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s.readFboBinding);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s.drawFboBinding);
    glBindFramebuffer(GL_FRAMEBUFFER, s.fboBinding);
    glActiveTexture(s.activeTexture);
    glReadBuffer(s.readBuffer);
    glPixelStorei(GL_PACK_ALIGNMENT, s.packAlignment);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, s.pboBinding);
    glBindTexture(GL_TEXTURE_2D, s.textureBinding2D);
    glViewport(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);
    glScissor(s.scissorBox[0], s.scissorBox[1], s.scissorBox[2], s.scissorBox[3]);
    if (s.depthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (s.blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (s.cullFace) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (s.scissorTest) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    glDepthFunc(s.depthFunc);
    glDepthMask(static_cast<GLboolean>(s.depthMask));
    glBlendFunc(s.blendSrc, s.blendDst);
    glBlendEquationSeparate(s.blendEquationRgb, s.blendEquationAlpha);
    glPolygonMode(GL_FRONT, s.polygonMode[0]);
    glPolygonMode(GL_BACK, s.polygonMode[1]);
    glUseProgram(s.currentProgram);
    glBindVertexArray(s.vertexArrayBinding);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s.elementArrayBuffer);
    if (s.stencilTest) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    if (s.framebufferSrgb) glEnable(GL_FRAMEBUFFER_SRGB); else glDisable(GL_FRAMEBUFFER_SRGB);
    if (GLAD_GL_ARB_clip_control) glClipControl(s.clipControlLower, s.clipControlDepth);
}

// ---------------------------------------------------------------------------
// Main readback + save
// ---------------------------------------------------------------------------
ScreenshotCapture::Result ScreenshotCapture::readFboAndSave(
    GLuint fboId, int w, int h, int samples,
    bool transparent, const QString& path) {

    Result result;
    result.savedPath = path;

    if (fboId == 0 || path.isEmpty() || w <= 0 || h <= 0)
        return result;

    const bool isPng  = path.endsWith(".png", Qt::CaseInsensitive);
    const bool isJpeg = path.endsWith(".jpg", Qt::CaseInsensitive) || path.endsWith(".jpeg", Qt::CaseInsensitive);
    const bool isBmp  = path.endsWith(".bmp", Qt::CaseInsensitive);
    if (!isPng && !isJpeg && !isBmp) {
        qWarning() << "Screenshot unsupported format:" << path;
        return result;
    }

    const bool readTransparent = isPng && transparent;
    const int channels = readTransparent ? 4 : 3;
    const GLenum format = readTransparent ? GL_RGBA : GL_RGB;

    GlState state = saveState();

    // --- MSAA resolve: blit to non-MSAA FBO if source is multisampled ---
    GLuint readFboId = fboId;
    if (samples > 0) {
        ensureResolveFbo(w, h);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fboId);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_resolveFbo);
        glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        readFboId = m_resolveFbo;
    }

    glFinish();

    // --- Readback ---
    glBindFramebuffer(GL_READ_FRAMEBUFFER, readFboId);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    const size_t rawRow = static_cast<size_t>(w) * channels;
    const size_t rawSize = rawRow * static_cast<size_t>(h);

    std::vector<unsigned char> raw(rawSize, 0);
    glReadPixels(0, 0, w, h, format, GL_UNSIGNED_BYTE, raw.data());

    // --- Restore GL state ---
    restoreState(state);

    // --- Flip vertically (GL origin is bottom-left) ---
    const size_t stride = (rawRow + 3) & ~size_t(3);
    std::vector<unsigned char> flipped(stride * static_cast<size_t>(h), 0);
    for (int y = 0; y < h; ++y) {
        std::memcpy(flipped.data() + static_cast<size_t>(y) * stride,
                    raw.data() + static_cast<size_t>(h - 1 - y) * rawRow, rawRow);
    }

    QImage::Format qf = readTransparent ? QImage::Format_RGBA8888 : QImage::Format_RGB888;
    QImage img = QImage(flipped.data(), w, h, static_cast<int>(stride), qf).copy();

    const char* token = isPng ? "PNG" : (isBmp ? "BMP" : "JPEG");
    result.success = img.save(path, token, -1);
    if (!result.success) {
        qWarning() << "Screenshot save failed:" << path;
    }
    return result;
}

// ---------------------------------------------------------------------------
// Cleanup
// ---------------------------------------------------------------------------
void ScreenshotCapture::destroyAll() {
    m_displayFbo.reset();
    m_displayColor.reset();
    m_displayDepth.reset();
    m_displayW = m_displayH = m_displaySamples = 0;

    m_screenshotFbo.reset();
    m_screenshotColor.reset();
    m_screenshotDepth.reset();
    m_screenshotW = m_screenshotH = m_screenshotSamples = 0;

    m_resolveFbo.reset();
    m_resolveColor.reset();
    m_resolveW = m_resolveH = 0;

    destroyPbos();
}


