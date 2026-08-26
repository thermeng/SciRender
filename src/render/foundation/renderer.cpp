#include "render/foundation/renderer.h"
#include "core/FieldResolver.h"
#include "render/foundation/shader_utils.h"
#include "render/foundation/NumberFormat.h"
#include "render/foundation/render_config.h"
#include "render/streamlines/StreamlineSet.h"
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
    m_lastOrthoRadius = m_state.worldRadius;
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
        m_lightMarkers.shutdown();
        colorbarOverlay.shutdown();
        m_bbox.shutdown();
        m_qualityOverlay.shutdown();
        m_streamlines.shutdown();
        m_depthPeel.shutdown();
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

    m_bbox.init(sources);
    m_qualityOverlay.init(sources);
    m_streamlines.init(sources);
    m_depthPeel.init(sources);
}

void Renderer::initGizmo() {
    gizmo.init();
    m_lightMarkers.init();
    colorbarOverlay.init();
}

void Renderer::renderTransparent(const glm::mat4& view, const glm::mat4& proj,
                                   GLuint meshUbo,
                                   const std::vector<std::pair<GLuint, int>>& transparentMeshes) {
    (void)view; (void)proj;
    m_depthPeel.renderTransparent(effectiveDeviceW(), effectiveDeviceH(), m_state, meshUbo, colormap, transparentMeshes);
}

void Renderer::uploadMesh(std::shared_ptr<const RenderMesh> renderMesh) {
    if (!renderMesh) return;
    // Phase 1.3: gate vector/ volume rebuild on content hash to skip static fields
    bool vectorChanged = true;
    bool volumeChanged = true;
    if (m_lastUploadedMesh) {
        if (renderMesh->vectorHash != 0 && m_lastUploadedMesh->vectorHash == renderMesh->vectorHash
            && m_lastUploadedMesh->pointVectorCount == renderMesh->pointVectorCount
            && m_lastUploadedMesh->cellVectorCount == renderMesh->cellVectorCount) {
            vectorChanged = false;
        }
        if (renderMesh->scalarHash != 0 && m_lastUploadedMesh->scalarHash == renderMesh->scalarHash
            && renderMesh->gridDimX == m_lastUploadedMesh->gridDimX
            && renderMesh->gridDimY == m_lastUploadedMesh->gridDimY
            && renderMesh->gridDimZ == m_lastUploadedMesh->gridDimZ) {
            volumeChanged = false;
        }
    }
    meshManager.upload(renderMesh);
    m_lastUploadedMesh = renderMesh;
    if (vectorChanged) {
        vectorGlyph.rebuild(*renderMesh, m_state.vectorStride, m_state.vectorField, m_state.vectorMagTransform, m_state.vectorPlacement);
    }
    // Phase 1.2: streamline sync rebuild removed — rely solely on async StreamlineController
    // (requestRecompute is already debounced at 0.15s and coalesces rapid animation frames)
    // streamlineSet.rebuild(...) removed to halve CPU per frame

    if (renderMesh->gridDimX > 0 && renderMesh->gridDimY > 0 && renderMesh->gridDimZ > 0 && !renderMesh->scalars.empty()) {
        if (!volumeChanged) {
            // Scalar content unchanged, skip 64MB 3D texture upload
        } else {
            glm::vec3 boxMin(renderMesh->bounds.minX, renderMesh->bounds.minY, renderMesh->bounds.minZ);
            glm::vec3 boxMax(renderMesh->bounds.maxX, renderMesh->bounds.maxY, renderMesh->bounds.maxZ);
            qDebug() << "[DEBUG MESH] bounds:"
                     << "min:" << static_cast<float>(renderMesh->bounds.minX) << static_cast<float>(renderMesh->bounds.minY) << static_cast<float>(renderMesh->bounds.minZ)
                     << "max:" << static_cast<float>(renderMesh->bounds.maxX) << static_cast<float>(renderMesh->bounds.maxY) << static_cast<float>(renderMesh->bounds.maxZ)
                     << "gridDim:" << renderMesh->gridDimX << renderMesh->gridDimY << renderMesh->gridDimZ;
            m_volume.uploadVolume(m_state, renderMesh->scalars, renderMesh->gridDimX, renderMesh->gridDimY, renderMesh->gridDimZ, boxMin, boxMax);
        }
    } else {
        m_volume.clearVolume();
    }
    // Ensure streamlines are recomputed for new mesh via async path
    if (m_state.showStreamlines) {
        m_streamlines.requestRecompute();
    }
}

