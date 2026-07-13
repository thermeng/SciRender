#include <glad/glad.h>
#include "custom_viewport_item.h"
#include <QQuickWindow>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QOpenGLContext>
#include <QDebug>
#include <QQuickOpenGLUtils>
#include <QImage>
#include <QStandardPaths>
#include <vector>

// ---------------------------------------------------------------------------
// ViewportVisualizer (QQuickFramebufferObject)
//
// The previous approach drew raw OpenGL into the default framebuffer inside
// QQuickWindow::beforeRendering. That fails on Qt 6 because the scene graph
// starts its own render pass with a clear after beforeRendering, so anything
// drawn there is immediately erased. The only reliable way to composite raw
// OpenGL under a QML UI is to render into an OFFSCREEN framebuffer object and
// let the scene graph texturize it as a normal item. This file implements that.
// ---------------------------------------------------------------------------

ViewportVisualizer::ViewportVisualizer(QQuickItem* parent)
    : QQuickFramebufferObject(parent) {
    setAcceptedMouseButtons(Qt::AllButtons);
    // Keep the FBO continuous (camera interaction, animation).
    setFlag(ItemHasContents, true);
}

QQuickFramebufferObject::Renderer* ViewportVisualizer::createRenderer() const {
    return new ViewportFboRenderer();
}

void ViewportVisualizer::setRenderer(::Renderer* r) {
    if (m_scene == r) return;
    m_scene = r;
    emit rendererChanged();

    // The scene is now bound: make sure at least one FBO render is scheduled
    // (gizmo/grid show immediately even before any mesh is loaded).
    update();

    // When a new mesh finishes loading, ask the scene graph for a repaint so
    // the FBO re-renders with the new geometry.
    if (m_scene) {
        connect(m_scene, &::Renderer::meshDataUpdated, this, [this]() {
            update();
        });
        connect(m_scene, &::Renderer::screenshotRequested, this, [this](const QString& path) {
            // Run on the GUI thread (QML side). We forward to the renderer which
            // performs the GL read inside its FBO render pass via synchronize().
            if (m_scene) m_pendingScreenshot = path;
            update();
        });
        // ponytail: lighting changes never repainted before — connect once here (guarded by setRenderer's early return)
        connect(m_scene, &::Renderer::lightingParametersChanged, this, [this]() { update(); });
        connect(m_scene, &::Renderer::viewChanged, this, [this]() { update(); });
        // ponytail: colormap switch must repaint the GL mesh, not just the QML legend
        connect(m_scene, &::Renderer::colormapChanged, this, [this]() { update(); });
        // ponytail: wireframe/grid/surface setters emit these but nothing repainted them before
        connect(m_scene, &::Renderer::wireframeChanged, this, [this]() { update(); });
        connect(m_scene, &::Renderer::gridVisibilityChanged, this, [this]() { update(); });
        connect(m_scene, &::Renderer::surfaceVisibilityChanged, this, [this]() { update(); });
    }
}

::Renderer* ViewportVisualizer::renderer() const { return m_scene; }

void ViewportVisualizer::mousePressEvent(QMouseEvent* event) {
    m_lastMousePos = event->pos();
    m_isRightClick = (event->button() == Qt::RightButton);
    event->accept();
}

void ViewportVisualizer::mouseMoveEvent(QMouseEvent* event) {
    if (!m_scene) return;
    QPoint delta = event->pos() - m_lastMousePos;
    m_lastMousePos = event->pos();
    if (m_isRightClick) {
        m_scene->pan(delta.x(), delta.y());
    } else {
        m_scene->azimuth(-delta.x() * 0.5);
        m_scene->elevation(delta.y() * 0.5);
    }
    update();
    event->accept();
}

void ViewportVisualizer::mouseReleaseEvent(QMouseEvent* event) {
    event->accept();
}

void ViewportVisualizer::wheelEvent(QWheelEvent* event) {
    if (!m_scene) return;
    double factor = (event->angleDelta().y() > 0) ? 1.1 : 0.9; // ponytail: scroll up = zoom IN (smaller distance)
    m_scene->dolly(factor);
    update();
    event->accept();
}

// ---------------------------------------------------------------------------
// ViewportFboRenderer
// ---------------------------------------------------------------------------

ViewportFboRenderer::ViewportFboRenderer()
    : m_initialized(false) {
}

void ViewportFboRenderer::synchronize(QQuickFramebufferObject* item) {
    ViewportVisualizer* vv = qobject_cast<ViewportVisualizer*>(item);
    if (!vv) return;

    // Hand the shared Renderer pointer over to the render thread.
    m_scene = vv->renderer();

    // Forward the latest size in LOGICAL pixels; renderFrame() multiplies by
    // devicePixelRatio exactly once to match the device-sized FBO. (Passing
    // device px here double-applied the dpr and supersized glViewport.)
    if (m_scene) {
        const float dpr = static_cast<float>(item->window() ? item->window()->devicePixelRatio() : 1.0);
        m_scene->setDevicePixelRatio(dpr);
        m_scene->resizeViewport(
            static_cast<int>(item->width()),
            static_cast<int>(item->height()));
    }

    // Drain the queued mesh handoff (upload happens on the render thread, like
    // the old beforeRendering path did — safe because we own the GL context).
    if (m_scene && m_scene->consumeMeshChanged()) {
        RenderMesh meshToUpload;
        m_scene->takeQueuedMesh(meshToUpload);
        if (!meshToUpload.vertices.empty()) {
            m_scene->uploadMesh(meshToUpload);
        }
    }

    // Screenshot request forwarded from the QML/GUI side.
    if (!vv->m_pendingScreenshot.isEmpty() && m_scene) {
        m_scene->captureScreenshotToFile(vv->m_pendingScreenshot);
        vv->m_pendingScreenshot.clear();
    }
}

QOpenGLFramebufferObject* ViewportFboRenderer::createFramebufferObject(const QSize& size) {
    m_fboSize = size;
    QOpenGLFramebufferObjectFormat format;
    format.setAttachment(QOpenGLFramebufferObject::Depth);
    format.setSamples(0); // raw GL draw handles MSAA itself if needed
    return new QOpenGLFramebufferObject(size, format);
}

void ViewportFboRenderer::render() {
    if (!m_scene) return;

    if (!m_initialized) {
        // GLAD must resolve against THIS context (the FBO render context).
        m_scene->initGLAD();
        m_scene->initShaders();
        m_scene->initGrid();
        m_scene->initGizmo();
        m_initialized = true;
    }

    // The FBO is already bound as the GL draw target by the scene graph; our
    // renderFrame() clears/draws into it. The SG composites this FBO texture
    // on top of the background and BELOW the QML overlays (colorbar, etc.).
    m_scene->renderFrame();

    // Qt 6: restore default GL state so the scene graph is not surprised.
    QQuickOpenGLUtils::resetOpenGLState();
}
