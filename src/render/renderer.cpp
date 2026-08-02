#include "render/renderer.h"
#include "render/shader_utils.h"
#include "render/render_config.h"
#include "render/StreamlineSet.h"
#include "core/Colormaps.h"
#include "core/Camera.h"
#include "core/mesh_loader.h"
#include <QOpenGLFramebufferObject>

#include <cstring>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <memory>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>
#include <limits>
#include <unordered_map>
#include <QImage>
#include <QBuffer>
#include <QTimer>
#include <QPainter>
#include <QFont>

#include <QDir>
#include <QDateTime>
#include <QRegularExpression>
#include <QFileInfo>
#include <QOpenGLContext>
#include <QSettings>

Renderer::Renderer()
    : m_state() {
    // Default system initialization parameters (mirror RenderSettings defaults;
    // the first synchronize() overwrites these with the GUI snapshot).
    m_state.meshColor[0] = 0.4f; m_state.meshColor[1] = 0.9f; m_state.meshColor[2] = 0.4f;
    m_state.surfaceColor[0] = 1.0f; m_state.surfaceColor[1] = 1.0f; m_state.surfaceColor[2] = 1.0f;
    m_state.bgColor[0] = 0.12f; m_state.bgColor[1] = 0.12f; m_state.bgColor[2] = 0.12f;
    m_state.worldCenterX = 0.0; m_state.worldCenterY = 0.0; m_state.worldCenterZ = 0.0;
    m_state.worldRadius = 1.0;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
Renderer::~Renderer() {
    m_destroying = true;

    // Join the background streamline worker before destroying GL resources.
    m_streamlines.cancelAndJoin();

    if (QOpenGLContext::currentContext()) {
        if (meshUbo) glDeleteBuffers(1, &meshUbo);
        if (glyphUbo) glDeleteBuffers(1, &glyphUbo);
        if (shaderProgram) glDeleteProgram(shaderProgram);
        colormap.shutdown();
        vectorGlyph.shutdown();
        streamlineSet.shutdown();
        gizmo.shutdown();
        colorbarOverlay.shutdown();
        m_grid.shutdown();
        m_bbox.shutdown();
        m_qualityOverlay.shutdown();
        m_streamlines.shutdown();
        destroyPeelFbos();
        if (m_peelProgram) { glDeleteProgram(m_peelProgram); m_peelProgram = 0; }
        if (m_compositeProgram) { glDeleteProgram(m_compositeProgram); m_compositeProgram = 0; }
    }
}
#pragma GCC diagnostic pop

void Renderer::initGLAD() {
    QOpenGLContext* currentContext = QOpenGLContext::currentContext();
    if (!currentContext) {
        qFatal("Fatal: initGLAD called but no active Qt OpenGL context was found on this thread.");
        return;
    }

    if (!gladLoaderLoadGL()) {
        qFatal("Fatal: GLAD failed to load core OpenGL functions.");
    }

    qDebug() << "[GL DIAGNOSTIC] VERSION:" << (const char*)glGetString(GL_VERSION)
             << "| RENDERER:" << (const char*)glGetString(GL_RENDERER)
             << "| GLSL:" << (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);
}

void Renderer::initShaders(const ShaderSources& sources) {
    if (sources.meshVert.empty() || sources.meshFrag.empty()) {
        qFatal("Shader compilation aborted due to unreadable file streams.");
        return;
    }

    // mesh program
    shaderProgram = compileProgram(sources.meshVert.c_str(), sources.meshFrag.c_str(), "Mesh");
    if (shaderProgram != 0) {
        meshUboIndex = glGetUniformBlockIndex(shaderProgram, "MeshUBO");
        glUniformBlockBinding(shaderProgram, meshUboIndex, 0);
        lutTextureLoc = glGetUniformLocation(shaderProgram, "uColormapLUT");
    }

    // instanced vector glyph program
    if (!sources.glyphVert.empty() && !sources.glyphFrag.empty()) {
        glyphProgram = compileProgram(sources.glyphVert.c_str(), sources.glyphFrag.c_str(), "Glyph");
        if (glyphProgram != 0) {
            glyphLutLoc = glGetUniformLocation(glyphProgram, "uColormapLUT");
            glyphUboIndex = glGetUniformBlockIndex(glyphProgram, "GlyphUBO");
            if (glyphUboIndex != GL_INVALID_INDEX) {
                glUniformBlockBinding(glyphProgram, glyphUboIndex, 1);
                glCreateBuffers(1, &glyphUbo);
                glNamedBufferData(glyphUbo, sizeof(GlyphUBOData), nullptr, GL_DYNAMIC_DRAW);
                glBindBufferBase(GL_UNIFORM_BUFFER, 1, glyphUbo);
            }
        }
    }

    // particle program
    if (!sources.particleVert.empty() && !sources.particleFrag.empty()) {
        particleProgram = compileProgram(sources.particleVert.c_str(), sources.particleFrag.c_str(), "Particle");
        if (particleProgram != 0) {
            particleColorLoc = glGetUniformLocation(particleProgram, "uColor");
            particleLutLoc = glGetUniformLocation(particleProgram, "uColormapLUT");
            particlePointSizeLoc = glGetUniformLocation(particleProgram, "uPointSize");
            particleUseColormapLoc = glGetUniformLocation(particleProgram, "uUseColormap");
            particleMagRangeLoc = glGetUniformLocation(particleProgram, "uParticleMagRange");
        }
    }

    meshManager.setComputeShaderSources(sources.lodComp, sources.lodOutputComp, sources.lodTrisComp);

    m_grid.init(sources);
    m_bbox.init(sources);
    m_qualityOverlay.init(sources);
    m_streamlines.init(sources);

    // depth peeling shaders for transparent surfaces
    if (!sources.depthPeelVert.empty() && !sources.depthPeelFrag.empty()) {
        m_peelProgram = compileProgram(sources.depthPeelVert.c_str(), sources.depthPeelFrag.c_str(), "DepthPeel");
        if (m_peelProgram != 0) {
            m_peelPrevDepthLoc = glGetUniformLocation(m_peelProgram, "uPrevDepth");
            m_peelLayerLoc    = glGetUniformLocation(m_peelProgram, "uPeelLayer");
        }
    }
    if (!sources.compositeVert.empty() && !sources.compositeFrag.empty()) {
        m_compositeProgram = compileProgram(sources.compositeVert.c_str(), sources.compositeFrag.c_str(), "Composite");
    }
}

void Renderer::initGizmo() {
    gizmo.init();
    colorbarOverlay.init();
}

// ---------------------------------------------------------------------------
// Depth peeling — two-layer OIT for transparent surfaces
// ---------------------------------------------------------------------------
void Renderer::ensurePeelFbos(int w, int h) {
    if (m_peelFboW == w && m_peelFboH == h && m_peelFbo[0] != 0) return;
    destroyPeelFbos();
    m_peelFboW = w; m_peelFboH = h;

    for (int i = 0; i < 2; ++i) {
        glCreateFramebuffers(1, &m_peelFbo[i]);
        glCreateTextures(GL_TEXTURE_2D, 1, &m_peelColorTex[i]);
        glTextureStorage2D(m_peelColorTex[i], 1, GL_RGBA8, w, h);
        glCreateTextures(GL_TEXTURE_2D, 1, &m_peelDepthTex[i]);
        glTextureStorage2D(m_peelDepthTex[i], 1, GL_DEPTH24_STENCIL8, w, h);

        glNamedFramebufferTexture(m_peelFbo[i], GL_COLOR_ATTACHMENT0, m_peelColorTex[i], 0);
        glNamedFramebufferTexture(m_peelFbo[i], GL_DEPTH_STENCIL_ATTACHMENT, m_peelDepthTex[i], 0);
    }

    // 1x1 dummy depth texture initialized to 1.0 for the first peel pass
    glCreateTextures(GL_TEXTURE_2D, 1, &m_peelDummyDepth);
    glTextureStorage2D(m_peelDummyDepth, 1, GL_DEPTH24_STENCIL8, 1, 1);
    uint32_t depthOne[2] = { 0xFFFFFFFF, 0 };
    glTextureSubImage2D(m_peelDummyDepth, 0, 0, 0, 1, 1, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, depthOne);

    // empty VAO for fullscreen triangle (composite shader uses gl_VertexID only)
    if (!m_peelDummyVao) glCreateVertexArrays(1, &m_peelDummyVao);
}

void Renderer::destroyPeelFbos() {
    for (int i = 0; i < 2; ++i) {
        if (m_peelFbo[i])        { glDeleteFramebuffers(1, &m_peelFbo[i]);        m_peelFbo[i] = 0; }
        if (m_peelColorTex[i])   { glDeleteTextures(1, &m_peelColorTex[i]);       m_peelColorTex[i] = 0; }
        if (m_peelDepthTex[i])   { glDeleteTextures(1, &m_peelDepthTex[i]);       m_peelDepthTex[i] = 0; }
    }
    if (m_peelDummyDepth) { glDeleteTextures(1, &m_peelDummyDepth); m_peelDummyDepth = 0; }
    if (m_peelDummyVao) { glDeleteVertexArrays(1, &m_peelDummyVao); m_peelDummyVao = 0; }
    m_peelFboW = m_peelFboH = 0;
}

void Renderer::renderTransparent(const glm::mat4& view, const glm::mat4& proj,
                                  const MeshUBOData& ubo, GLuint meshUbo,
                                  const std::vector<std::pair<GLuint, int>>& transparentMeshes) {
    if (m_peelProgram == 0 || m_compositeProgram == 0 || transparentMeshes.empty()) return;

    int vpW = static_cast<int>(width * devicePixelRatio);
    int vpH = static_cast<int>(height * devicePixelRatio);
    ensurePeelFbos(vpW, vpH);

    // Save the caller's FBO (QOpenGLWidget's offscreen FBO) so we can composite
    // back to it instead of FBO 0 (the screen default framebuffer).
    GLint prevFbo;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);

    glDisable(GL_CULL_FACE);

    // ---- Layer 0: standard depth test into FBO 0 ----
    glBindFramebuffer(GL_FRAMEBUFFER, m_peelFbo[0]);
    glViewport(0, 0, vpW, vpH);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);

    glUseProgram(m_peelProgram);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, meshUbo);
    glBindTextureUnit(0, m_peelDummyDepth);
    glUniform1i(m_peelPrevDepthLoc, 0);
    glUniform1f(m_peelLayerLoc, 0.0f);

    for (const auto& mesh : transparentMeshes) {
        glBindVertexArray(mesh.first);
        glDrawElements(GL_TRIANGLES, mesh.second, GL_UNSIGNED_INT, 0);
    }

    // ---- Layer 1: peel against layer 0 depth into FBO 1 ----
    glBindFramebuffer(GL_FRAMEBUFFER, m_peelFbo[1]);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDepthFunc(GL_LESS);

    glBindTextureUnit(0, m_peelDepthTex[0]);
    glUniform1f(m_peelLayerLoc, 1.0f);

    for (const auto& mesh : transparentMeshes) {
        glBindVertexArray(mesh.first);
        glDrawElements(GL_TRIANGLES, mesh.second, GL_UNSIGNED_INT, 0);
    }

    glBindVertexArray(0);
    glUseProgram(0);

    // ---- Composite: back-to-front onto the caller's framebuffer ----
    glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
    glViewport(0, 0, vpW, vpH);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(m_compositeProgram);
    glBindTextureUnit(0, m_peelColorTex[0]);
    glUniform1i(glGetUniformLocation(m_compositeProgram, "uLayer0"), 0);
    glBindTextureUnit(1, m_peelColorTex[1]);
    glUniform1i(glGetUniformLocation(m_compositeProgram, "uLayer1"), 1);

    glDepthMask(GL_FALSE);
    glBindVertexArray(m_peelDummyVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glDepthMask(GL_TRUE);

    glUseProgram(0);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDisable(GL_CULL_FACE);
}

