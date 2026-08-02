#include <glad/gl.h>
#include "viewport_widget.h"
#include "render/render_config.h"
#include <QOpenGLFramebufferObject>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QDebug>
#include <QFile>
#include <QTextStream>
#include <QTimer>
#include <QElapsedTimer>
#include <cmath>

ViewportWidget::ViewportWidget(int msaaSamples, QWidget* parent)
    : QOpenGLWidget(parent) {
    QSurfaceFormat fmt;
    fmt.setRenderableType(QSurfaceFormat::OpenGL);
    fmt.setVersion(4, 6);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setDepthBufferSize(24);
    fmt.setSamples(msaaSamples);
    setFormat(fmt);

    // FPS HUD samples elapsed() in paintGL(); start the clock or the first
    // frame delta is always 0 and the HUD never updates.
    m_fpsClock.start();
}

ViewportWidget::~ViewportWidget() = default;

void ViewportWidget::setSettings(::RenderSettings* s) {
    if (m_settings == s) return;
    m_settings = s;

    if (m_settings) {
        connect(m_settings, &::RenderSettings::meshDataUpdated, this, [this]() {
            m_dirty = true;
            update();
        });
        connect(m_settings, &::RenderSettings::screenshotRequested, this, [this](const QString& path) {
            m_pendingScreenshot = path;
            m_dirty = true;
            update();
        });
        connect(m_settings, &::RenderSettings::viewChanged, this, [this](ChangeFlags) {
            m_dirty = true;
            update();
        });
    }
    update();
}

void ViewportWidget::requestScreenshot(const QString& path) {
    m_pendingScreenshot = path;
    m_dirty = true;
    update();
}

void ViewportWidget::initializeGL() {
    initializeOpenGLFunctions();
    if (!m_settings) return;
    ::Renderer* scene = m_settings->backend();
    if (!scene) return;

    scene->initGLAD();
    scene->reinitForNewContext();
    loadShaders();
    scene->initGizmo();
    scene->reinitMeshData();
    m_initialized = true;
}

void ViewportWidget::loadShaders() {
    if (!m_settings) return;
    ::Renderer* scene = m_settings->backend();
    if (!scene) return;

    auto loadShader = [](const QString& rscPath) -> std::string {
        QFile file(rscPath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qCritical() << "Required shader asset missing:" << rscPath;
            return "";
        }
        return QTextStream(&file).readAll().toStdString();
    };

    ShaderSources sources;
    sources.meshVert        = loadShader(":/src/shaders/mesh.vert");
    sources.meshFrag        = loadShader(":/src/shaders/mesh.frag");
    sources.gridVert        = loadShader(":/src/shaders/grid.vert");
    sources.gridFrag        = loadShader(":/src/shaders/grid.frag");
    sources.glyphVert       = loadShader(":/src/shaders/glyph.vert");
    sources.glyphFrag       = loadShader(":/src/shaders/glyph.frag");
    sources.bboxVert        = loadShader(":/src/shaders/bbox.vert");
    sources.bboxFrag        = loadShader(":/src/shaders/bbox.frag");
    sources.streamlineVert  = loadShader(":/src/shaders/streamline.vert");
    sources.streamlineFrag  = loadShader(":/src/shaders/streamline.frag");
    sources.seedVert        = loadShader(":/src/shaders/seed.vert");
    sources.seedFrag        = loadShader(":/src/shaders/seed.frag");
    sources.particleVert    = loadShader(":/src/shaders/particle.vert");
    sources.particleFrag    = loadShader(":/src/shaders/particle.frag");
    sources.lodComp         = loadShader(":/src/shaders/lod.comp");
    sources.lodOutputComp   = loadShader(":/src/shaders/lod_output.comp");
    sources.lodTrisComp     = loadShader(":/src/shaders/lod_tris.comp");
    sources.qualityOverlayVert = loadShader(":/src/shaders/quality_overlay.vert");
    sources.qualityOverlayFrag = loadShader(":/src/shaders/quality_overlay.frag");
    sources.depthPeelVert = loadShader(":/src/shaders/depth_peel.vert");
    sources.depthPeelFrag = loadShader(":/src/shaders/depth_peel.frag");
    sources.compositeVert = loadShader(":/src/shaders/composite.vert");
    sources.compositeFrag = loadShader(":/src/shaders/composite.frag");

    scene->initShaders(sources);
}