void Renderer::setPendingMesh(std::shared_ptr<const RenderMesh> renderMesh) {
    std::lock_guard<std::mutex> lock(meshQueueMutex);
    m_pendingMesh = std::move(renderMesh);
    meshManager.meshChanged = true;
}

void Renderer::setPendingIsosurface(std::shared_ptr<const RenderMesh> isoMesh) {
    std::lock_guard<std::mutex> lock(meshQueueMutex);
    m_pendingIsosurface = std::move(isoMesh);
    isosurfaceDirty = true;
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

void Renderer::setState(const RenderRenderState& state) {
    bool colormapChanged = m_state.colormapChoice != state.colormapChoice
        || m_state.colormapReversed != state.colormapReversed
        || m_state.vectorColormapChoice != state.vectorColormapChoice
        || m_state.vectorColormapReversed != state.vectorColormapReversed
        || m_state.streamlineColormapChoice != state.streamlineColormapChoice
        || m_state.streamlineColormapReversed != state.streamlineColormapReversed
        || m_state.volumeColormapChoice != state.volumeColormapChoice
        || m_state.volumeColormapReversed != state.volumeColormapReversed
        || m_state.volumeSliceColormapChoice != state.volumeSliceColormapChoice
        || m_state.volumeSliceColormapReversed != state.volumeSliceColormapReversed;
    bool scalarRangeChanged = m_state.dataScalarMin != state.dataScalarMin
        || m_state.dataScalarMax != state.dataScalarMax
        || m_state.sliceScalarMin != state.sliceScalarMin
        || m_state.sliceScalarMax != state.sliceScalarMax
        || m_state.activeScalarName != state.activeScalarName
        || m_state.colorbarTicks != state.colorbarTicks
        || m_state.colorRangeOverrideEnabled != state.colorRangeOverrideEnabled
        || m_state.colorRangeLo != state.colorRangeLo
        || m_state.colorRangeHi != state.colorRangeHi;
    bool colorbarStyleChanged = m_state.colorbarFontFamily != state.colorbarFontFamily
        || m_state.colorbarFontBold != state.colorbarFontBold
        || m_state.colorbarFontItalic != state.colorbarFontItalic
        || m_state.colorbarFontScale != state.colorbarFontScale
        || m_state.colorbarTickFontScale != state.colorbarTickFontScale
        || m_state.colorbarLengthScale != state.colorbarLengthScale
        || m_state.colorbarThicknessScale != state.colorbarThicknessScale
        || m_state.colorbarPanelEnabled != state.colorbarPanelEnabled
        || m_state.colorbarPanelOpacity != state.colorbarPanelOpacity
        || m_state.colorbarShowAnnotation != state.colorbarShowAnnotation;
    if (colormapChanged || scalarRangeChanged || colorbarStyleChanged) {
        colorbarOverlay.markDirty();
    }
    double oldRadius = m_lastOrthoRadius;
    m_state = state;
    if (m_state.orthographic && oldRadius != m_state.worldRadius) {
        m_orthoRefDist = m_state.camera.distance > 0.0 ? m_state.camera.distance : 1.0;
        m_lastOrthoRadius = m_state.worldRadius;
    }
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
    m_lastIsosurfaceMesh.reset();
    m_pendingIsosurface.reset();
    isosurfaceDirty.store(false);
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
        m_depthPeel.shutdown();

        // Shutdown subsystems — each deletes its own GL handles and zeros them.
        m_bbox.shutdown();
        m_qualityOverlay.shutdown();
        m_streamlines.shutdown();
        gizmo.shutdown();
        m_lightMarkers.shutdown();
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
    m_depthPeel.reinitForNewContext();

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
                            m_state.vectorField, m_state.vectorMagTransform,
                            m_state.vectorPlacement);
        streamlineSet.rebuild(*m_lastUploadedMesh, m_state.streamlineSeedCount, m_state.streamlineStepSize, m_state.streamlineMaxSteps, m_state.streamlineVectorField, m_state.seedMode, m_state.streamlineDirection, m_state.seedPlanePos, m_state.seedJitter, m_state.seedPlaneCountU, m_state.seedPlaneCountV, m_state.showStreamlineArrows, m_state.streamlineArrowSpacingFrac, m_state.streamlineArrowSize, m_state.streamlineRibbonWidth, m_state.streamlineTaperFactor);
        streamlineSet.initParticles(m_state.particleCount);
        m_qualityOverlay.markDirty();

        if (m_lastUploadedMesh->hasVolumeData()) {
            glm::vec3 boxMin(m_lastUploadedMesh->bounds.minX, m_lastUploadedMesh->bounds.minY, m_lastUploadedMesh->bounds.minZ);
            glm::vec3 boxMax(m_lastUploadedMesh->bounds.maxX, m_lastUploadedMesh->bounds.maxY, m_lastUploadedMesh->bounds.maxZ);
            m_volume.uploadVolume(m_state, m_lastUploadedMesh->scalars, m_lastUploadedMesh->gridDimX, m_lastUploadedMesh->gridDimY, m_lastUploadedMesh->gridDimZ, boxMin, boxMax);
        }

        // Isosurface survives GL-context resets: re-upload the last extracted
        // surface (no recompute needed -- the CPU mesh is unchanged).
        if (m_state.showIsosurface && m_lastIsosurfaceMesh) {
            meshManager.uploadIsosurface(m_lastIsosurfaceMesh);
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

void Renderer::drawGizmo(int deviceW, int deviceH) {
    // Save engine state we mutate; restored automatically on scope exit.
    GLStateGuard guard;
    glDisable(GL_DEPTH_TEST);
    const float dpr = static_cast<float>(devicePixelRatio);
    const int corner = m_state.gizmoCorner;
    const int foot = Gizmo::footprintFor(m_state.gizmoSizeChoice);
    gizmo.draw(m_state.camera.getViewMatrix(), dpr, deviceW, deviceH, corner, foot,
               m_gizmoHoverAxis.load(std::memory_order_relaxed));
    if (m_state.lighting.lightKitEnabled && m_state.lighting.showLightMarkers) {
        glm::vec3 kitDirs[5] = {
            LightingModel::kitDirection(m_state.lighting.lightKeyAzimuth,  m_state.lighting.lightKeyElevation),
            LightingModel::kitDirection(m_state.lighting.lightFillAzimuth, m_state.lighting.lightFillElevation),
            LightingModel::kitDirection(m_state.lighting.lightBackAzimuth,  m_state.lighting.lightBackElevation),
            LightingModel::kitDirection(m_state.lighting.lightBackAzimuth + 180.0f, -m_state.lighting.lightBackElevation),
            LightingModel::kitDirection(m_state.lighting.lightHeadAzimuth,  m_state.lighting.lightHeadElevation),
        };
        glm::vec3 tint = LightingModel::warmTint(m_state.lighting.lightWarm);
        glm::vec3 cols[5] = { tint, tint * 0.9f, tint * 0.95f, tint * 0.95f, glm::vec3(1.0f, 1.0f, 1.0f) };
        m_lightMarkers.draw(kitDirs, cols, dpr, deviceW, deviceH, corner, foot);
    }
}

std::string Renderer::vectorGlyphTitle(const RenderRenderState& state, const RenderMesh* mesh) {
    if (!mesh) return state.vectorField;
    if (!state.vectorField.empty()) {
        auto f = FieldResolver::resolveVector(*mesh, state.vectorField, state.vectorPlacement);
        if (f.data) return state.vectorField;
    }
    return FieldResolver::resolveVectorName(*mesh, state.vectorField, state.vectorPlacement);
}

namespace {

// One persisted identity per bar TYPE (subtitle), so positions/orientations
// survive field switches. Subtitles are unique across the five bars.
QString colorbarPosKey(const ColorbarData& d) {
    return QString("colorbarPos/%1").arg(d.subtitle);
}

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

    m_colorbarBars.clear();
    const int tickCount = m_state.colorbarTicks;

    auto loadBarPosition = [this](ColorbarData& d) {
        const QString key = colorbarPosKey(d);
        std::lock_guard<std::mutex> lock(m_colorbarCacheMutex);
        auto it = m_colorbarPosCache.find(key);
        if (it == m_colorbarPosCache.end()) {
            QSettings s;
            float fx = 1.0f, fy = 1.0f;
            // Migration chain: per-type key -> per-type+field key (interim
            // build) -> legacy bare-title key.
            const QString byField = QString("colorbarPos/%1/%2").arg(d.subtitle, d.title);
            const QString legacy = QString("colorbarPos_%1").arg(d.title);
            if (s.contains(key + "_x")) {
                fx = s.value(key + "_x", 1.0).toFloat();
                fy = s.value(key + "_y", 1.0).toFloat();
            } else if (s.contains(byField + "_x")) {
                fx = s.value(byField + "_x", 1.0).toFloat();
                fy = s.value(byField + "_y", 1.0).toFloat();
            } else if (s.contains(legacy + "_x")) {
                fx = s.value(legacy + "_x", 1.0).toFloat();
                fy = s.value(legacy + "_y", 1.0).toFloat();
            }
            it = m_colorbarPosCache.emplace(key, std::make_pair(fx, fy)).first;
        }
        d.fracX = it->second.first;
        d.fracY = it->second.second;
    };

    auto loadBarOrientation = [this](ColorbarData& d) {
        const QString key = colorbarPosKey(d);
        std::lock_guard<std::mutex> lock(m_colorbarCacheMutex);
        auto it = m_colorbarOrientCache.find(key);
        if (it == m_colorbarOrientCache.end()) {
            QSettings s;
            const QString byField = QString("colorbarPos/%1/%2").arg(d.subtitle, d.title);
            int o = s.value(key + "_orient", -1).toInt();
            if (o < 0) o = s.value(byField + "_orient",
                                   static_cast<int>(ColorbarStyle::Horizontal)).toInt();
            it = m_colorbarOrientCache.emplace(
                key, o == ColorbarStyle::Vertical ? ColorbarStyle::Vertical
                                                  : ColorbarStyle::Horizontal).first;
        }
        d.style.orientation = static_cast<ColorbarStyle::Orientation>(it->second);
    };

    auto loadBarVisible = [this](ColorbarData& d) {
        const QString key = colorbarPosKey(d);
        std::lock_guard<std::mutex> lock(m_colorbarCacheMutex);
        auto it = m_colorbarVisibleCache.find(key);
        if (it == m_colorbarVisibleCache.end()) {
            QSettings s;
            it = m_colorbarVisibleCache.emplace(key, s.value(key + "_visible", true).toBool()).first;
        }
        d.visible = it->second;
    };

    auto makeTickLabels = [](auto&& valueAt, int count) {
        QStringList labels;
        if (count <= 0) return labels;
        for (int prec = 2;; ++prec) {
            labels.clear();
            for (int i = 0; i < count; ++i)
                labels.append(formatLabel(valueAt(i), prec));
            bool distinct = true;
            for (int i = 1; i < count; ++i) {
                if (labels[i] == labels[i - 1]) { distinct = false; break; }
            }
            if (distinct || prec >= 6) break;
        }
        return labels;
    };

    // Single factory for every bar type: gates stay at the call sites, all
    // shared assembly (labels, stops, persisted overrides) lives here.
    auto makeBar = [&](const char* subtitle, const QString& title,
                       const QVariantList& stops, auto&& valueAt) {
        ColorbarData d;
        d.title = title;
        d.subtitle = subtitle;
        d.stops = stops;
        d.tickLabels = makeTickLabels(std::forward<decltype(valueAt)>(valueAt), tickCount);
        loadBarPosition(d);
        loadBarOrientation(d);
        loadBarVisible(d);
        d.style.fontFamily = m_state.colorbarFontFamily;
        d.style.fontBold = m_state.colorbarFontBold;
        d.style.fontItalic = m_state.colorbarFontItalic;
        d.style.fontScale = m_state.colorbarFontScale;
        d.style.tickFontScale = m_state.colorbarTickFontScale;
        d.style.lengthScale = m_state.colorbarLengthScale;
        d.style.thicknessScale = m_state.colorbarThicknessScale;
        d.style.panelEnabled = m_state.colorbarPanelEnabled;
        d.style.panelOpacity = m_state.colorbarPanelOpacity;
        d.style.showAnnotation = m_state.colorbarShowAnnotation;
        m_colorbarBars.push_back(std::move(d));
    };

    // Scalar bar
    if (m_state.hasMeshLoaded && m_state.meshHasScalars && m_state.meshUseScalarColor && m_state.showScalarColorbar) {
        // Ticks span the effective mapping range: the user's fixed [lo, hi]
        // when the override is on, otherwise the auto-tracked data range.
        const float mapMin = m_state.colorMapMin();
        const float mapMax = m_state.colorMapMax();
        const float range = mapMax - mapMin;
        makeBar("Scalar", QString::fromStdString(m_state.activeScalarName),
                stopsFor(m_state.colormapChoice, m_state.colormapReversed),
                [&](int i) {
                    const float frac = tickCount > 1 ? static_cast<float>(i) / static_cast<float>(tickCount - 1) : 0.0f;
                    return mapMin + range * frac;
                });
    }

    // Vector bar
    if (m_state.showVectors && m_state.vectorUseColormap && m_state.hasMeshLoaded &&
        (m_state.vectorPlacement == 0 ? m_state.meshHasVectors : m_state.meshHasCellVectors)) {
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
        makeBar("Vector", QString::fromStdString(vectorGlyphTitle(m_state, m_lastUploadedMesh.get())),
                stopsFor(m_state.vectorColormapChoice, m_state.vectorColormapReversed),
                [&](int i) {
                    const float frac = tickCount > 1 ? static_cast<float>(i) / static_cast<float>(tickCount - 1) : 0.0f;
                    return invTxMag(tMin + tRange * frac);
                });
    }

    // Streamline bar
    if (m_state.showStreamlines && m_state.streamlineUseColormap && m_state.meshHasVectors && m_state.hasMeshLoaded) {
        const float sMin = streamlineSet.magMin;
        const float sMax = streamlineSet.magMax;
        const float sRange = sMax - sMin;
        makeBar("Streamline", QString::fromStdString(m_state.streamlineVectorField),
                stopsFor(m_state.streamlineColormapChoice, m_state.streamlineColormapReversed),
                [&](int i) {
                    const float frac = tickCount > 1 ? static_cast<float>(i) / static_cast<float>(tickCount - 1) : 0.0f;
                    return sMin + sRange * frac;
                });
    }

    // Volume bar
    if (m_state.showVolume && m_state.volumeUseColormap && m_state.hasMeshLoaded) {
        const float range = m_state.dataScalarMax - m_state.dataScalarMin;
        makeBar("Volume", QString::fromStdString(m_state.activeScalarName),
                stopsFor(m_state.volumeColormapChoice, m_state.volumeColormapReversed),
                [&](int i) {
                    const float frac = tickCount > 1 ? static_cast<float>(i) / static_cast<float>(tickCount - 1) : 0.0f;
                    return m_state.dataScalarMin + range * frac;
                });
    }

    // Slice plane bar (independent colormap, per-slice scalar range)
    if (m_state.showVolumeSlice && m_state.volumeSliceUseColormap && m_state.hasMeshLoaded) {
        const float range = m_state.sliceScalarMax - m_state.sliceScalarMin;
        makeBar("Slice", QString::fromStdString(m_state.activeScalarName),
                stopsFor(m_state.volumeSliceColormapChoice, m_state.volumeSliceColormapReversed),
                [&](int i) {
                    const float frac = tickCount > 1 ? static_cast<float>(i) / static_cast<float>(tickCount - 1) : 0.0f;
                    return m_state.sliceScalarMin + range * frac;
                });
    }

    colorbarOverlay.drawBars(dpr, deviceW, deviceH, m_colorbarBars);
}

int Renderer::colorbarIndexAt(int px, int py, const std::vector<ColorbarData>& bars) const {
    return colorbarOverlay.barIndexAt(devicePixelRatio, effectiveDeviceW(), effectiveDeviceH(), bars, px, py);
}

int Renderer::gizmoAxisAt(int pxDev, int pyDev) const {
    if (!m_state.showGizmo) return -1;
    return Gizmo::hitTestAxis(m_state.camera.getViewMatrix(),
                              static_cast<float>(devicePixelRatio),
                              effectiveDeviceW(), effectiveDeviceH(),
                              static_cast<float>(pxDev), static_cast<float>(pyDev),
                              m_state.gizmoCorner,
                              Gizmo::footprintFor(m_state.gizmoSizeChoice));
}

void Renderer::setColorbarPosition(int index, float fracX, float fracY) {
    if (index >= 0 && index < static_cast<int>(m_colorbarBars.size())) {
        auto& bar = m_colorbarBars[index];
        bar.fracX = fracX;
        bar.fracY = fracY;
        std::lock_guard<std::mutex> lock(m_colorbarCacheMutex);
        m_colorbarPosCache[colorbarPosKey(bar)] = { fracX, fracY };
    }
}

void Renderer::setColorbarOrientation(int index, ColorbarStyle::Orientation orient) {
    if (index >= 0 && index < static_cast<int>(m_colorbarBars.size())) {
        auto& bar = m_colorbarBars[index];
        bar.style.orientation = orient;
        std::lock_guard<std::mutex> lock(m_colorbarCacheMutex);
        m_colorbarOrientCache[colorbarPosKey(bar)] = static_cast<int>(orient);
    }
}

void Renderer::setColorbarVisible(int index, bool vis) {
    if (index >= 0 && index < static_cast<int>(m_colorbarBars.size())) {
        auto& bar = m_colorbarBars[index];
        bar.visible = vis;
        std::lock_guard<std::mutex> lock(m_colorbarCacheMutex);
        m_colorbarVisibleCache[colorbarPosKey(bar)] = vis;
    }
}

void Renderer::commitColorbarPositions() {
    std::map<QString, std::pair<float, float>> posSnapshot;
    std::map<QString, int> orientSnapshot;
    std::map<QString, bool> visibleSnapshot;
    {
        std::lock_guard<std::mutex> lock(m_colorbarCacheMutex);
        posSnapshot = m_colorbarPosCache;
        orientSnapshot = m_colorbarOrientCache;
        visibleSnapshot = m_colorbarVisibleCache;
    }
    QSettings s;
    for (const auto& [key, pos] : posSnapshot) {
        s.setValue(key + "_x", pos.first);
        s.setValue(key + "_y", pos.second);
    }
    for (const auto& [key, orient] : orientSnapshot) {
        s.setValue(key + "_orient", orient);
    }
    for (const auto& [key, vis] : visibleSnapshot) {
        s.setValue(key + "_visible", vis);
    }
}

QRectF Renderer::colorbarBarRect(float dpr, int deviceW, int deviceH, const ColorbarData& bar) const {
    return colorbarOverlay.barRectAt(dpr, deviceW, deviceH, bar);
}

void Renderer::updateSliceScalarRange() {
    const RenderMesh* mesh = m_lastUploadedMesh.get();
    if (!mesh || mesh->gridDimX <= 0 || mesh->gridDimY <= 0 || mesh->gridDimZ <= 0 ||
        mesh->scalars.empty()) {
        m_state.sliceScalarMin = 0.0f;
        m_state.sliceScalarMax = 1.0f;
        return;
    }
    const std::vector<float>& s = mesh->scalars;
    const int dx = mesh->gridDimX, dy = mesh->gridDimY, dz = mesh->gridDimZ;
    const int axis = m_state.volumeSliceAxis;
    const int dim = (axis == 0) ? dx : (axis == 1) ? dy : dz;
    int i0 = static_cast<int>(std::floor(m_state.volumeSlicePos * static_cast<float>(dim - 1)));
    if (i0 < 0) i0 = 0;
    if (i0 >= dim) i0 = dim - 1;
    int i1 = i0 + 1;
    if (i1 >= dim) i1 = dim - 1;

    const int total = static_cast<int>(s.size());
    // Initialize from +/-inf (NOT s[0]): s[0] is one corner of the whole volume and is
    // almost never on the current slice, so seeding mn from it pins the reported
    // minimum to an off-slice value and squashes the colormap to the top of the range.
    float mn = std::numeric_limits<float>::max();
    float mx = std::numeric_limits<float>::lowest();
    // The 3D texture uses GL_LINEAR, so the slice quad trilinearly interpolates between
    // the two grid planes straddling the position. Scanning BOTH planes captures the
    // exact value range the slice can display (otherwise a true [5,10] slice is
    // reported as [7,10] and the low end of the colormap is clipped).
    auto scanPlane = [&](int plane) {
        if (axis == 0) {
            for (int iy = 0; iy < dy; ++iy)
                for (int iz = 0; iz < dz; ++iz) {
                    int i = plane + iy * dx + iz * dx * dy;
                    if (i < total) { float v = s[i]; if (v < mn) mn = v; if (v > mx) mx = v; }
                }
        } else if (axis == 1) {
            for (int ix = 0; ix < dx; ++ix)
                for (int iz = 0; iz < dz; ++iz) {
                    int i = ix + plane * dx + iz * dx * dy;
                    if (i < total) { float v = s[i]; if (v < mn) mn = v; if (v > mx) mx = v; }
                }
        } else {
            for (int ix = 0; ix < dx; ++ix)
                for (int iy = 0; iy < dy; ++iy) {
                    int i = ix + iy * dx + plane * dx * dy;
                    if (i < total) { float v = s[i]; if (v < mn) mn = v; if (v > mx) mx = v; }
                }
        }
    };
    scanPlane(i0);
    if (i1 != i0) scanPlane(i1);
    if (mn > mx) {
        mn = 0.0f;
        mx = 1.0f;
    } else if (mx - mn < 1e-6f) {
        mx = mn + 1.0f;
    }
    m_state.sliceScalarMin = mn;
    m_state.sliceScalarMax = mx;
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
    // A true return means the visible state changed (dispatch or settle) —
    // schedule one more frame so the swap is actually presented.
    if (lodScheduler.tick(m_state, meshManager)) {
        lodSettleDirty = true;
    }

     // Consume a pending mesh handoff from the GUI thread (shared_ptr; no copy).
    // Uploading here keeps all GL work inside render() with the context current.
    {
        std::lock_guard<std::mutex> lock(meshQueueMutex);
        if (m_pendingMesh) {
            uploadMesh(m_pendingMesh);
            m_pendingMesh.reset();
            m_qualityOverlay.markDirty();
            m_streamlines.requestRecompute();
            // Note: uploadMesh() already re-uploads the volume texture, so no
            // separate volume invalidation is needed here.
        }
        // Isosurface handoff (same zero-copy shared_ptr pattern). Uploaded into
        // the dedicated iso mesh slot; no vector/streamline/volume rebuild.
        if (isosurfaceDirty.exchange(false)) {
            m_lastIsosurfaceMesh = m_pendingIsosurface;
            meshManager.uploadIsosurface(m_lastIsosurfaceMesh);
            m_pendingIsosurface.reset();
        }
    }

    if (vectorGlyphDirty.exchange(false)) {
        if (m_lastUploadedMesh) vectorGlyph.rebuild(*m_lastUploadedMesh, m_state.vectorStride, m_state.vectorField, m_state.vectorMagTransform, m_state.vectorPlacement);
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

    int deviceW = effectiveDeviceW();
    int deviceH = effectiveDeviceH();
    glViewport(0, 0, deviceW, deviceH);

    glDepthMask(GL_TRUE);
    glDisable(GL_SCISSOR_TEST);
    // Linear → sRGB conversion for the sRGB-capable display FBO so
    // translucency blending stays in linear space (ParaView-like).
    glEnable(GL_FRAMEBUFFER_SRGB);

    const float clearAlpha = m_state.screenshotTransparent ? 0.0f : 1.0f;
    glClearColor(m_state.bgColor[0], m_state.bgColor[1], m_state.bgColor[2], clearAlpha);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glm::mat4 view = m_state.camera.getViewMatrix();

    double camDist = m_state.camera.distance;
    if (m_state.orthographic) {
        // Orthographic projection: depth range based on world bounds for
        // adequate depth precision in volume ray-marching. The frustum size
        // is already controlled by worldRadius and camera distance via
        // m_orthoRefDist in the ortho matrix setup.
        nearPlane = std::max(0.01, static_cast<double>(m_state.worldRadius * 0.1));
        farPlane  = std::max(100.0, static_cast<double>(m_state.worldRadius * 3.0));
    } else {
        nearPlane = std::max(0.01, camDist * 0.001);
        farPlane  = camDist + m_state.worldRadius + 250.0;
    }

    // Clip control: switch post-projection NDC to Vulkan-style [0,1] depth so
    // shaders can skip manual gl_FragDepth remap and gain 24-bit extra precision.
    glm::mat4 proj = m_state.orthographic
    ? [&]() {
        if (m_orthoRefDist <= 0.0) m_orthoRefDist = std::max(m_state.camera.distance, 1e-6);
        float d = static_cast<float>(m_state.camera.distance / m_orthoRefDist);
        float half = static_cast<float>(m_state.worldRadius * d);
        float aspect = (deviceH > 0) ? static_cast<float>(deviceW) / static_cast<float>(deviceH) : 1.0f;
        m_state.fovY = glm::radians(45.0f);  // orthographic: use reference FOV for footprint scaling
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
        m_state.fovY = fov;  // store for volume ray casting footprint scale
        float tanHalf = std::tan(fov * 0.5f);
        float r = 1.0f / (aspect * tanHalf);
        float t = 1.0f / tanHalf;
        glm::mat4 p(1.0f);
        p[0][0] = r;
        p[1][1] = t;
        p[2][2] = f / (n - f);
        p[2][3] = -1.0f;
        p[3][2] = n * f / (n - f);
        p[3][3] = 0.0f;
        return p;
    }();

    glm::mat4 model = glm::mat4(1.0f);
    glm::mat4 mvp = proj * view * model;

    std::vector<std::pair<GLuint, int>> drawList;
    std::vector<int> drawVerts;
    std::vector<std::pair<GLuint, int>> edgeDrawList;
    if (meshManager.hasMeshes()) {
        meshManager.snapshotDrawList(drawList, m_state.useLod, lodScheduler.isCameraMoving(), drawVerts, &edgeDrawList);
    }

    // Isosurface draw list (independent of showSurface): the extractor emits a
    // plain triangle mesh colored by the colormap LUT, so MeshPass renders it
    // with the SAME shader/UBO as the surface mesh -- no dedicated program.
    std::vector<std::pair<GLuint, int>> isoDrawList;
    std::vector<int> isoDrawVerts;
    static const std::vector<std::pair<GLuint, int>> emptyEdges; // isosurface has no cell edges
    if (m_state.showIsosurface && meshManager.hasIsosurfaceMeshes()) {
        meshManager.snapshotIsosurfaceDrawList(isoDrawList,
                                               m_state.useLod,
                                               lodScheduler.isCameraMoving(),
                                               isoDrawVerts);
    }

    // Push colormap choices into the GPU LUT manager.
    colormap.sync(m_state);

    if (!drawList.empty() && meshPass.hasProgram()) {
        auto result =         meshPass.draw(m_state, view, proj, model, drawList, drawVerts, edgeDrawList, meshManager, colormap);
        if (!result.transparentMeshes.empty()) {
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            renderTransparent(view, proj, meshPass.uboHandle(), result.transparentMeshes);
            meshPass.drawOverlaysAfterTransparent(m_state, view, proj, model,
                                                  drawList, drawVerts, edgeDrawList, meshManager, colormap);
        }
    }

    // Isosurface surface-pass (separate draw list so it can stay visible when
    // showSurface is off). We pass a state copy with showSurface=true purely to
    // pass MeshPass::draw()'s opaque gate -- the VAO list passed in is the
    // isosurface list, so only the iso triangles are drawn, decoupled from the
    // boundary shell. MeshPass reuses the colormap LUT + PBR shader (no new GL).
    if (!isoDrawList.empty() && meshPass.hasProgram()) {
        RenderRenderState isoState = m_state;
        isoState.showSurface = true;
        auto isoResult = meshPass.draw(isoState, view, proj, model,
                                       isoDrawList, isoDrawVerts,
                                       emptyEdges,
                                       meshManager, colormap);
        if (!isoResult.transparentMeshes.empty()) {
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            renderTransparent(view, proj, meshPass.uboHandle(), isoResult.transparentMeshes);
            meshPass.drawOverlaysAfterTransparent(isoState, view, proj, model,
                                                  isoDrawList, isoDrawVerts, emptyEdges,
                                                  meshManager, colormap);
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

        particlePass.draw(m_state, static_cast<float>(m_lastFrameDt), mvp, streamlineSet, colormap);

    float pixelFootprintScale = deviceH > 0 ? std::tan(m_state.fovY * 0.5f) * 2.0f / static_cast<float>(deviceH) : 1.0f;
    m_volume.draw(m_state, view, proj, colormap, pixelFootprintScale);

    // Recompute the slice plane's scalar range from the current data slice so the
    // colormap remaps as the plane moves (only needed when a slice is visible).
    if (m_state.showVolumeSlice) updateSliceScalarRange();

    m_volumeSliceOverlay.draw(m_state, view, proj, colormap, m_volume.volumeTexture(),
                              m_volume.boxMin(), m_volume.boxMax(), m_lastUploadedMesh.get());

    if (m_state.showGizmo) drawGizmo(deviceW, deviceH);

    drawColorbarLegends(deviceW, deviceH);

    glBindVertexArray(0);
}