void Renderer::uploadMesh(std::shared_ptr<const RenderMesh> renderMesh) {
    if (!renderMesh) return;
    meshManager.upload(renderMesh);
    m_lastUploadedMesh = renderMesh;
    vectorGlyph.rebuild(*renderMesh, m_state.vectorStride, m_state.vectorField, m_state.vectorMagTransform);
    streamlineSet.rebuild(*renderMesh, m_state.streamlineSeedCount, m_state.streamlineStepSize, m_state.streamlineMaxSteps, m_state.streamlineVectorField, m_state.seedMode, m_state.seedPlanePos, m_state.seedJitter, m_state.seedPlaneCountU, m_state.seedPlaneCountV, m_state.showStreamlineArrows, m_state.streamlineArrowSpacing, m_state.streamlineArrowSize, m_state.streamlineRibbonWidth, m_state.streamlineTaperFactor);
}

void Renderer::setPendingMesh(std::shared_ptr<const RenderMesh> renderMesh) {
    std::lock_guard<std::mutex> lock(meshQueueMutex);
    m_pendingMesh = std::move(renderMesh);
    meshManager.meshChanged = true;
}

void Renderer::markCameraMoving() {
    cameraMoving = true;
    m_lastMotion = std::chrono::steady_clock::now();
}

void Renderer::computeLightDirections(glm::vec3& key, glm::vec3& fill, glm::vec3& back1, glm::vec3& back2, glm::vec3& head) {
    m_state.lighting.computeDirections(m_state.camera.position, m_state.camera.focalPoint, m_state.camera.viewUp,
                               key, fill, back1, back2, head);
}

