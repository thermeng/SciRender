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

        // CRITICAL FIX: We draw the GL scene in beforeRendering. In Qt 6 the
        // scene graph always clears the framebuffer, so to keep our underlay
        // visible we make the window transparent: the clear then uses an
        // alpha=0 color and our GL content (which clears to bgColor itself in
        // renderFrame()) shows through. The sidebar/overlay QML draws on top.
        window->setColor(Qt::transparent);

        // Guard against duplicate connections if windowChanged fires more than once.
        if (m_renderConnectionsDone) return;
        m_renderConnectionsDone = true;

        // Step A: Keep beforeSynchronizing purely for data state adjustments if needed
        connect(window, &QQuickWindow::beforeSynchronizing, this, [this]() {
            // Properties synchronization only (keep lightweight)
        }, Qt::DirectConnection);

        // Step B: Direct underlay rendering loop injection
        connect(window, &QQuickWindow::beforeRendering, this, [this, window]() {
            if (!m_renderer) return;

            static bool openGLAssetsInitialized = false;
            if (!openGLAssetsInitialized) {
                m_renderer->initGLAD();
                m_renderer->initShaders();
                m_renderer->initGrid();
                m_renderer->initGizmo();
                openGLAssetsInitialized = true;
            }

            // SAFE RENDERING THREAD HANDOFF ROUTINE
            if (m_renderer->consumeMeshChanged()) {
                // 1. Fetch the queued raw geometry under a thread-safe lock.
                //    Copy quickly to minimize lock contention time.
                RenderMesh meshToUpload;
                m_renderer->takeQueuedMesh(meshToUpload);

                // 2. Safe to call glDelete/glGen/glBufferData routines here because
                //    we are on the render thread. uploadMesh() cleans up old GPU
                //    handles internally, so we must NOT call clearMeshes() here
                //    (that would reset hasMeshLoaded and re-show the drop overlay).
                if (!meshToUpload.vertices.empty()) {
                    m_renderer->uploadMesh(meshToUpload);
                }
            }

            // Forward device-pixel dimensions so the GL viewport matches the real
            // framebuffer on HiDPI displays.
            if (window) {
                m_renderer->setDevicePixelRatio(static_cast<float>(window->devicePixelRatio()));
            }
            m_renderer->resizeViewport(static_cast<int>(this->width()), static_cast<int>(this->height()));
            m_renderer->renderFrame();

        }, Qt::DirectConnection);

        // Establish the screenshot capture connection now that we have a window.
        tryConnectScreenshotCapture();
    });
}

void CustomViewportItem::tryConnectScreenshotCapture() {
    // Connect only once, and only when both the window and the renderer exist.
    if (m_screenshotConnected || !window() || !m_renderer) return;
    m_screenshotConnected = true;

    // The Renderer emits screenshotRequested from the GUI thread (where no GL
    // context is current). We perform the actual GL read + save here, connected
    // to afterRendering with DirectConnection so it runs on the render thread
    // while the OpenGL context is bound.
    connect(m_renderer, &Renderer::screenshotRequested, window(), [this](const QString& path) {
        if (m_renderer) m_renderer->captureScreenshotToFile(path);
    }, Qt::DirectConnection);
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
        // The window may already exist; wire up render-thread screenshot capture.
        tryConnectScreenshotCapture();
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