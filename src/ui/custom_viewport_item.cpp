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
#include <QElapsedTimer>
#include <QTimer>
#include <cmath>
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
    // Y-flip now lives here (scene-graph FBO node mirrors the texture)
    // instead of a depth-hazardous glm::scale in the render path.
    setMirrorVertically(true);
}

QQuickFramebufferObject::Renderer* ViewportVisualizer::createRenderer() const {
    return new ViewportFboRenderer();
}

void ViewportVisualizer::setSettings(::RenderSettings* s) {
    if (m_settings == s) return;
    m_settings = s;
    emit settingsChanged();

    // The scene is now bound: make sure at least one FBO render is scheduled
    // (grid shows immediately even before any mesh is loaded).
    update();

    // When a new mesh finishes loading, ask the scene graph for a repaint so
    // the FBO re-renders with the new geometry.
    if (m_settings) {
        connect(m_settings, &::RenderSettings::meshDataUpdated, this, [this]() {
            m_needsRender = true;
            update();
        });
        connect(m_settings, &::RenderSettings::screenshotRequested, this, [this](const QString& path) {
            // Forwarded from the GUI thread (QML side) to the render pass.
            m_pendingScreenshot = path;
            m_needsRender = true;
            update();
        });
        connect(m_settings, &::RenderSettings::lightingParametersChanged, this, [this]() { m_needsRender = true; update(); });
        connect(m_settings, &::RenderSettings::viewChanged, this, [this]() { m_needsRender = true; update(); });
        connect(m_settings, &::RenderSettings::colormapChanged, this, [this]() { m_needsRender = true; update(); });
        connect(m_settings, &::RenderSettings::vectorColormapChanged, this, [this]() { m_needsRender = true; update(); });
        connect(m_settings, &::RenderSettings::wireframeChanged, this, [this]() { m_needsRender = true; update(); });
        connect(m_settings, &::RenderSettings::gridVisibilityChanged, this, [this]() { m_needsRender = true; update(); });
        connect(m_settings, &::RenderSettings::surfaceVisibilityChanged, this, [this]() { m_needsRender = true; update(); });
    }
}

::RenderSettings* ViewportVisualizer::settings() const { return m_settings; }

void ViewportVisualizer::mousePressEvent(QMouseEvent* event) {
    m_lastMousePos = event->pos();
    m_isRightClick = (event->button() == Qt::RightButton);
    event->accept();
}

void ViewportVisualizer::mouseMoveEvent(QMouseEvent* event) {
    if (!m_settings) return;
    QPoint delta = event->pos() - m_lastMousePos;
    m_lastMousePos = event->pos();
    if (m_isRightClick) {
        m_settings->pan(delta.x(), delta.y());
    } else {
        m_settings->azimuth(-delta.x() * 0.5);
        m_settings->elevation(delta.y() * 0.5);
    }
    update();
    event->accept();
}

void ViewportVisualizer::mouseReleaseEvent(QMouseEvent* event) {
    event->accept();
    // Wake one frame after the LOD debounce window so cameraMoving clears and
    // the full-res mesh is redrawn — nothing else repaints once motion stops.
    QTimer::singleShot(160, this, [this]() { m_needsRender = true; update(); });
}