void Renderer::applyLightingPreset(int preset) {
    m_state.lighting.applyPreset(preset);
}

void Renderer::resetLighting() {
    m_state.lighting.reset();
}

void Renderer::resetCamera() {
    Camera& camera = m_state.camera;
    camera.focalPoint = glm::dvec3(m_state.worldCenterX, m_state.worldCenterY, m_state.worldCenterZ);

    const double dx = m_state.worldMaxX - m_state.worldMinX;
    const double dy = m_state.worldMaxY - m_state.worldMinY;
    const double dz = m_state.worldMaxZ - m_state.worldMinZ;
    const double diag = std::sqrt(dx * dx + dy * dy + dz * dz);
    const double fitRadius = diag * 0.5;

    const double aspect = (height > 0) ? (static_cast<double>(width) / static_cast<double>(height)) : 1.0;
    const double fov = glm::radians(45.0);
    const double vFov = fov;
    const double hFov = 2.0 * std::atan(std::tan(fov * 0.5) * aspect);
    const double effFov = std::min(vFov, hFov);
    double dist = fitRadius / std::tan(effFov * 0.5);
    dist *= RenderConfig::defaults().cameraFitMultiplier;

    camera.distance = dist;
    // ponytail: keep ortho zoom baseline in sync with the fit distance so
    // dolly-zoom tracks correctly after a reset (m_orthoRefDist is seeded once).
    m_orthoRefDist = camera.distance;
    if (camera.distance < 1.0) camera.distance = 1.0;
    camera.maxDistance = std::max(1000.0, camera.distance * 50.0);
    nearPlane = std::max(0.01, camera.distance * 0.001);
    farPlane  = std::max(100.0, camera.distance * 20.0);
    camera.position = camera.focalPoint + glm::dvec3(0.0, 0.0, camera.distance);
    camera.viewUp = glm::dvec3(0.0, 1.0, 0.0);
    camera.orthogonalizeViewUp();
}

void Renderer::snapToOrthoView(int axis) {
    m_state.camera.snapToOrthoView(axis);
}

void Renderer::snapToAxisView(int axis, bool flip) {
    int preset = flip ? (axis * 2 + 1) : (axis * 2);
    m_state.camera.snapToOrthoView(preset);
}

void Renderer::resizeViewport(int w, int h) {
    width = w;
    height = h;
}

void Renderer::clearGpuMeshes() {
    // Join the background streamline worker before tearing down GL resources.
    m_streamlines.cancelAndJoin();

    meshManager.clear();
    vectorGlyph = VectorGlyphSet{};
    streamlineSet.shutdown();
    m_lastUploadedMesh.reset();
    m_pendingMesh.reset();
    m_state.hasMeshLoaded = false;
    m_qualityOverlay.markDirty();
    m_qualityOverlay.shutdown();
}

bool Renderer::consumeScalarDirty() {
    return scalarDirty.exchange(false);
}

void Renderer::updateScalarsOnGPU(std::shared_ptr<const std::vector<float>> scalars) {
    {
        std::lock_guard<std::mutex> lock(meshQueueMutex);
        m_pendingScalarSrc = scalars; // shared_ptr, no data copy
    }
    meshManager.updateScalars(scalars);
}

