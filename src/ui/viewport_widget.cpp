#include <glad/gl.h>
#include "viewport_widget.h"
#include "colorbar_style_dialog.h"
#include "render/foundation/render_config.h"
#include <QColorSpace>
#include <QContextMenuEvent>
#include <QFileDialog>
#include <QMenu>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QStyle>
#include <QWheelEvent>
#include <QDebug>
#include <QFile>
#include <QTextStream>
#include <QTimer>
#include <QElapsedTimer>
#include <QPainter>
#include <cmath>

ViewportWidget::ViewportWidget(int msaaSamples, QWidget* parent)
    : QOpenGLWidget(parent), m_msaaSamples(msaaSamples) {
    QSurfaceFormat fmt;
    fmt.setRenderableType(QSurfaceFormat::OpenGL);
    fmt.setVersion(4, 6);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setDepthBufferSize(24);
    fmt.setSamples(msaaSamples);
    // Request sRGB-capable color buffer so GL_FRAMEBUFFER_SRGB does
    // linear -> sRGB conversion and blending stays in linear space
    // ( ParaView-like correct translucency ).
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    fmt.setColorSpace(QColorSpace(QColorSpace::SRgb));
#else
    fmt.setOption(QSurfaceFormat::sRGBColorSpace);
#endif
    setFormat(fmt);

    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setMouseTracking(true); // hover feedback for colorbar drag handles

    m_fpsLabel = new QLabel(this);
    m_fpsLabel->setAlignment(Qt::AlignLeft | Qt::AlignBottom);
    m_fpsLabel->hide();

    // FPS HUD samples elapsed() in paintGL(); start the clock or the first
    // frame delta is always 0 and the HUD never updates.
    m_fpsClock.start();

    // Spinner timer for loading indicator
    m_spinnerTimer = new QTimer(this);
    m_spinnerTimer->setInterval(16); // ~60fps
    connect(m_spinnerTimer, &QTimer::timeout, this, &ViewportWidget::advanceSpinner);
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
        connect(m_settings, &::RenderSettings::meshLoadStateChanged, this, [this]() {
            if (m_settings->isLoading()) {
                if (!m_spinnerTimer->isActive()) m_spinnerTimer->start();
            } else {
                m_spinnerTimer->stop();
                update();
            }
        });
        if (m_settings->isLoading()) m_spinnerTimer->start();
    }
    update();
}

void ViewportWidget::requestScreenshot(const QString& path) {
    m_pendingScreenshot = path;
    m_dirty = true;
    update();
}

QImage ViewportWidget::captureFrameImage(int width, int height, int samples, bool transparent) {
    ::Renderer* scene = m_settings ? m_settings->backend() : nullptr;
    if (!scene || width <= 0 || height <= 0) return QImage();

    makeCurrent();
    m_screenshotCapture.ensureScreenshotFbo(width, height, samples);
    glBindFramebuffer(GL_FRAMEBUFFER, m_screenshotCapture.screenshotFboId());
    scene->setViewportOverride(width, height);
    scene->renderFrame();
    scene->clearViewportOverride();
    QImage img = m_screenshotCapture.readFboToImage(
        m_screenshotCapture.screenshotFboId(), width, height, samples, transparent);
    glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
    doneCurrent();
    return img;
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
    sources.meshClipGeo     = loadShader(":/src/shaders/mesh_clip.geo");
    sources.meshWireVert    = loadShader(":/src/shaders/wire.vert");
    sources.meshWireGeo     = loadShader(":/src/shaders/wire.geo");
    sources.meshWireFrag    = loadShader(":/src/shaders/wire.frag");
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
    sources.volumeVert      = loadShader(":/src/shaders/volume.vert");
    sources.volumeFrag      = loadShader(":/src/shaders/volume.frag");
    sources.volumeSliceVert = loadShader(":/src/shaders/volume_slice.vert");
    sources.volumeSliceFrag = loadShader(":/src/shaders/volume_slice.frag");
    sources.lodComp         = loadShader(":/src/shaders/lod.comp");
    sources.lodOutputComp   = loadShader(":/src/shaders/lod_output.comp");
    sources.lodTrisComp     = loadShader(":/src/shaders/lod_tris.comp");
    sources.qualityOverlayVert = loadShader(":/src/shaders/quality_overlay.vert");
    sources.qualityOverlayFrag = loadShader(":/src/shaders/quality_overlay.frag");
    sources.depthPeelVert = loadShader(":/src/shaders/depth_peel.vert");
    sources.depthPeelFrag = loadShader(":/src/shaders/depth_peel.frag");
    sources.compositeVert = loadShader(":/src/shaders/composite.vert");
    sources.compositeFrag = loadShader(":/src/shaders/composite.frag");
    sources.pbrFragCommon = loadShader(":/src/shaders/pbr_common.glsl");

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

    if (m_fpsLabel) {
        m_fpsLabel->move(8, height() - m_fpsLabel->height() - 8);
    }
}

