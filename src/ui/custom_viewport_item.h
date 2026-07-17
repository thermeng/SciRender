#pragma once

#include <QtQuick/QQuickFramebufferObject>
#include <QOpenGLFramebufferObject>
#include <QPoint>
#include <QString>
#include "render/renderer.h"

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
    // CPU mesh handoff lives in synchronize() (GUI thread, no GL
    // context), but the GL upload must run in render() (context current).
    RenderMesh m_pendingMesh;
    bool m_uploadPending = false;
    // Scalar-only re-upload: the field switched on the GUI thread, so the
    // render thread re-buffers just the scalar attribute (sbo) — not the mesh.
    std::vector<float> m_pendingScalars;
    bool m_uploadScalarsPending = false;
    bool m_dirty = true; // render only when something changed (idle = no GPU work)
    QString m_pendingScreenshot; // carried from GUI thread (synchronize) to render() where GL context is current
};

class ViewportVisualizer : public QQuickFramebufferObject {
    Q_OBJECT
    Q_PROPERTY(::Renderer* renderer READ renderer WRITE setRenderer NOTIFY rendererChanged)

public:
    explicit ViewportVisualizer(QQuickItem* parent = nullptr);

    ::Renderer* renderer() const;
    void setRenderer(::Renderer* r);

    QQuickFramebufferObject::Renderer* createRenderer() const override;

signals:
    void rendererChanged();

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    // Screenshot path forwarded from the GUI thread; consumed in synchronize().
    QString m_pendingScreenshot;
    bool m_needsRender = true; // set by signal lambdas; copied into the renderer's dirty flag
    ::Renderer* m_scene = nullptr;
    QPoint m_lastMousePos;
    bool m_isRightClick = false;
    friend class ViewportFboRenderer;
};