bool Renderer::captureViewportToFile(const QString& path) {
    if (path.isEmpty()) return false;
    if (!m_viewportFbo || !m_viewportFbo->isValid()) {
        qWarning() << "Screenshot skipped: viewport FBO not available.";
        return false;
    }

    const bool isPng = path.endsWith(".png", Qt::CaseInsensitive);
    const bool transparent = isPng && m_state.screenshotTransparent;

    // ponytail: MSAA FBOs cannot be read back with glReadPixels (undefined);
    // resolve to a single-sample target first.
    QOpenGLFramebufferObject* live = m_viewportFbo;
    QOpenGLFramebufferObject* readFbo = live;
    std::unique_ptr<QOpenGLFramebufferObject> resolveHolder;
    if (live->format().samples() > 0) {
        QOpenGLFramebufferObjectFormat rf;
        rf.setInternalTextureFormat(GL_RGBA8);
        resolveHolder = std::make_unique<QOpenGLFramebufferObject>(live->size(), rf);
        QOpenGLFramebufferObject::blitFramebuffer(resolveHolder.get(), live, GL_COLOR_BUFFER_BIT);
        readFbo = resolveHolder.get();
    }

    const int w = readFbo->width();
    const int h = readFbo->height();
    const int channels = transparent ? 4 : 3;
    const GLenum fmt = transparent ? GL_RGBA : GL_RGB;
    std::vector<unsigned char> raw(static_cast<size_t>(w) * h * channels);

    glBindFramebuffer(GL_FRAMEBUFFER, readFbo->handle());
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, fmt, GL_UNSIGNED_BYTE, raw.data());

    // Flip vertically (GL origin is bottom-left).
    std::vector<unsigned char> flipped(raw.size());
    const size_t row = static_cast<size_t>(w) * channels;
    for (int y = 0; y < h; ++y)
        std::memcpy(flipped.data() + static_cast<size_t>(y) * row,
                    raw.data() + static_cast<size_t>(h - 1 - y) * row, row);

    QImage::Format qf = transparent ? QImage::Format_RGBA8888 : QImage::Format_RGB888;
    QImage img = QImage(flipped.data(), w, h, static_cast<int>(row), qf).copy();

    const char* token = isPng ? "PNG"
                               : (path.endsWith(".bmp", Qt::CaseInsensitive) ? "BMP" : "JPG");
    const bool ok = img.save(path, token, -1);
    if (!ok) qWarning() << "Screenshot save failed:" << path;
    return ok;
}

void Renderer::drawGizmo() {
    GLboolean depthWas = glIsEnabled(GL_DEPTH_TEST);
    glDisable(GL_DEPTH_TEST);
    gizmo.draw(m_state.camera.getViewMatrix(), static_cast<float>(devicePixelRatio));
    if (m_state.lighting.lightKitEnabled && m_state.lighting.showLightMarkers) {
        glm::vec3 kitDirs[5] = {
            LightingModel::kitDirection(m_state.lighting.lightKeyAzimuth,  m_state.lighting.lightKeyElevation),
            LightingModel::kitDirection(m_state.lighting.lightFillAzimuth, m_state.lighting.lightFillElevation),
            LightingModel::kitDirection(m_state.lighting.lightBackAzimuth,  m_state.lighting.lightBackElevation),
            LightingModel::kitDirection(m_state.lighting.lightBackAzimuth + 180.0f, -m_state.lighting.lightBackElevation),
            LightingModel::kitDirection(m_state.lighting.lightHeadAzimuth,  m_state.lighting.lightHeadElevation),
        };
        auto warmTint = [](float w) -> glm::vec3 {
            if (w < 0.5f) return glm::mix(glm::vec3(0.6f,0.7f,1.0f), glm::vec3(1.0f), w/0.5f);
            return glm::mix(glm::vec3(1.0f), glm::vec3(1.0f,0.85f,0.7f), (w-0.5f)/0.5f);
        };
        glm::vec3 tint = warmTint(m_state.lighting.lightWarm);
        glm::vec3 cols[5] = { tint, tint * 0.9f, tint * 0.95f, tint * 0.95f, glm::vec3(1.0f, 1.0f, 1.0f) };
        gizmo.drawLights(kitDirs, cols, static_cast<float>(devicePixelRatio));
    }
    if (depthWas) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
}

