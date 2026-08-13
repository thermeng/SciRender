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
#include <memory>
#include <vector>
#include <algorithm>
#include <limits>
#include <QTimer>
#include <QPainter>
#include <QFont>

#include <QOpenGLContext>

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
    meshPass.shutdown();
    glyphPass.shutdown();
    particlePass.shutdown();
    m_volume.shutdown();
    m_volumeSliceOverlay.shutdown();
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
        m_peelProgram.reset();
        m_compositeProgram.reset();
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

    m_clipControlAvailable = GLAD_GL_ARB_clip_control;

    GLint maxSsboBindings = 0;
    glGetIntegerv(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, &maxSsboBindings);
    GLint maxWorkGroupSize[3] = {0, 0, 0};
    glGetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_SIZE, maxWorkGroupSize);
    GLint maxInvocations = 0;
    glGetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, &maxInvocations);

    if (maxSsboBindings < 12) {
        qWarning() << "[GL CAPABILITY] GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS ="
                   << maxSsboBindings << "(below minimum 12; GPU LOD compute may fail)";
    }
    qDebug() << "[GL CAPABILITY] SSBO bindings:" << maxSsboBindings
             << "| Max workgroup size:" << maxWorkGroupSize[0] << "x" << maxWorkGroupSize[1] << "x" << maxWorkGroupSize[2]
             << "| Max invocations:" << maxInvocations;

    qDebug() << "[GL DIAGNOSTIC] VERSION:" << (const char*)glGetString(GL_VERSION)
             << "| RENDERER:" << (const char*)glGetString(GL_RENDERER)
             << "| GLSL:" << (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION)
             << "| CLIP_CONTROL:" << m_clipControlAvailable;
}

