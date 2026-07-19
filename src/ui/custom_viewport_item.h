#pragma once

#include <QtQuick/QQuickFramebufferObject>
#include <QOpenGLFramebufferObject>
#include <QPoint>
#include <QString>
#include <QElapsedTimer>
#include "render/renderer.h"
#include "render/render_settings.h"

class ViewportFboRenderer : public QQuickFramebufferObject::Renderer {
public:
    ViewportFboRenderer();
    void synchronize(QQuickFramebufferObject* item) override;
    QOpenGLFramebufferObject* createFramebufferObject(const QSize& size) override;
    void render() override;

private:
    ::Renderer* m_scene = nullptr;
    bool m_initialized = false;
    QSize m_fboSize;
    QOpenGLFramebufferObject* m_fbo = nullptr; // viewport FBO, used for screenshot capture
    // The mesh handoff now arrives as a shared_ptr stored on the Renderer
    // (setPendingMesh) and is uploaded inside renderFrame() under the GL context,
    // so no CPU mesh buffer is kept here.
    // Scalar-only re-upload: the field switched on the GUI thread, so the
    // render thread re-buffers just the scalar attribute (sbo) — not the mesh.
    // Stored as a shared_ptr (zero-copy); only the ref-count is bumped.
    std::shared_ptr<const std::vector<float>> m_pendingScalars;
    bool m_uploadScalarsPending = false;
    bool m_dirty = true; // render only when something changed (idle = no GPU work)
    QString m_pendingScreenshot; // carried from GUI thread (synchronize) to render() where GL context is current

    ::RenderSettings* m_settings = nullptr; // GUI-thread facade for FPS push-back
    int m_fboSamples = 0; // last FBO sample count; mismatch vs settings => recreate
    QElapsedTimer m_fpsClock;
    double m_fpsLast = 0.0;
    double m_fpsSmoothed = 0.0;
    double m_fpsAccum = 0.0;
};

class ViewportVisualizer : public QQuickFramebufferObject {
    Q_OBJECT
    Q_PROPERTY(::RenderSettings* settings READ settings WRITE setSettings NOTIFY settingsChanged)

public:
    explicit ViewportVisualizer(QQuickItem* parent = nullptr);

    ::RenderSettings* settings() const;
    void setSettings(::RenderSettings* s);

    QQuickFramebufferObject::Renderer* createRenderer() const override;

signals:
    void settingsChanged();

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    // Screenshot path forwarded from the GUI thread; consumed in synchronize().
    QString m_pendingScreenshot;
    bool m_needsRender = true; // set by signal lambdas; copied into the renderer's dirty flag
    ::RenderSettings* m_settings = nullptr;
    QPoint m_lastMousePos;
    bool m_isRightClick = false;
    friend class ViewportFboRenderer;
};