void ViewportWidget::paintGL() {
    if (!m_settings || !m_initialized) return;
    ::Renderer* scene = m_settings->backend();
    if (!scene) return;

    // Publish the current GUI state into the renderer snapshot.
    m_settings->publishRenderState(scene);

    const bool continuous = (scene->autoRotate() || scene->showFps()
                             || scene->isParticlesAnimating()
                             || scene->consumeLodSettle());
    if (!m_dirty && !continuous && m_pendingScreenshot.isEmpty()) return;

    // Scalar-only re-upload if needed.
    if (scene->consumeScalarDirty() && scene->hasGpuMeshes()) {
        auto scalars = scene->cachedScalars();
        if (scalars) scene->updateScalarsOnGPU(scalars);
        if (scene->consumeVolumeDirty() && scene->hasVolumeData()) {
            auto volMesh = scene->cachedVolumeMesh();
            if (volMesh) scene->uploadVolumeFromScalarDirty(m_settings->snapshot(), scalars, volMesh);
        }
    }

    // Ensure the persistent display FBO matches the current viewport.
    const int fbW = static_cast<int>(width() * devicePixelRatioF());
    const int fbH = static_cast<int>(height() * devicePixelRatioF());
    m_screenshotCapture.ensureDisplayFbo(fbW, fbH, m_msaaSamples);

    // Render into the persistent display FBO.
    glBindFramebuffer(GL_FRAMEBUFFER, m_screenshotCapture.displayFboId());
    scene->renderFrame();

    // Blit the display FBO to the default framebuffer for on-screen display.
    // MSAA resolve happens automatically if the display FBO is multisampled.
    const GLuint defaultFbo = static_cast<GLuint>(defaultFramebufferObject());
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, defaultFbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_screenshotCapture.displayFboId());
    glBlitFramebuffer(0, 0, fbW, fbH, 0, 0, fbW, fbH,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, defaultFbo);

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
        m_fpsLabel->setText(m_settings->getFpsText());
        m_fpsLabel->show();
    } else {
        m_fpsLabel->hide();
    }

    // Deferred screenshot: schedule capture after Qt composites the displayed frame.
    if (!m_pendingScreenshot.isEmpty()) {
        QString path = m_pendingScreenshot;
        m_pendingScreenshot.clear();
        QTimer::singleShot(0, this, [this, path]() { deferredCapture(path); });
    }
}

