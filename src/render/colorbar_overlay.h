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
#include <vector>

struct ColorbarData {
    QString title;          // label on top (e.g. "Scalar", "Vector", "Streamline")
    QVariantList stops;     // [t, r, g, b] x N (t in 0..1)
    QStringList tickLabels; // left-to-right, below the bar
    bool visible = false;
};

class ColorbarOverlay {
public:
    ColorbarOverlay() = default;
    ~ColorbarOverlay() { shutdown(); }

    bool init();
    void shutdown();
    bool isInitialized() const { return program_ != 0; }

    // Draws all visible bars into the currently-bound FBO, covering the full
    // deviceW x deviceH viewport. Bars are stacked bottom-right.
    void drawBars(float dpr, int deviceW, int deviceH,
                  const std::vector<ColorbarData>& bars);

    void markDirty() { imageCacheValid_ = false; textureCacheValid_ = false; }

private:
    void uploadAndDraw(const QImage& img, int deviceW, int deviceH);

    GLuint program_ = 0;
    GLuint vao_ = 0, vbo_ = 0;
    GLuint tex_ = 0;
    GLint samplerLoc_ = -1;
    int texW_ = 0, texH_ = 0;

    bool buildProgram();
    QImage buildImage(float dpr, int deviceW, int deviceH,
                      const std::vector<ColorbarData>& bars) const;

    // Image cache: avoids rebuilding the QImage every frame when the
    // colormap choices, tick labels, or viewport dimensions haven't changed.
    QImage cachedImage_;
    bool imageCacheValid_ = false;
    bool textureCacheValid_ = false;
    std::vector<ColorbarData> cachedBars_;
    float cachedDpr_ = 0.0f;
    int cachedW_ = 0, cachedH_ = 0;
};
