#pragma once
// GPU-composited colorbar overlay. Builds a QImage of the colorbar (gradient
// bar + tick labels + title) on the CPU via QPainter, uploads it as a texture,
// and draws it as a single full-viewport textured quad so it lands inside the
// viewport FBO and is therefore captured in screenshots (including transparent
// PNG exports), instead of living only as a QML overlay outside the GL surface.

#include <glm/glm.hpp>
#include <glad/gl.h>
#include <QString>
#include <QVariantList>
#include <QImage>

struct ColorbarData {
    QString title;          // header text (e.g. active scalar name)
    QVariantList stops;     // [t, r, g, b] x N  (t in 0..1 top->bottom)
    QStringList tickLabels; // bottom->top, count = stops coverage
    bool visible = false;
};

class ColorbarOverlay {
public:
    ColorbarOverlay() = default;
    ~ColorbarOverlay() { shutdown(); }

    bool init();
    void shutdown();
    bool isInitialized() const { return program_ != 0; }

    // Draws the colorbar into the currently-bound FBO, covering the full
    // deviceW x deviceH viewport. corner: 0 = bottom-right (scalar),
    // 1 = top-right (vector). dpr scales the logical layout from Main.qml.
    void draw(float dpr, int deviceW, int deviceH,
              const ColorbarData& data, int corner);

    void markDirty() { imageCacheValid_ = false; textureCacheValid_ = false; }

private:
    void uploadAndDraw(const QImage& img, int deviceW, int deviceH);

    GLuint program_ = 0;
    GLuint vao_ = 0, vbo_ = 0;
    GLuint tex_ = 0;
    GLint samplerLoc_ = -1;

    bool buildProgram();
    QImage buildImage(float dpr, int deviceW, int deviceH,
                      const ColorbarData& data, int corner) const;

    // Image cache: avoids rebuilding the QImage every frame when the
    // colormap choice, reversed flag, tick count, scalar range, title,
    // or viewport dimensions haven't changed.
    QImage cachedImage_;
    bool imageCacheValid_ = false;
    bool textureCacheValid_ = false;
    ColorbarData cachedData_;
    float cachedDpr_ = 0.0f;
    int cachedW_ = 0, cachedH_ = 0;
    int cachedCorner_ = -1;
};
