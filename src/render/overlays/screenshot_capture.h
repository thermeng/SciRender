#pragma once

#include <QString>
#include <QImage>
#include <glad/gl.h>
#include "render/foundation/gl_raii.h"

class Renderer;

class ScreenshotCapture {
public:
    struct Options {
        bool transparent = false;
    };

    struct Result {
        bool success = false;
        QString savedPath;
    };

    // --- Persistent display FBO (viewport-sized, used every frame) ---
    void ensureDisplayFbo(int w, int h, int samples);
    GLuint displayFboId() const { return m_displayFbo; }
    int displayFboWidth() const { return m_displayW; }
    int displayFboHeight() const { return m_displayH; }
    int displayFboSamples() const { return m_displaySamples; }

    // --- Persistent screenshot FBO (higher-res, used on demand) ---
    void ensureScreenshotFbo(int w, int h, int samples);
    GLuint screenshotFboId() const { return m_screenshotFbo; }

    // --- Read from a given FBO and save to file ---
    // Handles MSAA resolve, PBO async readback, vertical flip, and file save.
    Result readFboAndSave(GLuint fboId, int w, int h, int samples,
                          bool transparent, const QString& path);

    // --- Destroy all GL resources (call on context teardown) ---
    void destroyAll();

private:
    // Persistent display FBO resources
    GlFramebuffer m_displayFbo;
    GlRenderbuffer m_displayColor;
    GlRenderbuffer m_displayDepth;
    int m_displayW = 0;
    int m_displayH = 0;
    int m_displaySamples = 0;

    // Persistent screenshot FBO resources
    GlFramebuffer m_screenshotFbo;
    GlRenderbuffer m_screenshotColor;
    GlRenderbuffer m_screenshotDepth;
    int m_screenshotW = 0;
    int m_screenshotH = 0;
    int m_screenshotSamples = 0;

    // Resolve FBO for MSAA readback (non-multisampled color-only)
    GlFramebuffer m_resolveFbo;
    GlRenderbuffer m_resolveColor;
    int m_resolveW = 0;
    int m_resolveH = 0;

    // PBO double-buffer for async readback
    GLuint m_pboIds[2] = {};
    int m_pboIdx = 0;
    size_t m_pboCapacity = 0;
    bool m_pboReady = false;

    // GL state save/restore
    struct GlState {
        GLint readFboBinding = 0;
        GLint drawFboBinding = 0;
        GLint fboBinding = 0;
        GLint activeTexture = 0;
        GLint readBuffer = GL_NONE;
        GLint packAlignment = 4;
        GLint pboBinding = 0;
        GLint textureBinding2D = 0;
        GLint viewport[4] = {0, 0, 0, 0};
        GLint scissorBox[4] = {0, 0, 0, 0};
        GLboolean depthTest = 0;
        GLboolean blend = 0;
        GLboolean cullFace = 0;
        GLboolean scissorTest = 0;
        GLint depthFunc = GL_LESS;
        GLint depthMask = 1;
        GLint blendSrc = GL_ONE;
        GLint blendDst = GL_ZERO;
        GLint blendEquationRgb = GL_FUNC_ADD;
        GLint blendEquationAlpha = GL_FUNC_ADD;
        GLint polygonMode[2] = {GL_FILL, GL_FILL};
        GLint currentProgram = 0;
        GLint vertexArrayBinding = 0;
        GLint elementArrayBuffer = 0;
        GLboolean stencilTest = 0;
        GLboolean framebufferSrgb = 0;
        GLint clipControlLower = GL_LOWER_LEFT;
        GLint clipControlDepth = GL_NEGATIVE_ONE_TO_ONE;
    };

    GlState saveState();
    void restoreState(const GlState& s);

    void ensureResolveFbo(int w, int h);
    void ensurePbos(size_t bytes);
    void destroyPbos();
};


