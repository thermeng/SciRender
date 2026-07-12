#include "custom_viewport_item.h"
#include <QQuickWindow>
#include <QMouseEvent>
#include <QWheelEvent>
#include <glad/glad.h>

CustomViewportItem::CustomViewportItem(QQuickItem* parent)
    : QQuickItem(parent) {
    setAcceptedMouseButtons(Qt::AllButtons);
    setFlag(ItemHasContents, true);

    // Establish the window link as soon as this item joins the QML Scene Graph
    connect(this, &QQuickItem::windowChanged, this, [this](QQuickWindow *window) {
        if (window) {
            // Step A: Safely handle initializations on the rendering thread
            connect(window, &QQuickWindow::beforeSynchronizing, this, [this]() {
                if (m_renderer) {
                    // Lazy-init GLAD once the window's OpenGL context becomes active
                    static bool gladInitialized = false;
                    if (!gladInitialized) {
                        m_renderer->initGLAD();
                        m_renderer->initShaders();
                        m_renderer->initGrid();
                        gladInitialized = true;
                    }
                }
            }, Qt::DirectConnection);

            // Step B: Direct underlay rendering loop injection
            connect(window, &QQuickWindow::beforeRendering, this, [this]() {
                if (!m_renderer) return;

                // Sync the rendering draw viewport sizing coordinates
                glViewport(0, 0, static_cast<GLsizei>(this->width()), static_cast<GLsizei>(this->height()));

                // Fire your core OpenGL draw arrays/elements stack
                m_renderer->renderFrame();
            }, Qt::DirectConnection);

            // FIX FOR QT 6: Prevent Qt from overwriting the background canvas
            // by forcing the window background to transparent.
            window->setColor(Qt::transparent);
        }
    });
}

void CustomViewportItem::setRenderer(Renderer* r) {
    if (m_renderer == r) return;
    m_renderer = r;
    emit rendererChanged();

    // Update structural sizing values if the object is swapped at runtime
    if (m_renderer) {
        m_renderer->resizeViewport(static_cast<int>(width()), static_cast<int>(height()));
    }
    if (window()) window()->update();
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