void Renderer::initShaders(const ShaderSources& sources) {
    if (sources.meshVert.empty() || sources.meshFrag.empty()) {
        qFatal("Shader compilation aborted due to unreadable file streams.");
        return;
    }

    meshPass.init(sources);
    glyphPass.init(sources);
    particlePass.init(sources);
    m_volume.init(sources);
    m_volumeSliceOverlay.init(sources);

    meshManager.setComputeShaderSources(sources.lodComp, sources.lodOutputComp, sources.lodTrisComp);

    m_grid.init(sources);
    m_bbox.init(sources);
    m_qualityOverlay.init(sources);
    m_streamlines.init(sources);

    // depth peeling shaders for transparent surfaces
    if (!sources.depthPeelVert.empty() && !sources.depthPeelFrag.empty()) {
        m_peelProgram.reset(compileProgram(sources.depthPeelVert.c_str(), sources.depthPeelFrag.c_str(), "DepthPeel"));
        if (m_peelProgram.has()) {
            m_peelPrevDepthLoc = glGetUniformLocation(m_peelProgram, "uPrevDepth");
            m_peelLayerLoc    = glGetUniformLocation(m_peelProgram, "uPeelLayer");
        }
    }
    if (!sources.compositeVert.empty() && !sources.compositeFrag.empty()) {
        m_compositeProgram.reset(compileProgram(sources.compositeVert.c_str(), sources.compositeFrag.c_str(), "Composite"));
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
    if (m_peelFboW == w && m_peelFboH == h && m_peelFbo[0].has()) return;
    destroyPeelFbos();
    m_peelFboW = w; m_peelFboH = h;

    for (int i = 0; i < 2; ++i) {
        glCreateFramebuffers(1, m_peelFbo[i].ptr());
        glCreateTextures(GL_TEXTURE_2D, 1, m_peelColorTex[i].ptr());
        glTextureStorage2D(m_peelColorTex[i], 1, GL_RGBA8, w, h);
        glCreateTextures(GL_TEXTURE_2D, 1, m_peelDepthTex[i].ptr());
        glTextureStorage2D(m_peelDepthTex[i], 1, GL_DEPTH24_STENCIL8, w, h);

        glNamedFramebufferTexture(m_peelFbo[i], GL_COLOR_ATTACHMENT0, m_peelColorTex[i], 0);
        glNamedFramebufferTexture(m_peelFbo[i], GL_DEPTH_STENCIL_ATTACHMENT, m_peelDepthTex[i], 0);
    }

    glCreateTextures(GL_TEXTURE_2D, 1, m_peelDummyDepth.ptr());
    glTextureStorage2D(m_peelDummyDepth, 1, GL_DEPTH24_STENCIL8, 1, 1);
    uint32_t depthOne[2] = { 0xFFFFFFFF, 0 };
    glTextureSubImage2D(m_peelDummyDepth, 0, 0, 0, 1, 1, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, depthOne);

    glCreateTextures(GL_TEXTURE_2D, 1, m_peelMainDepth.ptr());
    glTextureStorage2D(m_peelMainDepth, 1, GL_DEPTH24_STENCIL8, w, h);

    if (!m_peelDummyVao.has()) glCreateVertexArrays(1, m_peelDummyVao.ptr());
}

void Renderer::destroyPeelFbos() {
    for (int i = 0; i < 2; ++i) {
        m_peelFbo[i].reset();
        m_peelColorTex[i].reset();
        m_peelDepthTex[i].reset();
    }
    m_peelDummyDepth.reset();
    m_peelDummyVao.reset();
    m_peelFboW = m_peelFboH = 0;
}

void Renderer::renderTransparent(const glm::mat4& view, const glm::mat4& proj,
                                  GLuint meshUbo,
                                  const std::vector<std::pair<GLuint, int>>& transparentMeshes) {
    if (!m_peelProgram.has() || !m_compositeProgram.has() || transparentMeshes.empty()) return;

    int vpW = static_cast<int>(width * devicePixelRatio);
    int vpH = static_cast<int>(height * devicePixelRatio);
    ensurePeelFbos(vpW, vpH);

    // Save the caller's FBO (QOpenGLWidget's offscreen FBO) so we can composite
    // back to it instead of FBO 0 (the screen default framebuffer).
    GLint prevFbo;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    GLint samples = 0;
    glGetIntegerv(GL_SAMPLES, &samples);
    if (samples > 0) {
        GlFramebuffer tempFbo;
        glCreateFramebuffers(1, tempFbo.ptr());
        glNamedFramebufferTexture(tempFbo, GL_DEPTH_STENCIL_ATTACHMENT, m_peelMainDepth, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, tempFbo);
        glBlitFramebuffer(0, 0, vpW, vpH, 0, 0, vpW, vpH, GL_DEPTH_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
    } else {
        glBindTexture(GL_TEXTURE_2D, m_peelMainDepth);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, vpW, vpH);
    }

    glDisable(GL_CULL_FACE);

    // ---- Layer 0: depth-tested against opaque geometry ----
    glBindFramebuffer(GL_FRAMEBUFFER, m_peelFbo[0]);
    glViewport(0, 0, vpW, vpH);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);

    glUseProgram(m_peelProgram);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, meshUbo);
    glBindTextureUnit(0, m_peelMainDepth);
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
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

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
    streamlineSet.rebuild(*renderMesh, m_state.streamlineSeedCount, m_state.streamlineStepSize, m_state.streamlineMaxSteps, m_state.streamlineVectorField, m_state.seedMode, m_state.streamlineDirection, m_state.seedPlanePos, m_state.seedJitter, m_state.seedPlaneCountU, m_state.seedPlaneCountV, m_state.showStreamlineArrows, m_state.streamlineArrowSpacing, m_state.streamlineArrowSize, m_state.streamlineRibbonWidth, m_state.streamlineTaperFactor);

    if (renderMesh->gridDimX > 0 && renderMesh->gridDimY > 0 && renderMesh->gridDimZ > 0 && !renderMesh->scalars.empty()) {
        glm::vec3 boxMin(renderMesh->bounds.minX, renderMesh->bounds.minY, renderMesh->bounds.minZ);
        glm::vec3 boxMax(renderMesh->bounds.maxX, renderMesh->bounds.maxY, renderMesh->bounds.maxZ);
        m_volume.uploadVolume(m_state, renderMesh->scalars, renderMesh->gridDimX, renderMesh->gridDimY, renderMesh->gridDimZ, boxMin, boxMax);
    } else {
        m_volume.clearVolume();
    }
}

