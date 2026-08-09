#include "render/screenshot_capture.h"

#include <QImage>
#include <QFileInfo>
#include <QDebug>
#include <cstring>
#include <vector>

#include "render/renderer.h"

ScreenshotCapture::GlState ScreenshotCapture::saveState() {
    GlState s;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &s.readFboBinding);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s.drawFboBinding);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s.fboBinding);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s.activeTexture);
    glGetIntegerv(GL_READ_BUFFER, &s.readBuffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &s.packAlignment);
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
    glGetIntegerv(GL_POLYGON_MODE, s.polygonMode);
    return s;
}

void ScreenshotCapture::restoreState(const GlState& s) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s.readFboBinding);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s.drawFboBinding);
    glBindFramebuffer(GL_FRAMEBUFFER, s.fboBinding);
    glActiveTexture(s.activeTexture);
    glReadBuffer(s.readBuffer);
    glPixelStorei(GL_PACK_ALIGNMENT, s.packAlignment);
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
    glPolygonMode(GL_FRONT, s.polygonMode[0]);
    glPolygonMode(GL_BACK, s.polygonMode[1]);
}

ScreenshotCapture::Result ScreenshotCapture::renderAndCapture(::Renderer* renderer, int w, int h, int samples,
                                                                const Options& opts, const QString& path) {
    Result result;
    result.savedPath = path;

    if (!renderer || path.isEmpty() || w <= 0 || h <= 0) {
        return result;
    }

    const bool isPng  = path.endsWith(".png", Qt::CaseInsensitive);
    const bool isJpeg = path.endsWith(".jpg", Qt::CaseInsensitive) || path.endsWith(".jpeg", Qt::CaseInsensitive);
    const bool isBmp  = path.endsWith(".bmp", Qt::CaseInsensitive);
    if (!isPng && !isJpeg && !isBmp) {
        qWarning() << "Screenshot unsupported format:" << path;
        return result;
    }

    const bool transparent = isPng && opts.transparent;

    GlState state = saveState();

    // --- Offscreen color + depth-stencil renderbuffers + FBO ---
    GlRenderbuffer colorRbo;
    GlRenderbuffer depthRbo;
    GlFramebuffer offscreenFbo;

    glGenRenderbuffers(1, colorRbo.ptr());
    glBindRenderbuffer(GL_RENDERBUFFER, colorRbo);
    if (samples > 0) {
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, w, h);
    } else {
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
    }

    glGenRenderbuffers(1, depthRbo.ptr());
    glBindRenderbuffer(GL_RENDERBUFFER, depthRbo);
    if (samples > 0) {
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, w, h);
    } else {
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    }

    glGenFramebuffers(1, offscreenFbo.ptr());
    glBindFramebuffer(GL_FRAMEBUFFER, offscreenFbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, colorRbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depthRbo);

    GLenum fbStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fbStatus != GL_FRAMEBUFFER_COMPLETE) {
        qWarning() << "Screenshot offscreen FBO incomplete: 0x" << Qt::hex << fbStatus;
        restoreState(state);
        return result;
    }

    // --- Resolve FBO for MSAA (color-only, non-multisampled) ---
    GlFramebuffer resolveFbo;
    GlRenderbuffer resolveColorRbo;
    GLuint readFboId = offscreenFbo;

    if (samples > 0) {
        glGenRenderbuffers(1, resolveColorRbo.ptr());
        glBindRenderbuffer(GL_RENDERBUFFER, resolveColorRbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);

        glGenFramebuffers(1, resolveFbo.ptr());
        glBindFramebuffer(GL_FRAMEBUFFER, resolveFbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, resolveColorRbo);

        GLenum resolveStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (resolveStatus != GL_FRAMEBUFFER_COMPLETE) {
            qWarning() << "Screenshot resolve FBO incomplete: 0x" << Qt::hex << resolveStatus;
            restoreState(state);
            return result;
        }
    }

    // --- Drain display pipeline before touching GL state ---
    glFinish();

    // --- Render scene into the offscreen FBO ---
    glBindFramebuffer(GL_FRAMEBUFFER, offscreenFbo);
    glViewport(0, 0, w, h);
    renderer->renderFrame();

    // --- Drain offscreen render before readback ---
    glFinish();

    // --- MSAA resolve via blit ---
    if (samples > 0) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, offscreenFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolveFbo);
        glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        readFboId = resolveFbo;
    }

    // --- Readback ---
    glBindFramebuffer(GL_FRAMEBUFFER, readFboId);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    const int channels = transparent ? 4 : 3;
    const size_t rawRow = static_cast<size_t>(w) * channels;
    std::vector<unsigned char> raw(rawRow * static_cast<size_t>(h));
    glReadPixels(0, 0, w, h, transparent ? GL_RGBA : GL_RGB, GL_UNSIGNED_BYTE, raw.data());

    // --- Restore Qt-expected GL state ---
    restoreState(state);

    // --- Flip vertically (GL origin is bottom-left) into a stride-aligned buffer ---
    const size_t stride = (rawRow + 3) & ~size_t(3);
    std::vector<unsigned char> flipped(stride * static_cast<size_t>(h), 0);
    for (int y = 0; y < h; ++y) {
        std::memcpy(flipped.data() + static_cast<size_t>(y) * stride,
                    raw.data() + static_cast<size_t>(h - 1 - y) * rawRow, rawRow);
    }

    QImage::Format qf = transparent ? QImage::Format_RGBA8888 : QImage::Format_RGB888;
    QImage img = QImage(flipped.data(), w, h, static_cast<int>(stride), qf).copy();

    const char* token = isPng ? "PNG" : (isBmp ? "BMP" : "JPEG");
    result.success = img.save(path, token, -1);
    if (!result.success) {
        qWarning() << "Screenshot save failed:" << path;
    }
    return result;
}
