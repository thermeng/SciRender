#pragma once

// Include glad before any Qt OpenGL headers to avoid GL/gl.h redefinition
#include <glad/gl.h>

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QPoint>
#include <QElapsedTimer>
#include <QTimer>
#include <memory>
#include "render/renderer.h"
#include "render/render_settings.h"

class ViewportWidget : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    explicit ViewportWidget(QWidget* parent = nullptr);
    ~ViewportWidget() override;

    void setSettings(::RenderSettings* s);
    ::RenderSettings* settings() const { return m_settings; }

    void requestScreenshot(const QString& path);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    void loadShaders();

    ::RenderSettings* m_settings = nullptr;
    bool m_initialized = false;
    QPoint m_lastMousePos;
    bool m_isRightClick = false;

    QString m_pendingScreenshot;
    bool m_dirty = true;

    QElapsedTimer m_fpsClock;
    double m_fpsLast = 0.0;
    double m_fpsSmoothed = 0.0;
    double m_fpsAccum = 0.0;
};