void Renderer::setPendingMesh(std::shared_ptr<const RenderMesh> renderMesh) {
    std::lock_guard<std::mutex> lock(meshQueueMutex);
    m_pendingMesh = std::move(renderMesh);
    meshManager.meshChanged = true;
}

void Renderer::markCameraMoving() {
    lodScheduler.setCameraMoving();
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
    vectorGlyph.shutdown();
    streamlineSet.shutdown();
    m_lastUploadedMesh.reset();
    m_pendingMesh.reset();
    m_state.hasMeshLoaded = false;
    m_qualityOverlay.markDirty();
    m_qualityOverlay.shutdown();
}

void Renderer::reinitForNewContext() {
    // Tear down GL resources from the previous context. In the normal Qt
    // initializeGL path the old context is already gone, so glDelete* here are
    // no-ops; the guard also keeps this correct for shared-context or in-place
    // reinit where the old context may still be current.
    const bool haveCtx = QOpenGLContext::currentContext() != nullptr;
    if (haveCtx) {
        meshPass.shutdown();
        glyphPass.shutdown();
        particlePass.shutdown();
        m_volume.shutdown();
        destroyPeelFbos();
        m_peelProgram.reset();
        m_compositeProgram.reset();

        // Shutdown subsystems — each deletes its own GL handles and zeros them.
        m_grid.shutdown();
        m_bbox.shutdown();
        m_qualityOverlay.shutdown();
        m_streamlines.shutdown();
        gizmo.shutdown();
        colorbarOverlay.shutdown();
        colormap.shutdown();
        vectorGlyph.shutdown();
        streamlineSet.shutdown();
        m_volumeSliceOverlay.shutdown();

        // Drop mesh geometry from the old context (previously only LOD compute
        // was cleared, leaving stale VAO/VBO/EBO/SBO handles and stale
        // hasMeshes()/hasFullSource()/hasDecimated() flags). fullSource_ is
        // reset here because reinitMeshData() re-uploads from m_lastUploadedMesh.
        meshManager.clear();
        meshManager.cleanupLodCompute();
    }

    // Always land handle slots and integer locs at safe defaults so lazy
    // re-creation in renderFrame()/initShaders() rebuilds them exactly once.
    m_peelProgram.reset(); m_compositeProgram.reset();
    m_peelPrevDepthLoc = -1; m_peelLayerLoc = -1;
    for (auto& fbo : m_peelFbo) fbo.reset();
    for (auto& tex : m_peelColorTex) tex.reset();
    for (auto& tex : m_peelDepthTex) tex.reset();
    m_peelDummyDepth.reset(); m_peelMainDepth.reset(); m_peelDummyVao.reset();
    m_peelFboW = 0; m_peelFboH = 0;

    // Reset transient render state so a recreated context does not inherit a
    // stale animation clock (which would leap forward on the first frame) or
    // stale LOD/dirty flags (which could spuriously dispatch LOD compute or
    // trigger a redundant vector-glyph rebuild).
    m_lastFrameTime = {};
    m_lastFrameDt = 0.0;
    m_animationTime = 0.0;
    lodScheduler.reset();
    vectorGlyphDirty.store(false);
    scalarDirty.store(false);
    m_pendingScalarSrc.reset();
}

