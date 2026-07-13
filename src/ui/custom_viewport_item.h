#pragma once

#include <QtQuick/QQuickFramebufferObject>
#include <QOpenGLFramebufferObject>
#include <QPoint>
#include <QString>
#include "renderer/renderer.h"

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
    QString m_pendingScreenshot; // ponytail: carried from GUI thread (synchronize) to render() where GL context is current
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
    ::Renderer* m_scene = nullptr;
    QPoint m_lastMousePos;
    bool m_isRightClick = false;
    friend class ViewportFboRenderer;
};
