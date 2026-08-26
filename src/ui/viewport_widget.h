#pragma once

// Include glad before any Qt OpenGL headers to avoid GL/gl.h redefinition
#include <glad/gl.h>

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QPoint>
#include <QElapsedTimer>
#include <QLabel>
#include <QTimer>
#include <QPainter>
#include <QContextMenuEvent>
#include <QFileDialog>
#include <QMenu>
#include <QSettings>
#include <memory>
#include "render/foundation/renderer.h"
#include "render/settings/render_settings.h"
#include "render/overlays/screenshot_capture.h"
#include "render/overlays/colorbar_overlay.h"

class ViewportWidget : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    explicit ViewportWidget(int msaaSamples = 0, QWidget* parent = nullptr);
    ~ViewportWidget() override;

    void setSettings(::RenderSettings* s);
    ::RenderSettings* settings() const { return m_settings; }

    void requestScreenshot(const QString& path);

    // Offscreen render of the CURRENT published state at an arbitrary size;
    // used by AnimationExporter (GL context work stays on the GUI thread).
    QImage captureFrameImage(int width, int height, int samples, bool transparent);
    void forceRepaint() { m_dirty = true; update(); }

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    bool event(QEvent* event) override;

private:
    void loadShaders();
    void drawEmptyState(QPainter& painter);
    void drawSpinner(QPainter& painter);
    void advanceSpinner();
    void updateColorbarHitState();
    void updateGizmoHover(const QPoint& pos);
    void beginColorbarDrag(const QPoint& pos, int barIndex);
    void updateColorbarDrag(const QPoint& pos);
    void endColorbarDrag();

    ::RenderSettings* m_settings = nullptr;
    bool m_initialized = false;
    int m_msaaSamples = 0;
    QPoint m_lastMousePos;
    bool m_isMiddleClick = false;

    QString m_pendingScreenshot;
    ScreenshotCapture m_screenshotCapture;
    bool m_dirty = true;

    QElapsedTimer m_fpsClock;
    double m_fpsLast = 0.0;
    double m_fpsSmoothed = 0.0;
    double m_fpsAccum = 0.0;

    QLabel* m_fpsLabel = nullptr;

    QTimer* m_spinnerTimer = nullptr;
    int m_spinnerAngle = 0;

    // Colorbar drag state
    bool m_draggingColorbar = false;
    bool m_colorbarHover = false;
    int m_dragBarIndex = -1;
    QPoint m_dragStartPos;
    std::vector<ColorbarData> m_colorbarBars;

    // Axis-triad hover state (mirrors Renderer's atomic for change detection)
    int m_gizmoHoverAxis = -1;

private slots:
    void deferredCapture(const QString& path);
};


