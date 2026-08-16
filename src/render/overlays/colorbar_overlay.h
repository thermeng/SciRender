#pragma once
// GPU-composited colorbar overlay. Builds a QImage of the colorbar (gradient
// bar + tick labels + title) on the CPU via QPainter, uploads it as a texture,
// and draws it as a single full-viewport textured quad so it lands inside the
// viewport FBO and is therefore captured in screenshots (including transparent
// PNG exports), instead of living only as a QML overlay outside the GL surface.

#include <glm/glm.hpp>
#include "render/foundation/gl_raii.h"
#include <QString>
#include <QVariantList>
#include <QImage>
#include <vector>

struct ColorbarData {
    QString title;
    QString subtitle;
    QVariantList stops;
    QStringList tickLabels;
    bool visible = false;
};

class ColorbarOverlay {
public:
    ColorbarOverlay() = default;
    ~ColorbarOverlay() { shutdown(); }

    bool init();
    void shutdown();
    bool isInitialized() const { return program_.has(); }

    void drawBars(float dpr, int deviceW, int deviceH,
                  const std::vector<ColorbarData>& bars);

    void markDirty() { imageCacheValid_ = false; textureCacheValid_ = false; }

private:
    void uploadAndDraw(const QImage& img, int deviceW, int deviceH);

    GlProgram program_;
    GlVao vao_;
    GlBuffer vbo_;
    GlTexture tex_;
    GLint samplerLoc_ = -1;
    int texW_ = 0, texH_ = 0;

    bool buildProgram();
    QImage buildImage(float dpr, int deviceW, int deviceH,
                      const std::vector<ColorbarData>& bars) const;

    QImage cachedImage_;
    bool imageCacheValid_ = false;
    bool textureCacheValid_ = false;
    std::vector<ColorbarData> cachedBars_;
    float cachedDpr_ = 0.0f;
    int cachedW_ = 0, cachedH_ = 0;
};


