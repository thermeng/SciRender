#pragma once

#include <QString>
#include <QImage>
#include <glad/gl.h>
#include "render/gl_raii.h"

class ScreenshotCapture {
public:
    struct Options {
        bool transparent = false;
    };

    struct Result {
        bool success = false;
        QString savedPath;
    };

    Result capture(GLuint fboId, int w, int h, int samples, const Options& opts, const QString& path);

private:
    struct GlState {
        GLint readFboBinding = 0;
        GLint drawFboBinding = 0;
        GLint fboBinding = 0;
        GLint activeTexture = 0;
        GLint readBuffer = GL_NONE;
        GLint packAlignment = 4;
        GLint textureBinding2D = 0;
    };

    GlState saveState();
    void restoreState(const GlState& s);

    GLuint resolveMsaa(GLuint srcFbo, int w, int h, GlTexture& resolveTex, GlFramebuffer& resolveFbo);

    Result saveImage(const QImage& img, const QString& path);
};