void Renderer::drawColorbarLegends(int deviceW, int deviceH) {
    if (deviceW <= 0 || deviceH <= 0) return;
    const float dpr = static_cast<float>(devicePixelRatio);

    const int maxLegendW = static_cast<int>(width * dpr);
    const int maxLegendH = static_cast<int>(height * dpr);
    if (deviceW > maxLegendW) deviceW = maxLegendW;
    if (deviceH > maxLegendH) deviceH = maxLegendH;
    if (deviceW <= 0 || deviceH <= 0) return;

    const auto stopsFor = [&](int choice, bool reversed) {
        QVariantList out;
        const int steps = 16;
        for (int i = 0; i <= steps; ++i) {
            float t = static_cast<float>(i) / static_cast<float>(steps);
            float s = reversed ? (1.0f - t) : t;
            glm::vec3 c = Colormaps::evaluate(s, static_cast<ColormapType>(choice));
            QVariantList stop;
            stop << t << c.r << c.g << c.b;
            out.append(QVariant(stop));
        }
        return out;
    };

    std::vector<ColorbarData> bars;
    const int tickCount = m_state.colorbarTicks;

    // Scalar bar
    if (m_state.hasMeshLoaded && m_state.meshHasScalars && m_state.meshUseScalarColor && m_state.showScalarColorbar) {
        ColorbarData d;
        d.visible = true;
        d.title = QString::fromStdString(m_state.activeScalarName);
        d.stops = stopsFor(m_state.colormapChoice, m_state.colormapReversed);
        const float range = m_state.dataScalarMax - m_state.dataScalarMin;
        for (int i = 0; i < tickCount; ++i) {
            const float frac = tickCount > 1 ? static_cast<float>(i) / static_cast<float>(tickCount - 1) : 0.0f;
            const float v = m_state.dataScalarMin + range * frac;
            d.tickLabels.append(QString::number(v, 'f', 3));
        }
        bars.push_back(d);
    }

    // Vector bar
    if (m_state.showVectors && m_state.vectorUseColormap && m_state.hasMeshLoaded) {
        ColorbarData d;
        d.visible = true;
        d.title = QString::fromStdString(m_state.vectorField) + QChar(0x27A1);
        d.stops = stopsFor(m_state.vectorColormapChoice, m_state.vectorColormapReversed);
        auto txMag = [&](float m) -> float {
            if (m_state.vectorMagTransform == 1) return std::sqrt(std::max(m, 0.0f));
            if (m_state.vectorMagTransform == 2) return std::log(1.0f + std::max(m, 0.0f));
            return m;
        };
        auto invTxMag = [&](float t) -> float {
            if (m_state.vectorMagTransform == 1) return t * t;
            if (m_state.vectorMagTransform == 2) return std::exp(t) - 1.0f;
            return t;
        };
        const float tMin = txMag(vectorGlyph.magMin);
        const float tMax = txMag(vectorGlyph.magMax);
        const float tRange = tMax - tMin;
        for (int i = 0; i < tickCount; ++i) {
            const float frac = tickCount > 1 ? static_cast<float>(i) / static_cast<float>(tickCount - 1) : 0.0f;
            const float t = tMin + tRange * frac;
            const float v = invTxMag(t);
            d.tickLabels.append(QString::number(v, 'f', 3));
        }
        bars.push_back(d);
    }

    // Streamline bar
    if (m_state.showStreamlines && m_state.streamlineUseColormap && m_state.hasMeshLoaded) {
        ColorbarData d;
        d.visible = true;
        d.title = QString::fromStdString(m_state.streamlineVectorField) + QChar(0x27A1);
        d.stops = stopsFor(m_state.streamlineColormapChoice, m_state.streamlineColormapReversed);
        const float sMin = streamlineSet.magMin;
        const float sMax = streamlineSet.magMax;
        const float sRange = sMax - sMin;
        for (int i = 0; i < tickCount; ++i) {
            const float frac = tickCount > 1 ? static_cast<float>(i) / static_cast<float>(tickCount - 1) : 0.0f;
            const float v = sMin + sRange * frac;
            d.tickLabels.append(QString::number(v, 'f', 3));
        }
        bars.push_back(d);
    }

    colorbarOverlay.drawBars(dpr, deviceW, deviceH, bars);
}