void ViewportWidget::resizeGL(int w, int h) {
    if (!m_settings) { m_dirty = true; update(); return; }
    ::Renderer* scene = m_settings->backend();
    if (!scene) { m_dirty = true; update(); return; }
    const float dpr = static_cast<float>(devicePixelRatioF());
    scene->setDevicePixelRatio(dpr);
    scene->resizeViewport(w, h);
    m_dirty = true;
    update();
}

void ViewportWidget::paintGL() {
    if (!m_settings || !m_initialized) return;
    ::Renderer* scene = m_settings->backend();
    if (!scene) return;

    // Publish the current GUI state into the renderer snapshot.
    m_settings->publishRenderState(scene);

    const bool continuous = (scene->autoRotate() || scene->showFps()
                             || scene->isDashAnimating() || scene->isParticlesAnimating());
    if (!m_dirty && !continuous && m_pendingScreenshot.isEmpty()) return;

    // Scalar-only re-upload if needed.
    if (scene->consumeScalarDirty() && scene->hasGpuMeshes()) {
        auto scalars = scene->cachedScalars();
        if (scalars) scene->updateScalarsOnGPU(scalars);
    }

    scene->renderFrame();

    // Screenshot capture.
    if (!m_pendingScreenshot.isEmpty()) {
        // For QOpenGLWidget, read the widget's default framebuffer. When MSAA is
        // active that FBO is multisampled and glReadPixels is undefined, so the
        // renderer resolves it into a single-sample target first.
        const int fbW = static_cast<int>(width() * devicePixelRatio());
        const int fbH = static_cast<int>(height() * devicePixelRatio());
        scene->captureViewportFbo(defaultFramebufferObject(), fbW, fbH,
                                  format().samples(), m_pendingScreenshot);
        // Leave Qt's framebuffer binding intact for its post-paint handling.
        glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
        m_pendingScreenshot.clear();
    }

    m_dirty = continuous;

    // FPS measurement.
    if (scene->showFps()) {
        const double now = m_fpsClock.elapsed() / 1000.0;
        const double dt = now - m_fpsLast;
        m_fpsLast = now;
        if (dt > 0.0 && dt < 1.0) {
            const double inst = 1.0 / dt;
            m_fpsSmoothed = (m_fpsSmoothed <= 0.0) ? inst : (m_fpsSmoothed * 0.9 + inst * 0.1);
            m_fpsAccum += dt;
            if (m_fpsAccum >= 0.25) {
                m_fpsAccum = 0.0;
                const int fps = static_cast<int>(std::round(m_fpsSmoothed));
                m_settings->setFpsText(QString("FPS: %1").arg(fps));
            }
        }
    }
}

void ViewportWidget::mousePressEvent(QMouseEvent* event) {
    m_lastMousePos = event->pos();
    m_isRightClick = (event->button() == Qt::RightButton);
    event->accept();
}

void ViewportWidget::mouseMoveEvent(QMouseEvent* event) {
    if (!m_settings) return;
    QPoint delta = event->pos() - m_lastMousePos;
    m_lastMousePos = event->pos();
    if (m_isRightClick) {
        m_settings->pan(delta.x(), delta.y());
    } else {
        m_settings->azimuth(-delta.x() * RenderConfig::defaults().mouseSensitivity);
        m_settings->elevation(delta.y() * RenderConfig::defaults().mouseSensitivity);
    }
    m_dirty = true;
    update();
    event->accept();
}

void ViewportWidget::mouseReleaseEvent(QMouseEvent* event) {
    event->accept();
    QTimer::singleShot(RenderConfig::defaults().postMotionRedrawMs, this, [this]() {
        m_dirty = true;
        update();
    });
}

void ViewportWidget::wheelEvent(QWheelEvent* event) {
    if (!m_settings) return;
    double factor = (event->angleDelta().y() > 0) ? 1.1 : 0.9;
    m_settings->dolly(factor);
    m_dirty = true;
    update();
    QTimer::singleShot(RenderConfig::defaults().postMotionRedrawMs, this, [this]() {
        m_dirty = true;
        update();
    });
    event->accept();
}
