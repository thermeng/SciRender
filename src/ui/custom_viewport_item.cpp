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
    // (grid shows immediately even before any mesh is loaded).
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
        // ponytail: wireframe/surface setters emit these but nothing repainted them before
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
        // Grab-style panning: pass raw deltas. pan() already negates X so the
        // scene follows the cursor; vertical uses +up so dragging down moves the
        // camera up (scene follows the cursor down) — consistent with horizontal.
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

    // Drain the queued mesh handoff. Only copy CPU data here (GUI thread,
    // no GL context). The actual GL upload happens in render() where the
    // context is current — calling uploadMesh (glGenBuffers etc.) here is UB.
    if (m_scene && m_scene->consumeMeshChanged()) {
        m_scene->takeQueuedMesh(m_pendingMesh);
        m_uploadPending = !m_pendingMesh.vertices.empty();
    }

    // Carry the screenshot request across to render() (GL context current there).
    // Clear the visualizer copy so it is NOT re-copied on the next frame.
    m_pendingScreenshot = vv->m_pendingScreenshot;
    vv->m_pendingScreenshot.clear();
}

QOpenGLFramebufferObject* ViewportFboRenderer::createFramebufferObject(const QSize& size) {
    m_fboSize = size;
    QOpenGLFramebufferObjectFormat format;
    format.setAttachment(QOpenGLFramebufferObject::Depth);
    format.setSamples(0); // raw GL draw handles MSAA itself if needed
    m_fbo = new QOpenGLFramebufferObject(size, format);
    return m_fbo;
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
    // Upload any pending mesh NOW — this is the render thread with the GL
    // context current. (synchronize() only copied the CPU data.)
    if (m_uploadPending) {
        m_scene->uploadMesh(m_pendingMesh);
        m_uploadPending = false;
    }

    m_scene->renderFrame();

    // ponytail: capture here, NOT in synchronize() — synchronize runs on the GUI
    // thread with no GL context current; render() owns the context. FBO now holds
    // the freshly drawn frame, so the read is correct. MUST happen BEFORE
    // resetOpenGLState(): that call can unbind the FBO, after which glReadPixels
    // would read the wrong/empty buffer and produce a blank capture.
    if (!m_pendingScreenshot.isEmpty()) {
        m_scene->captureScreenshotToFile(m_pendingScreenshot, m_fbo);
        m_pendingScreenshot.clear();
    }

    // Qt 6: restore default GL state so the scene graph is not surprised.
    QQuickOpenGLUtils::resetOpenGLState();
}