void Renderer::renderFrame() {
    // Advance animation clock with real elapsed time (drives dash/arrow animation).
    {
        auto now = std::chrono::steady_clock::now();
        if (m_lastFrameTime.time_since_epoch().count() == 0) m_lastFrameTime = now;
        double dt = std::chrono::duration<double>(now - m_lastFrameTime).count();
        m_lastFrameTime = now;
        m_lastFrameDt = dt;
        m_animationTime += dt;
    }

    // LOD debounce: once 140 ms have elapsed since the last camera motion, clear
    // the moving flag so the next frame uses the full-resolution mesh.
    if (cameraMoving.load()) {
        auto now = std::chrono::steady_clock::now();
        auto dt = std::chrono::duration<double>(now - m_lastMotion).count();
        if (dt >= RenderConfig::defaults().lodDebounceSeconds) cameraMoving = false;
    }

    // Compute LOD throttle: only re-dispatch once per camera-motion burst.
    // Without this, every frame during a 140 ms camera-moving window would
    // run the full 3-pass compute pipeline plus read-back, causing driver stalls.
    if (cameraMoving.load() && !m_wasCameraMoving) {
        gpuDecimationDirty = true;
    }
    m_wasCameraMoving = cameraMoving.load();

    // Consume a pending mesh handoff from the GUI thread (shared_ptr; no copy).
    // Uploading here keeps all GL work inside render() with the context current.
    {
        std::lock_guard<std::mutex> lock(meshQueueMutex);
        if (m_pendingMesh) {
            uploadMesh(m_pendingMesh);
            m_pendingMesh.reset();
            m_qualityOverlay.markDirty();
            m_streamlines.requestRecompute();
        }
    }

    if (vectorGlyphDirty.exchange(false)) {
        if (m_lastUploadedMesh) vectorGlyph.rebuild(*m_lastUploadedMesh, m_state.vectorStride, m_state.vectorField, m_state.vectorMagTransform);
    }

    if (m_streamlines.streamlineDirty.exchange(false)) {
        m_streamlines.requestRecompute();
    }

    // Off-thread streamline: debounce, launch compute, consume results
    m_streamlines.dispatchCompute(m_state, m_lastUploadedMesh, streamlineSet);
    m_streamlines.consumeResult(m_state, streamlineSet);

    if (m_streamlines.particleCountDirty.exchange(false)) {
        streamlineSet.initParticles(m_state.particleCount);
        particleVertexCount = 0;
    }

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDisable(GL_CULL_FACE);

    const float clearAlpha = m_state.screenshotTransparent ? 0.0f : 1.0f;
    glClearColor(m_state.bgColor[0], m_state.bgColor[1], m_state.bgColor[2], clearAlpha);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    int deviceW = static_cast<int>(width * devicePixelRatio);
    int deviceH = static_cast<int>(height * devicePixelRatio);
    glViewport(0, 0, deviceW, deviceH);

    glm::mat4 view = m_state.camera.getViewMatrix();

    double camDist = m_state.camera.distance;
    nearPlane = std::max(0.01, camDist * 0.001);
    farPlane  = camDist + m_state.worldRadius + 250.0;

    // Clip control: switch post-projection NDC to Vulkan-style [0,1] depth so the
    // grid shader can skip manual gl_FragDepth remap and gain 24-bit extra precision.
    glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);

    glm::mat4 proj = m_state.orthographic
        ? [&]() {
            if (m_orthoRefDist <= 0.0) m_orthoRefDist = std::max(m_state.camera.distance, 1e-6);
            float d = static_cast<float>(m_state.camera.distance / m_orthoRefDist);
            float half = static_cast<float>(m_state.worldRadius * d);
            float aspect = (deviceH > 0) ? static_cast<float>(deviceW) / static_cast<float>(deviceH) : 1.0f;
            float n = static_cast<float>(nearPlane);
            float f = static_cast<float>(farPlane);
            const float r = half * aspect;
            const float t = half;
            glm::mat4 p(1.0f);
            p[0][0] = 2.0f / (r + r);
            p[1][1] = 2.0f / (t + t);
            p[2][2] = -1.0f / (f - n);
            p[2][3] = -n / (f - n);
            p[3][3] = 1.0f;
            return p;
          }()
        : [&]() {
            float aspect = (deviceH > 0) ? static_cast<float>(deviceW) / static_cast<float>(deviceH) : 1.0f;
            float n = static_cast<float>(nearPlane);
            float f = static_cast<float>(farPlane);
            float fov = glm::radians(45.0f);
            float tanHalf = std::tan(fov * 0.5f);
            float r = 1.0f / (aspect * tanHalf);
            float t = 1.0f / tanHalf;
            glm::mat4 p(1.0f);
            p[0][0] = r;
            p[1][1] = t;
            p[2][2] = f / (n - f);
            p[2][3] = -1.0f;
            p[3][2] = n * f / (n - f);
            return p;
          }();

    glm::mat4 model = glm::mat4(1.0f);
    glm::mat4 mvp = proj * view * model;

    // Push the colormap choice/reversed snapshot into the GPU LUT manager.
    colormap.setScalarChoice(m_state.colormapChoice);
    colormap.setScalarReversed(m_state.colormapReversed);
    colormap.setVectorChoice(m_state.vectorColormapChoice);
    colormap.setVectorReversed(m_state.vectorColormapReversed);
    colormap.setStreamlineChoice(m_state.streamlineColormapChoice);
    colormap.setStreamlineReversed(m_state.streamlineColormapReversed);
    colormap.update();

    const bool useLod = m_state.useLod;
    if (meshManager.hasMeshes() && shaderProgram != 0) {
        glUseProgram(shaderProgram);

        if (meshUbo == 0) {
            glCreateBuffers(1, &meshUbo);
            glNamedBufferData(meshUbo, sizeof(MeshUBOData), nullptr, GL_DYNAMIC_DRAW);
        }
        if (meshUboIndex != GL_INVALID_INDEX)
            glBindBufferBase(GL_UNIFORM_BUFFER, 0, meshUbo);
        MeshUBOData ubo{};
        ubo.mvp = mvp;
        ubo.model = model;
        ubo.viewPos_ps = glm::vec4(glm::vec3(m_state.camera.position), m_state.pointSize);
        ubo.meshColor_wire = glm::vec4(m_state.meshColor[0], m_state.meshColor[1], m_state.meshColor[2], 0.0f);
        ubo.surfaceColor_sop = glm::vec4(m_state.surfaceColor[0], m_state.surfaceColor[1], m_state.surfaceColor[2], m_state.surfaceOpacity);
        ubo.point_clip = glm::vec4(0.0f, m_state.pointUseScalar ? 1.0f : 0.0f, m_state.pointOpacity, m_state.clipEnabled ? 1.0f : 0.0f);
        glm::vec3 kDir, fDir, b1Dir, b2Dir, hDir;
        computeLightDirections(kDir, fDir, b1Dir, b2Dir, hDir);
        ubo.lightDir = glm::vec4(kDir, 0.0f);
        ubo.lightFill = glm::vec4(fDir, 0.0f);
        ubo.lightBack1 = glm::vec4(b1Dir, 0.0f);
        ubo.lightBack2 = glm::vec4(b2Dir, 0.0f);
        ubo.lightHead = glm::vec4(hDir, 0.0f);
        auto warmTint = [](float w) -> glm::vec3 {
            if (w < 0.5f) return glm::mix(glm::vec3(0.6f,0.7f,1.0f), glm::vec3(1.0f), w/0.5f);
            return glm::mix(glm::vec3(1.0f), glm::vec3(1.0f,0.85f,0.7f), (w-0.5f)/0.5f);
        };
        glm::vec3 tint = warmTint(m_state.lighting.lightWarm);
        ubo.keyColor = glm::vec4(tint, 0.0f);
        ubo.fillColor = glm::vec4(tint * glm::vec3(0.90f, 0.92f, 1.00f), 0.0f);
        ubo.backColor = glm::vec4(tint * glm::vec3(0.95f, 0.95f, 0.98f), 0.0f);
        ubo.headColor = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
        ubo.scalars = glm::vec4(m_state.scalarMin, m_state.scalarMax, (m_state.meshHasScalars && m_state.meshUseScalarColor) ? 1.0f : 0.0f, 0.0f);
        ubo.sliceY = glm::vec4(m_state.sliceHeightX, m_state.sliceHeightY, m_state.sliceHeightZ, 0.0f);
        ubo.sliceEn = glm::vec4(m_state.sliceEnabledX ? 1.0f : 0.0f, m_state.sliceEnabledY ? 1.0f : 0.0f, m_state.sliceEnabledZ ? 1.0f : 0.0f, 0.0f);
        ubo.invert = glm::vec4(m_state.invertX ? 1.0f : 0.0f, m_state.invertY ? 1.0f : 0.0f, m_state.invertZ ? 1.0f : 0.0f, 0.0f);
        ubo.filter = glm::vec4(m_state.filterMin, m_state.filterMax, 0.0f, 0.0f);
        float keyI = m_state.lighting.lightKitEnabled ? m_state.lighting.lightKeyIntensity : 0.0f;
        float kf = std::max(m_state.lighting.lightKF, 0.001f);
        float kh = std::max(m_state.lighting.lightKH, 0.001f);
        float kb = std::max(m_state.lighting.lightKB, 0.001f);
        ubo.intensities = glm::vec4(keyI, m_state.lighting.lightKitEnabled ? keyI / kf : 0.0f, m_state.lighting.lightKitEnabled ? keyI / kb : 0.0f, m_state.lighting.lightKitEnabled ? keyI / kh : 0.0f);
        ubo.material = glm::vec4(m_state.lighting.matAmbient, m_state.lighting.matDiffuse, m_state.lighting.matSpecular, m_state.lighting.matShininess);
        glNamedBufferSubData(meshUbo, 0, sizeof(MeshUBOData), &ubo);

        if (m_state.meshHasScalars && m_state.meshUseScalarColor && colormap.scalarTexture() != 0) {
            glBindTextureUnit(0, colormap.scalarTexture());
            glUniform1i(lutTextureLoc, 0);
        }

        if (useLod && gpuDecimationDirty.load() && meshManager.hasDecimated() && meshManager.hasFullSource()) {
            Mesh newDec;
            if (meshManager.dispatchLodCompute(*meshManager.getFullSource(), newDec)) {
                meshManager.replaceDecimatedMesh(0, newDec);
            } else {
                QString err = QString::fromStdString(meshManager.lastLodError());
                if (err.isEmpty())
                    qWarning() << "[LOD] GPU compute decimation failed, using CPU fallback";
                else
                    qWarning().noquote() << "[LOD] GPU compute decimation failed:\n" + err.trimmed();
            }
            gpuDecimationDirty = false;
        }

        std::vector<std::pair<GLuint, int>> drawList;
        std::vector<int> drawMode;
        std::vector<int> drawVerts;
        meshManager.snapshotDrawList(drawList, m_state.useLod, cameraMoving.load(), drawMode, drawVerts);

        std::vector<std::pair<GLuint, int>> transparentMeshes;

        // --- Pass 1: opaque surfaces ---
        for (size_t di = 0; di < drawList.size(); ++di) {
            glBindVertexArray(drawList[di].first);

            if (m_state.showSurface) {
                const bool opaque = m_state.surfaceOpacity >= 1.0f;

                if (opaque) {
                    const bool cull = m_state.cullMode != 0;
                    if (cull) { glEnable(GL_CULL_FACE); glCullFace(m_state.cullMode == 2 ? GL_FRONT : GL_BACK); }
                    else glDisable(GL_CULL_FACE);
                    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
                    glEnable(GL_POLYGON_OFFSET_FILL);
                    glPolygonOffset(1.0f, 1.0f);
                    glDrawElements(GL_TRIANGLES, drawList[di].second, GL_UNSIGNED_INT, 0);
                    glDisable(GL_POLYGON_OFFSET_FILL);
                    if (cull) glDisable(GL_CULL_FACE);
                } else {
                    transparentMeshes.push_back({drawList[di].first, drawList[di].second});
                }
            }
        }

        // --- Pass 2: batch all transparent meshes through depth peeling ---
        if (!transparentMeshes.empty()) {
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            renderTransparent(view, proj, ubo, meshUbo, transparentMeshes);
        }

        // --- Pass 3: wireframe and points overlays ---
        for (size_t di = 0; di < drawList.size(); ++di) {
            glBindVertexArray(drawList[di].first);

            if (m_state.showWireframe) {
                glLineWidth(m_state.lineWidth);
                glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
                glEnable(GL_POLYGON_OFFSET_LINE);
                glPolygonOffset(-1.0f, -1.0f);
                ubo.meshColor_wire.w = 1.0f;
                glNamedBufferSubData(meshUbo, 0, sizeof(MeshUBOData), &ubo);
                glDrawElements(GL_TRIANGLES, drawList[di].second, GL_UNSIGNED_INT, 0);
                glDisable(GL_POLYGON_OFFSET_LINE);
                glLineWidth(1.0f);
                ubo.meshColor_wire.w = 0.0f;
                glNamedBufferSubData(meshUbo, 0, sizeof(MeshUBOData), &ubo);
            }

            if (m_state.showPoints && drawVerts[di] > 0) {
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
                ubo.point_clip.x = 1.0f;
                glNamedBufferSubData(meshUbo, 0, sizeof(MeshUBOData), &ubo);
                glDrawArrays(GL_POINTS, 0, drawVerts[di]);
                ubo.point_clip.x = 0.0f;
                glNamedBufferSubData(meshUbo, 0, sizeof(MeshUBOData), &ubo);
                glDisable(GL_BLEND);
            }
        }
        glBindVertexArray(0);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

        // ponytail: ParaView-style cell edges — true per-cell boundaries (quads
        // without the triangle diagonal). Drawn from the per-mesh line VBO built
        // at load from globalCellToVertices. Reuses mesh shader + wireframe color.
        if (m_state.showCellEdges && shaderProgram != 0) {
            auto ce = meshManager.getCellEdgeLine();
            if (ce.first != 0 && ce.second > 0) {
                glEnable(GL_DEPTH_TEST);
                // ponytail: surface is pushed back via GL_POLYGON_OFFSET_FILL in
                // the fill pass, so these lines (at true depth) win cleanly.
                // A polygon offset here would be a no-op for GL_LINES.
                glLineWidth(m_state.cellEdgeLineWidth); // ponytail: own thickness, not wireframe's
                glBindVertexArray(ce.first);
                glDrawArrays(GL_LINES, 0, ce.second);
                glBindVertexArray(0);
                glLineWidth(1.0f);
                // depth test left enabled (it was on for the surface pass above);
                // no GL_LINES polygon offset to clear.
            }
        }

        // ponytail: mesh-quality highlight overlay — degenerate faces (red fill)
        // + open edges (amber) + non-manifold edges (magenta), drawn ON TOP of
        // the mesh with a slight depth bias.
        m_qualityOverlay.draw(m_state, glm::value_ptr(view), glm::value_ptr(proj));

        glUseProgram(0);
    }

    m_bbox.draw(m_state, view, proj, meshManager.hasMeshes());

    if (m_state.showVectors && vectorGlyph.instanceCount > 0 && glyphProgram != 0) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glUseProgram(glyphProgram);
        if (glyphUbo == 0 && glyphUboIndex != GL_INVALID_INDEX) {
            glCreateBuffers(1, &glyphUbo);
            glNamedBufferData(glyphUbo, sizeof(GlyphUBOData), nullptr, GL_DYNAMIC_DRAW);
            glBindBufferBase(GL_UNIFORM_BUFFER, 1, glyphUbo);
        }
        glm::vec3 kDir, fDir, b1Dir, b2Dir, hDir;
        computeLightDirections(kDir, fDir, b1Dir, b2Dir, hDir);
        glm::vec3 camPos = glm::vec3(m_state.camera.position);
        GlyphUBOData ubo{};
        ubo.mvp = mvp;
        ubo.scale_magMin_magMax_scaleByMag = glm::vec4(m_state.vectorScale, vectorGlyph.magMin, vectorGlyph.magMax, m_state.vectorScaleByMagnitude ? 1.0f : 0.0f);
        ubo.meshExtent_magTransform_viewPosY_colorR = glm::vec4(vectorGlyph.meshExtent, float(m_state.vectorMagTransform), camPos.y, m_state.vectorColor[0]);
        ubo.lightDir_colorGB = glm::vec4(kDir, m_state.vectorColor[1]);
        ubo.colorB_useColormap = glm::vec4(m_state.vectorColor[2], m_state.vectorUseColormap ? 1.0f : 0.0f, 0.0f, 0.0f);
        glNamedBufferSubData(glyphUbo, 0, sizeof(GlyphUBOData), &ubo);
        if (m_state.vectorUseColormap && colormap.vectorTexture() != 0) {
            glBindTextureUnit(1, colormap.vectorTexture());
            glUniform1i(glyphLutLoc, 1);
        }
        glBindVertexArray(vectorGlyph.vao);
        glDrawElementsInstanced(GL_TRIANGLES, vectorGlyph.glyphIndexCount, GL_UNSIGNED_INT, 0, vectorGlyph.instanceCount);
        glBindVertexArray(0);
        glUseProgram(0);
    }

    // Streamlines + seeds (delegated to StreamlineController)
    {
        glm::vec3 kDir, fDir, b1Dir, b2Dir, hDir;
        computeLightDirections(kDir, fDir, b1Dir, b2Dir, hDir);
        m_streamlines.draw(m_state, streamlineSet, colormap, mvp, m_animationTime, kDir);
    }

    // Particle rendering pass
    if (m_state.showParticles && !streamlineSet.empty() && particleProgram != 0) {
        streamlineSet.updateParticles(static_cast<float>(m_lastFrameDt), m_state.particleSpeed);

        std::vector<float> particleVerts;
        streamlineSet.buildParticleVertices(particleVerts);

        if (!particleVerts.empty()) {
            if (particleVao == 0) {
                glCreateVertexArrays(1, &particleVao);
                glCreateBuffers(1, &particleVbo);
                glEnableVertexArrayAttrib(particleVao, 0);
                glVertexArrayAttribFormat(particleVao, 0, 3, GL_FLOAT, GL_FALSE, 0);
                glVertexArrayAttribBinding(particleVao, 0, 0);
                glEnableVertexArrayAttrib(particleVao, 1);
                glVertexArrayAttribFormat(particleVao, 1, 1, GL_FLOAT, GL_FALSE, 3 * sizeof(float));
                glVertexArrayAttribBinding(particleVao, 1, 0);
                glVertexArrayVertexBuffer(particleVao, 0, particleVbo, 0, 4 * sizeof(float));
            }

            particleVertexCount = static_cast<int>(particleVerts.size() / 4);
            glNamedBufferData(particleVbo, particleVerts.size() * sizeof(float), particleVerts.data(), GL_DYNAMIC_DRAW);

            glUseProgram(particleProgram);
            GLboolean blendWas = glIsEnabled(GL_BLEND);
            GLboolean pointSizeWas = glIsEnabled(GL_PROGRAM_POINT_SIZE);
            GLboolean depthWas = glIsEnabled(GL_DEPTH_TEST);
            GLboolean depthMaskWas;
            glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMaskWas);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glEnable(GL_PROGRAM_POINT_SIZE);
            glEnable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);

            glUniform1f(particlePointSizeLoc, m_state.particleSize);

            if (m_state.streamlineUseColormap && colormap.streamlineTexture() != 0) {
                glBindTextureUnit(1, colormap.streamlineTexture());
                glUniform1i(particleLutLoc, 1);
                glUniform1i(particleUseColormapLoc, 1);
            } else {
                glm::vec4 pc(m_state.streamlineColor[0], m_state.streamlineColor[1], m_state.streamlineColor[2], 1.0f);
                glUniform4fv(particleColorLoc, 1, glm::value_ptr(pc));
                glUniform1i(particleUseColormapLoc, 0);
            }
            glUniform2f(particleMagRangeLoc, streamlineSet.magMin, streamlineSet.magMax);

            glBindVertexArray(particleVao);
            glDrawArrays(GL_POINTS, 0, particleVertexCount);
            glBindVertexArray(0);

            if (!blendWas) glDisable(GL_BLEND);
            if (!pointSizeWas) glDisable(GL_PROGRAM_POINT_SIZE);
            if (depthWas) glEnable(GL_DEPTH_TEST);
            glDepthMask(depthMaskWas);
            glUseProgram(0);
        }
    }

    if (!m_state.screenshotTransparent) m_grid.draw(m_state, view, proj);

    if (m_state.showGizmo) drawGizmo();

    drawColorbarLegends(deviceW, deviceH);

    glBindVertexArray(0);
}

