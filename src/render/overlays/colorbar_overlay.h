#pragma once
// GPU-composited colorbar overlay. Each colorbar is drawn independently at its
// own draggable position so users can move any colorbar anywhere on the viewport.

#include "render/foundation/gl_raii.h"
#include <QString>
#include <QVariantList>
#include <QImage>
#include <QSettings>
#include <vector>

// Visual styling for one colorbar. Defaults reproduce the legacy look.
struct ColorbarStyle {
    enum Orientation { Horizontal = 0, Vertical = 1 };
    Orientation orientation = Horizontal;
    float fontScale = 1.0f;      // title/annotation font size
    float tickFontScale = 1.0f;  // tick label font size (independent of title)
    float lengthScale = 1.0f;    // bar size along its main axis
    float thicknessScale = 1.0f; // bar cross-axis thickness
    bool panelEnabled = false;   // background scrim behind the whole bar
    float panelOpacity = 0.55f;  // 0..1, used when panelEnabled
    bool showAnnotation = true;  // "[Scalar]"-type line above the field name
};

struct ColorbarData {
    QString title;
    QString subtitle;
    QString units; // optional unit suffix rendered as "(unit)" after the title
    QVariantList stops;
    QStringList tickLabels;
    bool visible = false;
    float fracX = 1.0f; // 0 = left, 1 = right
    float fracY = 1.0f; // 0 = top, 1 = bottom
    ColorbarStyle style;
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

    // Geometry for one bar, computed once and shared by painting, hit-testing
    // and drag rects. All values are device pixels; the layout is the single
    // source of truth so painted and hit-tested rects can never desync.
    struct TickItem {
        int index = 0;
        QPointF markFrom;
        QPointF markTo;
        QRect label; // empty when the label is thinned out
    };
    struct Layout {
        QSize size;
        QRect panel;
        QRect annotation; // empty when the annotation line is hidden
        QRect title;      // field name line
        QRect bar;
        std::vector<TickItem> ticks;
    };
    Layout computeLayout(float dpr, const ColorbarData& bar) const;
    static void selectTickLabels(std::vector<TickItem>& ticks,
                                 const QStringList& labels, int minGap, bool vertical);

    GlProgram program_;
    GlVao vao_;
    GlBuffer vbo_;
    GLint samplerLoc_ = -1;

    bool buildProgram();

    // Per-texture cache keyed by full bar content
    // (title+subtitle+units+stops+ticks+style+dpr); position is applied at quad
    // level so dragging never re-uploads textures.
    struct TextureCache {
        QImage image;
        GLuint texId = 0;
        int w = 0, h = 0;
        bool valid = false;
        QString key;
        float dpr = 0.f;
    };
    std::vector<TextureCache> cachedTextures_;
    static QString barKey(float dpr, const ColorbarData& bar);
};