void Renderer::reinitMeshData() {
    if (m_lastUploadedMesh) {
        meshManager.upload(m_lastUploadedMesh);
        vectorGlyph.rebuild(*m_lastUploadedMesh, m_state.vectorStride,
                            m_state.vectorField, m_state.vectorMagTransform);
        streamlineSet.rebuild(*m_lastUploadedMesh, m_state.streamlineSeedCount, m_state.streamlineStepSize, m_state.streamlineMaxSteps, m_state.streamlineVectorField, m_state.seedMode, m_state.streamlineDirection, m_state.seedPlanePos, m_state.seedJitter, m_state.seedPlaneCountU, m_state.seedPlaneCountV, m_state.showStreamlineArrows, m_state.streamlineArrowSpacing, m_state.streamlineArrowSize, m_state.streamlineRibbonWidth, m_state.streamlineTaperFactor);
        streamlineSet.initParticles(m_state.particleCount);
        m_qualityOverlay.markDirty();

        if (m_lastUploadedMesh->gridDimX > 0 && m_lastUploadedMesh->gridDimY > 0 && m_lastUploadedMesh->gridDimZ > 0 && !m_lastUploadedMesh->scalars.empty()) {
            glm::vec3 boxMin(m_lastUploadedMesh->bounds.minX, m_lastUploadedMesh->bounds.minY, m_lastUploadedMesh->bounds.minZ);
            glm::vec3 boxMax(m_lastUploadedMesh->bounds.maxX, m_lastUploadedMesh->bounds.maxY, m_lastUploadedMesh->bounds.maxZ);
            m_volume.uploadVolume(m_state, m_lastUploadedMesh->scalars, m_lastUploadedMesh->gridDimX, m_lastUploadedMesh->gridDimY, m_lastUploadedMesh->gridDimZ, boxMin, boxMax);
        }
    }
    colormap.update();
}

bool Renderer::consumeScalarDirty() {
    return scalarDirty.exchange(false);
}

bool Renderer::consumeVolumeDirty() {
    return volumeDirty.exchange(false);
}

void Renderer::uploadVolumeFromScalarDirty(const RenderRenderState& state,
    std::shared_ptr<const std::vector<float>> scalars,
    std::shared_ptr<const RenderMesh> mesh) {
    if (!mesh || mesh->gridDimX <= 0 || mesh->gridDimY <= 0 || mesh->gridDimZ <= 0
        || !scalars || scalars->empty()) return;
    glm::vec3 boxMin(mesh->bounds.minX, mesh->bounds.minY, mesh->bounds.minZ);
    glm::vec3 boxMax(mesh->bounds.maxX, mesh->bounds.maxY, mesh->bounds.maxZ);
    m_volume.uploadVolume(state, *scalars, mesh->gridDimX, mesh->gridDimY, mesh->gridDimZ,
                          boxMin, boxMax);
}

