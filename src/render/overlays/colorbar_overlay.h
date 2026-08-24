#pragma once
// GPU-composited colorbar overlay. Each colorbar is drawn independently at its
// own draggable position so users can move any colorbar anywhere on the viewport.

#include "render/foundation/gl_raii.h"
#include <QString>
#include <QVariantList>
#include <QImage>
#include <QSettings>
#include <vector>

struct ColorbarData {
    QString title;
    QString subtitle;
    QVariantList stops;
    QStringList tickLabels;
    bool visible = false;
    float fracX = 1.0f; // 0 = left, 1 = right
    float fracY = 1.0f; // 0 = top, 1 = bottom
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

    // Returns the index of the bar at pixel (px,py), or -1 if none
    int barIndexAt(float dpr, int deviceW, int deviceH,
                   const std::vector<ColorbarData>& bars, int px, int py) const;

    QRectF barRectAt(float dpr, int deviceW, int deviceH,
                     const ColorbarData& bar) const;

    void markDirty();

private:
    void drawSingleBar(float dpr, int deviceW, int deviceH,
                       const ColorbarData& bar);
    QImage buildSingleBarImage(float dpr, const ColorbarData& bar) const;
    QSize singleBarSize(float dpr) const;

    GlProgram program_;
    GlVao vao_;
    GlBuffer vbo_;
    GlTexture tex_;
    GLint samplerLoc_ = -1;

    bool buildProgram();

    // Per-texture cache keyed by full bar content (title+subtitle+stops+ticks+position+dpr)
    struct TextureCache {
        QImage image;
        GLuint texId = 0;
        int w = 0, h = 0;
        bool valid = false;
        QString key; // hash of bar content for robust invalidation
        float dpr = 0.f;
    };
    std::vector<TextureCache> cachedTextures_;
    float cachedDpr_ = 0.0f;
    static QString barKey(float dpr, const ColorbarData& bar);
};