void ViewportWidget::deferredCapture(const QString& path) {
    if (!m_settings) return;

    if (!context()) {
        m_settings->screenshotCaptured(QString());
        return;
    }
    makeCurrent();

    ::Renderer* scene = m_settings->backend();
    if (!scene) {
        doneCurrent();
        m_settings->screenshotCaptured(QString());
        return;
    }

    m_settings->publishRenderState(scene);

    static constexpr struct { int w, h; } kResolutions[] = {
        {0, 0},       // [0] current viewport (special case)
        {1280, 720},  // [1] HD
        {1920, 1080}, // [2] Full HD
        {2560, 1440}, // [3] 2K
        {3840, 2160}, // [4] 4K
    };

    const int resMode = m_settings->getScreenshotResolution();
    const bool viewportRes = (resMode == 0);

    int fbW = 0;
    int fbH = 0;
    if (viewportRes) {
        fbW = static_cast<int>(width() * devicePixelRatioF());
        fbH = static_cast<int>(height() * devicePixelRatioF());
    } else {
        const int idx = (resMode >= 1 && resMode <= 4) ? resMode : 0;
        fbW = kResolutions[idx].w;
        fbH = kResolutions[idx].h;
    }

    const int samples = m_settings->getScreenshotAASamples();
    const bool transparent = m_settings->getScreenshotTransparent();

    ScreenshotCapture::Result result;

    if (viewportRes) {
        // Viewport resolution: read directly from the persistent display FBO (no re-render).
        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_screenshotCapture.displayFboId());
        result = m_screenshotCapture.readFboAndSave(
            m_screenshotCapture.displayFboId(),
            fbW, fbH, m_screenshotCapture.displayFboSamples(),
            transparent, path);
    } else {
        // Higher resolution: re-render into the persistent screenshot FBO.
        m_screenshotCapture.ensureScreenshotFbo(fbW, fbH, samples);

        glBindFramebuffer(GL_FRAMEBUFFER, m_screenshotCapture.screenshotFboId());
        scene->setViewportOverride(fbW, fbH);
        scene->renderFrame();
        scene->clearViewportOverride();

        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_screenshotCapture.screenshotFboId());
        result = m_screenshotCapture.readFboAndSave(
            m_screenshotCapture.screenshotFboId(),
            fbW, fbH, samples,
            transparent, path);
    }

    const GLuint defaultFbo = static_cast<GLuint>(defaultFramebufferObject());
    glBindFramebuffer(GL_FRAMEBUFFER, defaultFbo);

    glFinish();
    doneCurrent();
    m_settings->screenshotCaptured(result.success ? path : QString());
}

void ViewportWidget::mousePressEvent(QMouseEvent* event) {
    m_lastMousePos = event->pos();
    m_isMiddleClick = (event->button() == Qt::MiddleButton);

    if (event->button() == Qt::LeftButton && m_settings) {
        ::Renderer* scene = m_settings->backend();
        if (scene) {
            const float dpr = static_cast<float>(devicePixelRatioF());
            const int dw = static_cast<int>(width() * dpr);
            const int dh = static_cast<int>(height() * dpr);
            m_colorbarBars = scene->colorbarBars();
            int hitIdx = scene->colorbarIndexAt(static_cast<int>(event->pos().x() * dpr),
                                                 static_cast<int>(event->pos().y() * dpr),
                                                 m_colorbarBars);
            if (hitIdx >= 0) {
                beginColorbarDrag(event->pos(), hitIdx);
                event->accept();
                return;
            }
            // Axis triad: click an arrowhead to align the camera to that face;
            // clicking the same face again flips to its opposite.
            if (m_settings->isGizmoVisible()) {
                const int axisHit = scene->gizmoAxisAt(static_cast<int>(event->pos().x() * dpr),
                                                       static_cast<int>(event->pos().y() * dpr));
                if (axisHit >= 0) {
                    m_settings->snapGizmoAxis(axisHit);
                    event->accept();
                    return;
                }
            }
        }
    }
    event->accept();
}