void Renderer::updateScalarsOnGPU(std::shared_ptr<const std::vector<float>> scalars) {
    {
        std::lock_guard<std::mutex> lock(meshQueueMutex);
        m_pendingScalarSrc = scalars; // shared_ptr, no data copy
    }
    meshManager.updateScalars(scalars);
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

    // deviceW/deviceH are already the actual render target dimensions from renderFrame
    // (Qt widget size for live rendering, override size for screenshots).

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
        d.subtitle = "Scalar";
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
    if (m_state.showVectors && m_state.vectorUseColormap && m_state.meshHasVectors && m_state.hasMeshLoaded) {
        ColorbarData d;
        d.visible = true;
        d.title = QString::fromStdString(m_state.vectorField) + QChar(0x27A1);
        d.subtitle = "Vector";
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
    if (m_state.showStreamlines && m_state.streamlineUseColormap && m_state.meshHasVectors && m_state.hasMeshLoaded) {
        ColorbarData d;
        d.visible = true;
        d.title = QString::fromStdString(m_state.streamlineVectorField) + QChar(0x27A1);
        d.subtitle = "Streamline";
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

    // Volume bar
    if (m_state.showVolume && m_state.volumeUseColormap && m_state.hasMeshLoaded) {
        ColorbarData d;
        d.visible = true;
        d.title = QString::fromStdString(m_state.activeScalarName);
        d.subtitle = "Volume";
        d.stops = stopsFor(m_state.volumeColormapChoice, m_state.volumeColormapReversed);
        const float range = m_state.dataScalarMax - m_state.dataScalarMin;
        for (int i = 0; i < tickCount; ++i) {
            const float frac = tickCount > 1 ? static_cast<float>(i) / static_cast<float>(tickCount - 1) : 0.0f;
            const float v = m_state.dataScalarMin + range * frac;
            d.tickLabels.append(QString::number(v, 'f', 3));
        }
        bars.push_back(d);
    }

    colorbarOverlay.drawBars(dpr, deviceW, deviceH, bars);
}

void Renderer::renderFrame() {
    // Advance animation clock with real elapsed time (drives arrow animation).
    {
        auto now = std::chrono::steady_clock::now();
        if (m_lastFrameTime.time_since_epoch().count() == 0) m_lastFrameTime = now;
        double dt = std::chrono::duration<double>(now - m_lastFrameTime).count();
        m_lastFrameTime = now;
        m_lastFrameDt = dt;
        m_animationTime += dt;
    }

    // LOD debounce, throttle, and compute dispatch (owned by LodScheduler).
    lodScheduler.tick(m_state, meshManager);

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
        particlePass.resetCount();
    }

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDisable(GL_CULL_FACE);

    if (m_clipControlAvailable) {
        glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    }
    m_grid.setZeroToOne(m_clipControlAvailable);

    int deviceW = m_overrideDeviceW > 0 ? m_overrideDeviceW
                 : static_cast<int>(width * devicePixelRatio);
    int deviceH = m_overrideDeviceH > 0 ? m_overrideDeviceH
                 : static_cast<int>(height * devicePixelRatio);
    glViewport(0, 0, deviceW, deviceH);

    glDepthMask(GL_TRUE);
    glDisable(GL_SCISSOR_TEST);

    const float clearAlpha = m_state.screenshotTransparent ? 0.0f : 1.0f;
    glClearColor(m_state.bgColor[0], m_state.bgColor[1], m_state.bgColor[2], clearAlpha);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glm::mat4 view = m_state.camera.getViewMatrix();

    double camDist = m_state.camera.distance;
    nearPlane = std::max(0.01, camDist * 0.001);
    farPlane  = camDist + m_state.worldRadius + 250.0;

    // Clip control: switch post-projection NDC to Vulkan-style [0,1] depth so the
    // grid shader can skip manual gl_FragDepth remap and gain 24-bit extra precision.
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

    // Push colormap choices into the GPU LUT manager.
    colormapSync.apply(m_state, colormap);

    if (meshManager.hasMeshes() && meshPass.hasProgram()) {
        std::vector<std::pair<GLuint, int>> drawList;
        std::vector<int> drawVerts;
        meshManager.snapshotDrawList(drawList, m_state.useLod, lodScheduler.isCameraMoving(), drawVerts);

        auto result =         meshPass.draw(m_state, view, proj, model, drawList, drawVerts, meshManager, colormap);
        if (!result.transparentMeshes.empty()) {
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            renderTransparent(view, proj, meshPass.uboHandle(), result.transparentMeshes);
        }
    }

    m_bbox.draw(m_state, view, proj, meshManager.hasMeshes());

    m_qualityOverlay.draw(m_state, glm::value_ptr(view), glm::value_ptr(proj));

    glyphPass.draw(m_state, view, proj, vectorGlyph, colormap);

    // Streamlines + seeds (delegated to StreamlineController)
    {
        glm::vec3 kDir, fDir, b1Dir, b2Dir, hDir;
        computeLightDirections(kDir, fDir, b1Dir, b2Dir, hDir);
        m_streamlines.draw(m_state, streamlineSet, colormap, mvp, m_animationTime, kDir);
    }

    particlePass.draw(m_state, static_cast<float>(m_lastFrameDt), streamlineSet, colormap);

    if (!m_state.screenshotTransparent) m_grid.draw(m_state, view, proj);

    m_volume.draw(m_state, view, proj, colormap);

    m_volumeSliceOverlay.draw(m_state, view, proj, colormap, m_volume.volumeTexture(),
                              m_volume.boxMin(), m_volume.boxMax(), m_lastUploadedMesh.get());

    if (m_state.showGizmo) drawGizmo();

    drawColorbarLegends(deviceW, deviceH);

    glBindVertexArray(0);
}

