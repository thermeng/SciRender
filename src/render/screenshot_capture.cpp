#include "render/screenshot_capture.h"

#include <QImage>
#include <QFileInfo>
#include <QDebug>
#include <cstring>
#include <vector>

ScreenshotCapture::GlState ScreenshotCapture::saveState() {
    GlState s;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &s.readFboBinding);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s.drawFboBinding);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s.fboBinding);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s.activeTexture);
    glGetIntegerv(GL_READ_BUFFER, &s.readBuffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &s.packAlignment);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.textureBinding2D);
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
}

GLuint ScreenshotCapture::resolveMsaa(GLuint srcFbo, int w, int h, GlTexture& resolveTex, GlFramebuffer& resolveFbo) {
    glGenFramebuffers(1, resolveFbo.ptr());
    glGenTextures(1, resolveTex.ptr());
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, resolveTex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, resolveFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, resolveTex, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        qWarning() << "Screenshot resolve FBO incomplete: 0x" << Qt::hex << status;
        resolveFbo.reset();
        resolveTex.reset();
        return 0;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolveFbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    return resolveFbo.get();
}

ScreenshotCapture::Result ScreenshotCapture::saveImage(const QImage& img, const QString& path) {
    Result result;
    result.savedPath = path;

    const bool isPng  = path.endsWith(".png", Qt::CaseInsensitive);
    const bool isJpeg = path.endsWith(".jpg", Qt::CaseInsensitive) || path.endsWith(".jpeg", Qt::CaseInsensitive);
    const bool isBmp  = path.endsWith(".bmp", Qt::CaseInsensitive);

    if (!isPng && !isJpeg && !isBmp) {
        qWarning() << "Screenshot unsupported format:" << path;
        result.success = false;
        return result;
    }

    const char* token = isPng ? "PNG" : (isBmp ? "BMP" : "JPEG");
    result.success = img.save(path, token, -1);
    if (!result.success) {
        qWarning() << "Screenshot save failed:" << path;
    }
    return result;
}

ScreenshotCapture::Result ScreenshotCapture::capture(GLuint fboId, int w, int h, int samples,
                                                      const Options& opts, const QString& path) {
    Result result;
    result.savedPath = path;

    if (path.isEmpty() || fboId == 0 || w <= 0 || h <= 0) {
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
    const int channels = transparent ? 4 : 3;
    const GLenum fmt = transparent ? GL_RGBA : GL_RGB;
    const size_t stride = (static_cast<size_t>(w) * channels + 3) & ~size_t(3);

    GlState state = saveState();

    GLuint readFbo = fboId;
    GlFramebuffer resolveFbo;
    GlTexture resolveTex;

    if (samples > 0) {
        GLuint resolved = resolveMsaa(fboId, w, h, resolveTex, resolveFbo);
        if (resolved == 0) {
            restoreState(state);
            return result;
        }
        readFbo = resolved;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, readFbo);
    GLenum fbStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fbStatus != GL_FRAMEBUFFER_COMPLETE) {
        qWarning() << "Screenshot source FBO incomplete: 0x" << Qt::hex << fbStatus;
        restoreState(state);
        return result;
    }

    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glFinish();

    const size_t rawRow = static_cast<size_t>(w) * channels;
    std::vector<unsigned char> raw(rawRow * static_cast<size_t>(h));
    glReadPixels(0, 0, w, h, fmt, GL_UNSIGNED_BYTE, raw.data());

    restoreState(state);

    // Flip vertically (GL origin is bottom-left) into a stride-aligned buffer.
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