void ViewportWidget::mouseMoveEvent(QMouseEvent* event) {
    if (!m_settings) return;

    if (m_draggingColorbar) {
        updateColorbarDrag(event->pos());
        event->accept();
        return;
    }

    if (event->buttons() == Qt::NoButton) {
        m_lastMousePos = event->pos();
        updateColorbarHitState();
        updateGizmoHover(event->pos());
        event->accept();
        return;
    }

    // Middle button (or shift+left) = pan, Left button = orbit
    const bool middleOrShift = m_isMiddleClick || (event->buttons() & Qt::MiddleButton);
    QPoint delta = event->pos() - m_lastMousePos;
    m_lastMousePos = event->pos();
    if (middleOrShift) {
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
    if (m_draggingColorbar) {
        endColorbarDrag();
        event->accept();
        return;
    }
    // Show context menu on right-click release (no drag happened)
    if (event->button() == Qt::RightButton && !m_isMiddleClick) {
        QContextMenuEvent* cme = new QContextMenuEvent(QContextMenuEvent::Mouse, event->pos(), event->globalPosition().toPoint());
        QCoreApplication::postEvent(this, cme);
    }
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

void ViewportWidget::contextMenuEvent(QContextMenuEvent* event) {
    if (!m_settings) return;
    ::Renderer* scene = m_settings->backend();

    // Right-click on a colorbar -> per-bar menu (orientation for now)
    if (scene) {
        const float dpr = static_cast<float>(devicePixelRatioF());
        m_colorbarBars = scene->colorbarBars();
        const int hitIdx = scene->colorbarIndexAt(static_cast<int>(event->pos().x() * dpr),
                                                  static_cast<int>(event->pos().y() * dpr),
                                                  m_colorbarBars);
        if (hitIdx >= 0) {
            const ColorbarData& bar = m_colorbarBars[hitIdx];
            const QString name = bar.subtitle.isEmpty()
                ? bar.title
                : QString("[%1] %2").arg(bar.subtitle, bar.title);

            QMenu menu(this);
            menu.addAction(name)->setDisabled(true);
            menu.addSeparator();
            QAction* showAct = menu.addAction("Show Bar");
            showAct->setCheckable(true);
            showAct->setChecked(bar.visible);
            QAction* horiz = menu.addAction("Horizontal");
            QAction* vert = menu.addAction("Vertical");
            horiz->setCheckable(true);
            vert->setCheckable(true);
            horiz->setChecked(bar.style.orientation == ColorbarStyle::Horizontal);
            vert->setChecked(bar.style.orientation == ColorbarStyle::Vertical);
            menu.addSeparator();
            QAction* styleAct = menu.addAction("Style...");

            QAction* chosen = menu.exec(event->globalPos());
            if (!chosen) {
                event->accept();
                return;
            }
            if (chosen == showAct) {
                scene->setColorbarVisible(hitIdx, showAct->isChecked());
                scene->commitColorbarPositions();
                m_dirty = true;
                update();
            } else if (chosen == styleAct) {
                ColorbarStyleDialog dlg(m_settings, this);
                dlg.exec();
            } else {
                const ColorbarStyle::Orientation target =
                    (chosen == vert) ? ColorbarStyle::Vertical : ColorbarStyle::Horizontal;
                if (target != bar.style.orientation) {
                    scene->setColorbarOrientation(hitIdx, target);
                    scene->commitColorbarPositions();
                    m_dirty = true;
                    update();
                }
            }
            event->accept();
            return;
        }
    }

    QMenu menu(this);
    menu.addAction("Reset Camera", this, [this]() { m_settings->resetCamera(); });
    menu.addAction("Zoom to Fit", this, [this]() { m_settings->resetCamera(); });
    menu.addSeparator();

    QAction* wireAction = menu.addAction("Toggle Wireframe", this, [this]() {
        m_settings->setWireframe(!m_settings->isWireframe());
    });
    wireAction->setCheckable(true);
    wireAction->setChecked(m_settings->isWireframe());

    QAction* surfAction = menu.addAction("Toggle Surface", this, [this]() {
        m_settings->toggleSurface(!m_settings->isSurfaceVisible());
    });
    surfAction->setCheckable(true);
    surfAction->setChecked(m_settings->isSurfaceVisible());

    // Colorbars submenu: per-bar show/hide for every currently-gated bar.
    // Hidden bars stay listed (unchecked) so they can always be restored.
    if (scene) {
        const std::vector<ColorbarData>& bars = scene->colorbarBars();
        if (!bars.empty()) {
            QMenu* cbMenu = menu.addMenu("Colorbars");
            for (const ColorbarData& bar : bars) {
                const QString barName = bar.subtitle.isEmpty()
                    ? bar.title
                    : QString("[%1] %2").arg(bar.subtitle, bar.title);
                QAction* act = cbMenu->addAction(barName);
                act->setCheckable(true);
                act->setChecked(bar.visible);
                connect(act, &QAction::toggled, this,
                        [this, scene, subtitle = bar.subtitle](bool on) {
                    const std::vector<ColorbarData>& current = scene->colorbarBars();
                    for (int k = 0; k < static_cast<int>(current.size()); ++k) {
                        if (current[k].subtitle == subtitle) {
                            scene->setColorbarVisible(k, on);
                            scene->commitColorbarPositions();
                            break;
                        }
                    }
                    m_dirty = true;
                    update();
                });
            }
        }
    }

    menu.addSeparator();
    menu.addAction("Save Screenshot...", this, [this]() {
        QString path = QFileDialog::getSaveFileName(nullptr, "Save Screenshot", QString(),
            "PNG (*.png);;JPEG (*.jpg);;All files (*)");
        if (!path.isEmpty()) requestScreenshot(path);
    });

    menu.exec(event->globalPos());
}

void ViewportWidget::paintEvent(QPaintEvent* event) {
    m_dirty = true;
    QOpenGLWidget::paintEvent(event);

    if (!m_settings) return;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (m_settings->isLoading()) {
        drawSpinner(painter);
    } else if (!m_settings->getHasMeshLoaded()) {
        drawEmptyState(painter);
    }
}

void ViewportWidget::drawEmptyState(QPainter& painter) {
    const int cx = width() / 2;
    const int cy = height() / 2;

    // Icon
    QPixmap pix = style()->standardPixmap(QStyle::SP_FileIcon);
    QPixmap scaled = pix.scaled(32, 32, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    painter.drawPixmap(cx - scaled.width() / 2, cy - 40, scaled);

    // Text
    QFont f = painter.font();
    f.setPixelSize(16);
    painter.setFont(f);
    painter.setPen(palette().color(QPalette::Text));
    QRect textRect(cx - 120, cy, 240, 24);
    painter.drawText(textRect, Qt::AlignCenter, "Drop a file here");

    f.setPixelSize(13);
    painter.setFont(f);
    painter.setPen(palette().color(QPalette::Text));
    textRect.translate(0, 28);
    painter.drawText(textRect, Qt::AlignCenter, "or  File > Open Mesh");
}

void ViewportWidget::drawSpinner(QPainter& painter) {
    const int cx = width() / 2;
    const int cy = height() / 2;
    const int radius = 22;

    QPalette pal = palette();
    QColor bg = pal.color(QPalette::Window);
    bg.setAlpha(180);
    painter.setPen(Qt::NoPen);
    painter.setBrush(bg);
    painter.drawEllipse(QPoint(cx, cy), radius + 8, radius + 8);

    QPen pen(pal.color(QPalette::Highlight), 3);
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    const int startAngle = m_spinnerAngle * 16;
    painter.drawArc(cx - radius, cy - radius, radius * 2, radius * 2,
                    startAngle, 270 * 16);

    painter.setPen(pal.color(QPalette::Text));
    QFont f = painter.font();
    f.setPixelSize(12);
    painter.setFont(f);
    QRect textRect(cx - 60, cy + radius + 12, 120, 20);
    painter.drawText(textRect, Qt::AlignCenter, "Parsing mesh...");
}

void ViewportWidget::advanceSpinner() {
    m_spinnerAngle = (m_spinnerAngle + 6) % 360;
    update();
}

void ViewportWidget::updateColorbarHitState() {
    m_colorbarHover = false;
    if (!m_settings || m_draggingColorbar) return;
    ::Renderer* scene = m_settings->backend();
    if (!scene) return;
    m_colorbarBars = scene->colorbarBars();
    const float dpr = static_cast<float>(devicePixelRatioF());
    int hitIdx = scene->colorbarIndexAt(static_cast<int>(m_lastMousePos.x() * dpr),
                                         static_cast<int>(m_lastMousePos.y() * dpr),
                                         m_colorbarBars);
    m_colorbarHover = (hitIdx >= 0);
    setCursor(m_colorbarHover ? Qt::OpenHandCursor : Qt::ArrowCursor);
}

void ViewportWidget::updateGizmoHover(const QPoint& pos) {
    ::Renderer* scene = m_settings ? m_settings->backend() : nullptr;
    int axis = -1;
    if (scene && m_settings->isGizmoVisible()) {
        const float dpr = static_cast<float>(devicePixelRatioF());
        axis = scene->gizmoAxisAt(static_cast<int>(pos.x() * dpr),
                                  static_cast<int>(pos.y() * dpr));
    }
    if (axis == m_gizmoHoverAxis) return;
    m_gizmoHoverAxis = axis;
    if (scene) scene->setGizmoHoverAxis(axis);
    // Cursor priority: triad > colorbar > default. updateColorbarHitState()
    // has already run this event, so only assert PointingHand here and yield
    // to the colorbar's OpenHand when we just left the triad.
    if (axis >= 0) {
        setCursor(Qt::PointingHandCursor);
    } else if (!m_colorbarHover) {
        setCursor(Qt::ArrowCursor);
    }
    m_dirty = true;
    update();
}

void ViewportWidget::leaveEvent(QEvent* event) {
    if (m_gizmoHoverAxis >= 0) {
        m_gizmoHoverAxis = -1;
        if (m_settings) {
            if (::Renderer* scene = m_settings->backend()) scene->setGizmoHoverAxis(-1);
        }
        setCursor(Qt::ArrowCursor);
        update();
    }
    QOpenGLWidget::leaveEvent(event);
}

void ViewportWidget::beginColorbarDrag(const QPoint& pos, int barIndex) {
    m_draggingColorbar = true;
    m_dragBarIndex = barIndex;
    m_dragStartPos = pos;
    setCursor(Qt::ClosedHandCursor);
}

void ViewportWidget::updateColorbarDrag(const QPoint& pos) {
    if (!m_settings || m_dragBarIndex < 0) return;
    ::Renderer* scene = m_settings->backend();
    if (!scene) return;

    // Refresh from renderer — it holds the updated position
    m_colorbarBars = scene->colorbarBars();

    const float dpr = static_cast<float>(devicePixelRatioF());
    const int dw = static_cast<int>(width() * dpr);
    const int dh = static_cast<int>(height() * dpr);

    // Get current bar rect
    QRectF rect = scene->colorbarBarRect(dpr, dw, dh, m_colorbarBars[m_dragBarIndex]);
    const float contentW = rect.width();
    const float contentH = rect.height();

    const QPoint delta = pos - m_dragStartPos;

    float fracX = qBound(0.0f, (rect.left() + delta.x() * dpr) / (dw - contentW), 1.0f);
    float fracY = qBound(0.0f, (rect.top() + delta.y() * dpr) / (dh - contentH), 1.0f);

    scene->setColorbarPosition(m_dragBarIndex, fracX, fracY);
    m_dragStartPos = pos;
    m_dirty = true;
    update();
}

void ViewportWidget::endColorbarDrag() {
    if (m_settings) {
        if (::Renderer* scene = m_settings->backend())
            scene->commitColorbarPositions();
    }
    m_draggingColorbar = false;
    m_dragBarIndex = -1;
    setCursor(Qt::ArrowCursor);
}

bool ViewportWidget::event(QEvent* event) {
    if (event->type() == QEvent::Expose || event->type() == QEvent::WindowActivate) {
        m_dirty = true;
        update();
    }
    return QOpenGLWidget::event(event);
}