void ViewportVisualizer::wheelEvent(QWheelEvent* event) {
    if (!m_settings) return;
    double factor = (event->angleDelta().y() > 0) ? 1.1 : 0.9;
    m_settings->dolly(factor);
    update();
    QTimer::singleShot(160, this, [this]() { m_needsRender = true; update(); });
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

    // Resolve the backend Renderer from the GUI-thread RenderSettings facade.
    // This is the single point where the GUI thread's settings object touches
    // the render thread: we pull a deep-copied RenderRenderState snapshot and
    // hand the render thread the backend pointer. After this, render() reads
    // ONLY the snapshot — no GUI-thread data is touched during drawing.
    ::RenderSettings* settings = vv->settings();
    ::Renderer* scene = settings ? settings->backend() : nullptr;
    m_scene = scene;
    m_settings = settings;

    if (m_scene) {
        const float dpr = static_cast<float>(item->window() ? item->window()->devicePixelRatio() : 1.0);
        m_scene->setDevicePixelRatio(dpr);
        m_scene->resizeViewport(
            static_cast<int>(item->width()),
            static_cast<int>(item->height()));

        // Double-buffered handoff of the GUI view/visual state into the backend
        // (re-assembled only when dirty; idle frames skip the full copy).
        settings->publishRenderState(m_scene);
    }

    // Scalar-only field switch: re-buffer just the sbo on the render thread
    // (no GL context here). Consumed below in render() before draw.
    if (m_scene && m_scene->consumeScalarDirty() && m_scene->hasGpuMeshes()) {
        m_pendingScalars = m_scene->cachedScalars(); // shared_ptr copy (ref-count only)
        m_uploadScalarsPending = true;
        m_dirty = true;
    }

    // Carry the screenshot request across to render() (GL context current there).
    // Clear the visualizer copy so it is NOT re-copied on the next frame.
    m_pendingScreenshot = vv->m_pendingScreenshot;
    vv->m_pendingScreenshot.clear();

    // A signal-driven repaint request (viewChanged, lighting, colormap, mesh
    // update, screenshot, ...) sets m_needsRender on the visualizer. Propagate it
    // into this renderer's dirty flag so render() actually redraws; otherwise the
    // flag resets to false after the first frame and the viewport never refreshes
    // on mouse/wheel interaction.
    if (vv->m_needsRender) {
        m_dirty = true;
        vv->m_needsRender = false;
    }
}

QOpenGLFramebufferObject* ViewportFboRenderer::createFramebufferObject(const QSize& size) {
    m_fboSize = size;
    QOpenGLFramebufferObjectFormat format;
    format.setAttachment(QOpenGLFramebufferObject::Depth);
    format.setInternalTextureFormat(GL_RGBA8); // RGBA color attachment so transparent PNG exports retain an alpha channel
    format.setSamples(0); // raw GL draw handles MSAA itself if needed
    m_fbo = new QOpenGLFramebufferObject(size, format);
    // A newly-allocated FBO is uninitialized; force the next render() to draw
    // into it even if no other change occurred, otherwise the scene graph
    // composites garbage until the next interaction (e.g. when the sidebar
    // resizes the viewport or on window resize / DPR change).
    m_dirty = true;
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

    // Render only when something changed. The scene graph calls render() every
    // frame; skipping it when idle keeps the GPU (and fan) at rest instead of
    // redrawing the same image continuously. Continuous updates are still needed
    // while the turntable (autoRotate) or FPS HUD is on.
    const bool continuous = (m_scene->autoRotate() || m_scene->showFps());
    if (!m_dirty && !continuous && m_pendingScreenshot.isEmpty()) {
        return;
    }

    // Scalar-only re-upload: refresh just the sbo for the existing GPU meshes.
    if (m_uploadScalarsPending) {
        m_scene->updateScalarsOnGPU(m_pendingScalars);
        m_uploadScalarsPending = false;
    }

    // The mesh payload is uploaded inside renderFrame() (which consumes the
    // Renderer's pending shared_ptr under the GL context, set by
    // RenderSettings on the GUI thread). Mesh arrivals also emit meshDataUpdated,
    // which already flips this renderer's dirty flag via synchronize().

    m_scene->renderFrame();

    // capture here, NOT in synchronize() — synchronize runs on the GUI
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

    // Once we've drawn a fresh frame (and handled any capture), go idle until
    // the next dirty/continuous trigger. Keeps a static view from re-rendering.
    m_dirty = continuous;

    // FPS measurement (render thread). Sample frame intervals and push a
    // smoothed value to the GUI-thread settings so the HUD can display it.
    if (m_scene->showFps()) {
        const double now = m_fpsClock.elapsed() / 1000.0; // seconds
        const double dt = now - m_fpsLast;
        m_fpsLast = now;
        if (dt > 0.0 && dt < 1.0) {
            const double inst = 1.0 / dt;
            m_fpsSmoothed = (m_fpsSmoothed <= 0.0) ? inst : (m_fpsSmoothed * 0.9 + inst * 0.1);
            m_fpsAccum += dt;
            if (m_fpsAccum >= 0.25) {
                m_fpsAccum = 0.0;
                const int fps = static_cast<int>(std::round(m_fpsSmoothed));
                if (m_settings) {
                    QMetaObject::invokeMethod(m_settings, "setFpsText",
                                              Qt::QueuedConnection,
                                              Q_ARG(QString, QString("FPS: %1").arg(fps)));
                }
            }
        }
    }
}
