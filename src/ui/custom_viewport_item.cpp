#include "custom_viewport_item.h"
#include <QQuickWindow>
#include <QMouseEvent>
#include <QWheelEvent>
#include <glad/glad.h>
#include <QDebug>

CustomViewportItem::CustomViewportItem(QQuickItem* parent)
    : QQuickItem(parent) {
    setAcceptedMouseButtons(Qt::AllButtons);
    setFlag(ItemHasContents, true);

    connect(this, &QQuickItem::windowChanged, this, [this](QQuickWindow *window) {
        if (!window) return;

        // Step A: Keep beforeSynchronizing purely for data state adjustments if needed
        connect(window, &QQuickWindow::beforeSynchronizing, this, [this]() {
            // Properties synchronization only (keep lightweight)
        }, Qt::DirectConnection);

        // Step B: Direct underlay rendering loop injection
        connect(window, &QQuickWindow::beforeRendering, this, [this]() {
            if (!m_renderer) return;

            static bool openGLAssetsInitialized = false;
            if (!openGLAssetsInitialized) {
                m_renderer->initGLAD();
                m_renderer->initShaders();
                m_renderer->initGrid();
                openGLAssetsInitialized = true;
            }

            // SAFE RENDERING THREAD HANDOFF ROUTINE
            if (m_renderer->consumeMeshChanged()) {
                // 1. Safe to call glDelete routines here because we are on the render thread
                m_renderer->clearMeshes();

                // 2. Fetch the queued raw geometry under a thread-safe lock
                // We copy it quickly to minimize lock contention time
                RenderMesh meshToUpload;
                // Accessing the private mutex/queue via a quick public helper
                // or direct class scope injection if friend classes are configured.
                // For simplicity, make sure your renderer exposes a thread-safe way to get the data:
                // (Assuming you handle direct extraction here via a public function or direct pointer)

                // 3. Safe to call glGen/glBufferData routines here!
                // m_renderer->uploadMesh(meshToUpload);
            }

            m_renderer->resizeViewport(static_cast<int>(this->width()), static_cast<int>(this->height()));
            m_renderer->renderFrame();

        }, Qt::DirectConnection);
    });
}

void CustomViewportItem::setRenderer(Renderer* r) {
    if (m_renderer == r) return;

    if (m_renderer) {
        m_renderer->disconnect(this);
    }

    m_renderer = r;
    emit rendererChanged();

    if (m_renderer) {
        // When a new mesh finishes loading, force the Qt Quick Window to redraw
        connect(m_renderer, &Renderer::meshDataUpdated, this, [this]() {
            if (window()) {
                window()->update(); // Force a modern OpenGL frame paint pass
            }
        });
    }
}

void CustomViewportItem::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (m_renderer) {
        // Forward physical widget geometry shifts down into your camera aspect matrix structures
        m_renderer->resizeViewport(static_cast<int>(newGeometry.width()),
                                   static_cast<int>(newGeometry.height()));
    }
    if (window()) window()->update();
}

void CustomViewportItem::mousePressEvent(QMouseEvent* event) {
    m_lastMousePos = event->pos();
    m_isRightClick = (event->button() == Qt::RightButton);
    event->accept();
}

void CustomViewportItem::mouseMoveEvent(QMouseEvent* event) {
    if (!m_renderer) return;

    QPoint delta = event->pos() - m_lastMousePos;
    m_lastMousePos = event->pos();

    if (m_isRightClick) {
        m_renderer->pan(delta.x() * 0.005, delta.y() * 0.005);
    } else {
        m_renderer->azimuth(-delta.x() * 0.5);
        m_renderer->elevation(delta.y() * 0.5);
    }

    if (window()) window()->update(); // Force continuous UI repaint scheduling
    event->accept();
}

void CustomViewportItem::mouseReleaseEvent(QMouseEvent* event) {
    event->accept();
}

void CustomViewportItem::wheelEvent(QWheelEvent* event) {
    if (!m_renderer) return;

    double factor = (event->angleDelta().y() > 0) ? 0.9 : 1.1;
    m_renderer->dolly(factor);

    if (window()) window()->update();
    event->accept();
}