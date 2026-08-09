#pragma once

#include <QString>
#include <QImage>
#include <glad/gl.h>
#include "render/gl_raii.h"

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

    Result renderAndCapture(::Renderer* renderer, int w, int h, int samples,
                            const Options& opts, const QString& path);

private:
    struct GlState {
        GLint readFboBinding = 0;
        GLint drawFboBinding = 0;
        GLint fboBinding = 0;
        GLint activeTexture = 0;
        GLint readBuffer = GL_NONE;
        GLint packAlignment = 4;
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
        GLint polygonMode[2] = {GL_FILL, GL_FILL};
    };

    GlState saveState();
    void restoreState(const GlState& s);
};
