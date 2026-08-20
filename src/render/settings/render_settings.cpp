#include "render/settings/render_settings.h"
#include "render/foundation/render_config.h"
#include "core/Colormaps.h"
#include "core/mesh_loader.h"
#include "core/mesh_quality.h"
#include "core/isosurface.h"

#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <iterator>
#include <cmath>
#include <limits>
#include <QFileInfo>
#include <QSettings>

RenderSettings::RenderSettings(QObject* parent)
    : QObject(parent)
    , m_isoController(m_renderer, this) {
    loadRecentFromSettings();

    connect(&m_meshWatcher, &QFutureWatcher<MeshLoadResult>::finished,
            this, &RenderSettings::onMeshParsed);

    // IsosurfaceController signal -> RenderSettings m_state update + signal.
    connect(&m_isoController, &IsosurfaceController::showIsosurfaceChanged,
            this, [this](bool v) {
                m_state.showIsosurface = v;
                markStateDirty();
                emit viewChanged(ChangeFlag::Display);
            });
    connect(&m_isoController, &IsosurfaceController::needsScalarColor,
            this, [this]() {
                if (!m_state.meshUseScalarColor && m_state.meshHasScalars) {
                    m_state.meshUseScalarColor = true;
                    markStateDirty();
                    emit viewChanged(ChangeFlag::Display);
                }
            });
    connect(&m_isoController, &IsosurfaceController::isovalueChanged,
            this, [this](float v) {
                m_state.isovalue = v;
                emit viewChanged(ChangeFlag::Display);
            });
    connect(&m_isoController, &IsosurfaceController::displayDirty,
            this, [this]() {
                markStateDirty();
                emit viewChanged(ChangeFlag::Display);
            });
}

RenderSettings::~RenderSettings() = default;

void RenderSettings::publishRenderState(::Renderer* scene) {
    if (m_stateDirty) {
        m_stateDirty = false;
        if (scene) {
            // N4: skip copying quality overlay geometry when overlay is hidden
            if (!m_state.showQualityOverlay) {
                auto saved0 = std::exchange(m_state.qualityDegenerateTris, nullptr);
                auto saved1 = std::exchange(m_state.qualityOpenEdges, nullptr);
                auto saved2 = std::exchange(m_state.qualityNonManifoldEdges, nullptr);
                scene->setState(m_state);
                m_state.qualityDegenerateTris = std::move(saved0);
                m_state.qualityOpenEdges = std::move(saved1);
                m_state.qualityNonManifoldEdges = std::move(saved2);
            } else {
                scene->setState(m_state);
            }
        }
    }
}

void RenderSettings::setFpsText(const QString& text) {
    if (fpsText == text) return;
    fpsText = text;
    emit fpsChanged();
}

void RenderSettings::setStatus(const QString& msg) {
    if (statusMessage == msg) return;
    statusMessage = msg;
    emit statusMessageChanged();
}

void RenderSettings::setWireframe(bool enabled) {
    if (m_state.showWireframe == enabled) return;
    m_state.showWireframe = enabled;
    markStateDirty();
    emit viewChanged(ChangeFlag::Display);
}

void RenderSettings::setUseLod(bool enabled) {
    if (m_state.useLod == enabled) return;
    m_state.useLod = enabled;
    m_renderer.markCameraMoving();
    markStateDirty(); emit viewChanged(ChangeFlag::Camera);
}

void RenderSettings::setMsaaSamples(int n) {
    n = (n <= 0) ? 0 : (n <= 2 ? 2 : 4); // ponytail: only 0/2/4 supported; snap odd/stray QML values
    if (msaaSamples == n) return;
    msaaSamples = n;
    markStateDirty(); emit viewChanged(ChangeFlag::Display);
}

void RenderSettings::setScreenshotResolution(int v) {
    v = qBound(0, v, 4);
    if (m_screenshotResolution == v) return;
    m_screenshotResolution = v;
    markStateDirty(); emit viewChanged(ChangeFlag::Display);
}

void RenderSettings::setScreenshotAASamples(int n) {
    n = (n <= 0) ? 0 : (n <= 2 ? 2 : 4);
    if (m_screenshotAASamples == n) return;
    m_screenshotAASamples = n;
    markStateDirty(); emit viewChanged(ChangeFlag::Display);
}

void RenderSettings::toggleGrid(bool visible) {
    if (m_state.showGrid == visible) return;
    m_state.showGrid = visible;
    markStateDirty();
    emit viewChanged(ChangeFlag::Display);
}

void RenderSettings::setGridAxis(int axis) {
    int clamped = qBound(0, axis, 5);
    if (m_state.gridAxis == clamped) return;
    m_state.gridAxis = clamped;
    markStateDirty();
    emit viewChanged(ChangeFlag::Display);
}

void RenderSettings::setGridShadows(bool enabled) {
    if (m_state.gridShadows == enabled) return;
    m_state.gridShadows = enabled;
    markStateDirty();
    emit viewChanged(ChangeFlag::Display);
}

void RenderSettings::toggleSurface(bool visible) {
    if (m_state.showSurface == visible) return;
    m_state.showSurface = visible;
    markStateDirty();
    emit viewChanged(ChangeFlag::Display);
}

void RenderSettings::snapToOrthoView(int axis) {
    m_state.camera.snapToOrthoView(axis);
    m_renderer.markCameraMoving();
    markStateDirty(); emit viewChanged(ChangeFlag::Camera);
}

void RenderSettings::snapToAxisView(int axis, bool flip) {
    int preset = flip ? (axis * 2 + 1) : (axis * 2);
    m_state.camera.snapToOrthoView(preset);
    m_renderer.markCameraMoving();
    markStateDirty(); emit viewChanged(ChangeFlag::Camera);
}

void RenderSettings::resetCamera() {
    m_state.camera.focalPoint = glm::dvec3(m_state.worldCenterX, m_state.worldCenterY, m_state.worldCenterZ);
    const double dx = m_state.worldMaxX - m_state.worldMinX;
    const double dy = m_state.worldMaxY - m_state.worldMinY;
    const double dz = m_state.worldMaxZ - m_state.worldMinZ;
    const double diag = std::sqrt(dx * dx + dy * dy + dz * dz);
    const double fitRadius = diag * 0.5;
    const double fov = glm::radians(45.0);
    double dist = fitRadius / std::tan(fov * 0.5);
    dist *= RenderConfig::defaults().cameraFitMultiplier;
    m_state.camera.distance = dist < 1.0 ? 1.0 : dist;
    m_state.camera.maxDistance = std::max(1000.0, m_state.camera.distance * 50.0);
    m_state.camera.position = m_state.camera.focalPoint + glm::dvec3(0.0, 0.0, m_state.camera.distance);
    m_state.camera.viewUp = glm::dvec3(0.0, 1.0, 0.0);
    m_state.camera.orthogonalizeViewUp();
    markStateDirty(); emit viewChanged(ChangeFlag::Camera);
}

void RenderSettings::loadRecentFromSettings() {
    QSettings s;
    recentFiles = s.value("recentFiles").toStringList();
    recentFiles.removeAll("");
}

void RenderSettings::saveRecentToSettings() const {
    QSettings s;
    s.setValue("recentFiles", recentFiles);
}

const std::vector<RenderSettings::StateEntry>& RenderSettings::persistenceTable() {
    static const std::vector<StateEntry> table = [] {
        std::vector<StateEntry> t;
        auto add = [&t](const char* key,
                        std::function<QVariant(const RenderSettings&)> get,
                        std::function<void(RenderSettings&, const QVariant&)> set) {
            t.push_back({key, std::move(get), std::move(set)});
        };
        // float members
        add("matSpecular",         [](const RenderSettings& r) { return QVariant(r.m_state.lighting.matSpecular); },         [](RenderSettings& r, const QVariant& v) { r.m_state.lighting.matSpecular = v.toFloat(); });
        add("matRoughness",        [](const RenderSettings& r) { return QVariant(r.m_state.lighting.matRoughness); },        [](RenderSettings& r, const QVariant& v) { r.m_state.lighting.matRoughness = v.toFloat(); });
        add("matMetallic",         [](const RenderSettings& r) { return QVariant(r.m_state.lighting.matMetallic); },         [](RenderSettings& r, const QVariant& v) { r.m_state.lighting.matMetallic = v.toFloat(); });
        add("lightKeyIntensity",   [](const RenderSettings& r) { return QVariant(r.m_state.lighting.lightKeyIntensity); },   [](RenderSettings& r, const QVariant& v) { r.m_state.lighting.lightKeyIntensity = v.toFloat(); });
        add("lightWarm",           [](const RenderSettings& r) { return QVariant(r.m_state.lighting.lightWarm); },           [](RenderSettings& r, const QVariant& v) { r.m_state.lighting.lightWarm = v.toFloat(); });
        add("volumeStepSize",      [](const RenderSettings& r) { return QVariant(r.m_state.volumeStepSize); },               [](RenderSettings& r, const QVariant& v) { r.m_state.volumeStepSize = v.toFloat(); });
        add("volumeOpacity",       [](const RenderSettings& r) { return QVariant(r.m_state.volumeOpacity); },                [](RenderSettings& r, const QVariant& v) { r.m_state.volumeOpacity = v.toFloat(); });
        add("volumeSlicePos",      [](const RenderSettings& r) { return QVariant(r.m_state.volumeSlicePos); },               [](RenderSettings& r, const QVariant& v) { r.m_state.volumeSlicePos = v.toFloat(); });
        add("volumeSliceOpacity",  [](const RenderSettings& r) { return QVariant(r.m_state.volumeSliceOpacity); },           [](RenderSettings& r, const QVariant& v) { r.m_state.volumeSliceOpacity = v.toFloat(); });
        add("vectorScale",         [](const RenderSettings& r) { return QVariant(r.m_state.vectorScale); },                  [](RenderSettings& r, const QVariant& v) { r.m_state.vectorScale = v.toFloat(); });
        // bool members
        add("lightKitEnabled",     [](const RenderSettings& r) { return QVariant(r.m_state.lighting.lightKitEnabled); },     [](RenderSettings& r, const QVariant& v) { r.m_state.lighting.lightKitEnabled = v.toBool(); });
        add("colormapReversed",    [](const RenderSettings& r) { return QVariant(r.m_state.colormapReversed); },              [](RenderSettings& r, const QVariant& v) { r.m_state.colormapReversed = v.toBool(); });
        add("meshUseScalarColor",  [](const RenderSettings& r) { return QVariant(r.m_state.meshUseScalarColor); },            [](RenderSettings& r, const QVariant& v) { r.m_state.meshUseScalarColor = v.toBool(); });
        add("showVolume",          [](const RenderSettings& r) { return QVariant(r.m_state.showVolume); },                    [](RenderSettings& r, const QVariant& v) { r.m_state.showVolume = v.toBool(); });
        add("volumeUseColormap",   [](const RenderSettings& r) { return QVariant(r.m_state.volumeUseColormap); },             [](RenderSettings& r, const QVariant& v) { r.m_state.volumeUseColormap = v.toBool(); });
        add("volumeColormapReversed", [](const RenderSettings& r) { return QVariant(r.m_state.volumeColormapReversed); },     [](RenderSettings& r, const QVariant& v) { r.m_state.volumeColormapReversed = v.toBool(); });
        add("showVolumeSlice",     [](const RenderSettings& r) { return QVariant(r.m_state.showVolumeSlice); },               [](RenderSettings& r, const QVariant& v) { r.m_state.showVolumeSlice = v.toBool(); });
        add("volumeSliceUseColormap", [](const RenderSettings& r) { return QVariant(r.m_state.volumeSliceUseColormap); },     [](RenderSettings& r, const QVariant& v) { r.m_state.volumeSliceUseColormap = v.toBool(); });
        add("volumeSliceColormapReversed", [](const RenderSettings& r) { return QVariant(r.m_state.volumeSliceColormapReversed); }, [](RenderSettings& r, const QVariant& v) { r.m_state.volumeSliceColormapReversed = v.toBool(); });
        add("vectorScaleByMagnitude", [](const RenderSettings& r) { return QVariant(r.m_state.vectorScaleByMagnitude); },     [](RenderSettings& r, const QVariant& v) { r.m_state.vectorScaleByMagnitude = v.toBool(); });
        add("flatShading",         [](const RenderSettings& r) { return QVariant(r.m_state.flatShading); },                   [](RenderSettings& r, const QVariant& v) { r.m_state.flatShading = v.toBool(); });
        add("quickBarCollapsed",   [](const RenderSettings& r) { return QVariant(r.quickBarCollapsed); },                     [](RenderSettings& r, const QVariant& v) { r.quickBarCollapsed = v.toBool(); });
        // int members
        add("colormapChoice",      [](const RenderSettings& r) { return QVariant(r.m_state.colormapChoice); },                [](RenderSettings& r, const QVariant& v) { r.m_state.colormapChoice = v.toInt(); });
        add("volumeColormapChoice",[](const RenderSettings& r) { return QVariant(r.m_state.volumeColormapChoice); },          [](RenderSettings& r, const QVariant& v) { r.m_state.volumeColormapChoice = v.toInt(); });
        add("volumeSliceAxis",     [](const RenderSettings& r) { return QVariant(r.m_state.volumeSliceAxis); },               [](RenderSettings& r, const QVariant& v) { r.m_state.volumeSliceAxis = v.toInt(); });
        add("volumeSliceColormapChoice", [](const RenderSettings& r) { return QVariant(r.m_state.volumeSliceColormapChoice); }, [](RenderSettings& r, const QVariant& v) { r.m_state.volumeSliceColormapChoice = v.toInt(); });
        add("vectorPlacement",     [](const RenderSettings& r) { return QVariant(r.m_state.vectorPlacement); },               [](RenderSettings& r, const QVariant& v) { r.m_state.vectorPlacement = v.toInt(); });
        return t;
    }();
    return table;
}

void RenderSettings::saveStateToSettings() const {
    QSettings s;
    s.beginGroup("state");
    // Camera (3-vector) and background color are stored as QVariantLists.
    s.setValue("camDistance", m_state.camera.distance);
    s.setValue("camFocal", QVariantList{ m_state.camera.focalPoint.x, m_state.camera.focalPoint.y, m_state.camera.focalPoint.z });
    s.setValue("camPos", QVariantList{ m_state.camera.position.x, m_state.camera.position.y, m_state.camera.position.z });
    s.setValue("camUp", QVariantList{ m_state.camera.viewUp.x, m_state.camera.viewUp.y, m_state.camera.viewUp.z });
    s.setValue("bgColor", QVariantList{ m_state.bgColor[0], m_state.bgColor[1], m_state.bgColor[2] });
    // Scalar settings, table-driven.
    for (const auto& e : persistenceTable()) s.setValue(e.key, e.get(*this));
    // GUI members with bespoke storage.
    s.setValue("msaaSamples", msaaSamples);
    s.setValue("screenshotResolution", m_screenshotResolution);
    s.setValue("screenshotAASamples", m_screenshotAASamples);
    s.endGroup();
}

void RenderSettings::restoreStateFromSettings() {
    QSettings s;
    if (!s.childGroups().contains("state")) return;
    s.beginGroup("state");
    auto readVec3 = [&](const QString& key, glm::dvec3& out) {
        QVariantList v = s.value(key).toList();
        if (v.size() == 3) out = glm::dvec3(v[0].toDouble(), v[1].toDouble(), v[2].toDouble());
    };
    auto readFColor = [&](const QString& key, float* c) {
        QVariantList v = s.value(key).toList();
        if (v.size() == 3) { c[0] = v[0].toFloat(); c[1] = v[1].toFloat(); c[2] = v[2].toFloat(); }
    };
    if (s.contains("camDistance")) {
        m_state.camera.distance = s.value("camDistance").toDouble();
        readVec3("camFocal", m_state.camera.focalPoint);
        readVec3("camPos", m_state.camera.position);
        readVec3("camUp", m_state.camera.viewUp);
        m_state.camera.maxDistance = std::max(1000.0, m_state.camera.distance * 50.0);
        m_state.camera.orthogonalizeViewUp();
    }
    readFColor("bgColor", m_state.bgColor);
    // Scalar settings, table-driven. Missing keys keep the constructor default,
    // which already matches the historical restore defaults for the slice
    // colormap fields.
    for (const auto& e : persistenceTable())
        if (s.contains(e.key)) e.set(*this, s.value(e.key));
    // GUI members with bespoke validation.
    if (s.contains("msaaSamples")) {
        const int n = s.value("msaaSamples").toInt();
        msaaSamples = (n <= 0) ? 0 : (n <= 2 ? 2 : 4);
    }
    if (s.contains("screenshotResolution")) {
        m_screenshotResolution = qBound(0, s.value("screenshotResolution").toInt(), 4);
    }
    if (s.contains("screenshotAASamples")) {
        const int n = s.value("screenshotAASamples").toInt();
        m_screenshotAASamples = (n <= 0) ? 0 : (n <= 2 ? 2 : 4);
    }
    s.endGroup();
}

void RenderSettings::loadMesh(const QString& filePath) {
    if (filePath.isEmpty()) return;
    std::string stdPath = filePath.toStdString();
    if (stdPath.rfind("file:///", 0) == 0) stdPath = stdPath.substr(8);
    else if (stdPath.rfind("file://", 0) == 0) stdPath = stdPath.substr(7);

    // Parse OFF the GUI thread so heavy VTK/STL files never block the UI.
    // The parse produces a RenderMesh that we wrap in an immutable shared_ptr;
    // no per-vertex array is copied on the GUI thread. The QFutureWatcher's
    // finished() runs on the GUI thread and does the bookkeeping below.
    if (m_meshWatcher.isRunning()) {
        m_meshWatcher.cancel();
    }
    const uint64_t token = ++m_loadToken;
    auto taskToken = std::make_shared<std::atomic<uint64_t>>(token);
    m_taskToken = taskToken;
    // Chain load → analyze so the main thread can show progressive status.
    // Each phase runs off-thread; the continuation starts automatically when parsing completes.
    auto loadFuture = QtConcurrent::run([stdPath]() { return loadMeshFile(stdPath); });
    auto resultFuture = loadFuture.then([taskToken, token](RenderMesh loaded) -> MeshLoadResult {
        taskToken->store(token); // mark this task's generation on completion
        MeshLoadResult res;
        res.mesh = std::make_shared<const RenderMesh>(std::move(loaded));
        res.quality = analyzeMeshQuality(*res.mesh);
        return res;
    });
    m_loadingPath = stdPath;
    m_meshWatcher.setFuture(resultFuture);
    setStatus(QString("Loading %1…").arg(QString::fromStdString(stdPath)));
}

void RenderSettings::onMeshParsed() {
    // Ignore results from a cancelled or superseded load. A newer loadMesh()
    // increments m_loadToken; if this task's generation no longer matches, drop it
    // so it cannot clobber state or report a false "could not load" error.
    if (!m_taskToken || m_taskToken->load() != m_loadToken) return;
    MeshLoadResult res = m_meshWatcher.result();
    std::shared_ptr<const RenderMesh> loaded = res.mesh;
    if (!loaded) return;

    if (loaded->vertices.empty()) {
        setStatus(QString("Could not load: unsupported format or empty file"));
        return;
    }

    // The single heavy CPU payload now lives in m_meshData.loadedMesh (shared, immutable).
    m_meshData.loadedMesh = loaded;

    // Build a LIGHT GUI meta copy: keep only the cheap metadata (names, bounds,
    // active scalar array) so field switches work without the heavy vertex/
    // normal/index/vector payloads. The heavy geometry stays ONLY in m_meshData.loadedMesh
    // (immutable, shared with the renderer); attributes (all scalar/vector field
    // arrays) also stay ONLY in m_meshData.loadedMesh and are read from there on switch.
    m_meshData.guiMeta = *loaded;
    {
        const MeshQuality& mq = res.quality;
        m_meshData.degenerateFaces  = mq.degenerateFaces;
        m_meshData.openEdges        = mq.openEdges;
        m_meshData.nonManifoldEdges = mq.nonManifoldEdges;
        m_meshData.nonManifoldVerts = mq.nonManifoldVerts;
        m_meshData.watertight       = mq.watertight;
        m_state.qualityDegenerateTris  = std::make_shared<const std::vector<float>>(std::move(mq.degenerateTriVerts));
        m_state.qualityOpenEdges        = std::make_shared<const std::vector<float>>(std::move(mq.openEdgeVerts));
        m_state.qualityNonManifoldEdges = std::make_shared<const std::vector<float>>(std::move(mq.nonManifoldEdgeVerts));
    }

    m_meshData.guiMeta.vertices.clear();   m_meshData.guiMeta.vertices.shrink_to_fit();
    m_meshData.guiMeta.normals.clear();    m_meshData.guiMeta.normals.shrink_to_fit();
    m_meshData.guiMeta.indices.clear();    m_meshData.guiMeta.indices.shrink_to_fit();
    m_meshData.guiMeta.pointVectorsData.clear(); m_meshData.guiMeta.pointVectorsData.shrink_to_fit();
    m_meshData.guiMeta.attributes.reset();

    m_state.worldMinX = loaded->bounds.minX; m_state.worldMaxX = loaded->bounds.maxX;
    m_state.worldMinY = loaded->bounds.minY; m_state.worldMaxY = loaded->bounds.maxY;
    m_state.worldMinZ = loaded->bounds.minZ; m_state.worldMaxZ = loaded->bounds.maxZ;
    m_state.worldCenterX = loaded->bounds.centerX;
    m_state.worldCenterY = loaded->bounds.centerY;
    m_state.worldCenterZ = loaded->bounds.centerZ;
    m_state.worldRadius  = loaded->bounds.worldRadius;

    QFileInfo fileInfo(QString::fromStdString(m_loadingPath));
    m_meshData.fileName = fileInfo.fileName().toStdString();
    m_meshData.triangleCount = static_cast<int>(loaded->indices.size() / 3);
    m_meshData.pointCount = loaded->sourcePointCount >= 0
        ? loaded->sourcePointCount
        : static_cast<int>(loaded->vertices.size() / 3);
    m_meshData.datasetType = loaded->datasetType;
    m_meshData.meshFormat = loaded->fileFormat;
    m_state.hasMeshLoaded = true;

    // Vector field availability gate for colorbars / UI.
    m_state.meshHasVectors = !loaded->pointVectorsData.empty();
    m_state.meshHasCellVectors = !loaded->cellVectorsData.empty();

    // Auto-adjust vectorPlacement to match the mesh's vector data:
    // - Cell-center placement requires cell vectors; if absent, fall back to vertex.
    // - If only cell vectors exist (no point vectors), use cell-center so glyphs render.
    if (!m_state.meshHasCellVectors && m_state.vectorPlacement == 1) {
        m_state.vectorPlacement = 0;
    } else if (!m_state.meshHasVectors && m_state.meshHasCellVectors) {
        m_state.vectorPlacement = 1;
    }

    if (!m_state.meshHasVectors) {
        m_state.showStreamlines = false;
    }

    // Reset per-mesh vector state.
    m_state.showVectors = false;
    m_state.showVolume = false;
    m_state.vectorUseColormap = false;
    m_state.clipEnabled = false;
    m_state.sliceEnabledX = m_state.sliceEnabledY = m_state.sliceEnabledZ = false;
    if (!loaded->pointVectorsData.empty()) {
        m_meshData.guiMeta.vectorName = loaded->availableVectorNames.front();
        m_state.vectorField = loaded->availableVectorNames.front();
        m_state.streamlineVectorField = loaded->availableVectorNames.front();
    } else if (!loaded->cellVectorsData.empty()) {
        m_meshData.guiMeta.vectorName = loaded->cellVectorName;
        m_state.vectorField = loaded->cellVectorName;
        m_state.streamlineVectorField.clear();
    } else {
        m_meshData.guiMeta.vectorName.clear();
        m_state.vectorField.clear();
        m_state.streamlineVectorField.clear();
    }
    markStateDirty(); emit meshDataUpdated();

    if (!loaded->scalars.empty()) {
        m_state.meshHasScalars = true;
        m_state.meshUseScalarColor = false;          // ponytail: don't color on load
        m_state.showScalarColorbar = true;
        m_state.activeScalarName = loaded->scalarName;
        recomputeScalarRange();
        setFilterMin(m_state.dataScalarMin); setFilterMax(m_state.dataScalarMax);
     } else {
        m_state.meshHasScalars = false;
        m_state.meshUseScalarColor = false;
        m_state.showScalarColorbar = false;
        // For structured grids, surface extraction clears mesh.scalars (vertex
        // count mismatch), but the original per-node scalars survive in
        // attributes->pointScalars. Use them to set the isosurface slider range.
        m_state.dataScalarMin = 0.0f;
        m_state.dataScalarMax = 1.0f;
        if (loaded->attributes && !loaded->attributes->pointScalars.empty()) {
            const auto& ps = loaded->attributes->pointScalars;
            auto it = ps.find(loaded->scalarName);
            if (it == ps.end()) it = ps.begin();
            if (it != ps.end() && !it->second.empty()) {
                float mn = std::numeric_limits<float>::max();
                float mx = -std::numeric_limits<float>::max();
                for (float v : it->second) {
                    if (!std::isfinite(v)) continue;
                    if (v < mn) mn = v;
                    if (v > mx) mx = v;
                }
                if (mn > mx) { mn = 0.0f; mx = 1.0f; }
                m_state.dataScalarMin = mn;
                m_state.dataScalarMax = mx;
                m_state.scalarMin = mn;
                m_state.scalarMax = mx;
                if (mx - mn < 1e-6f) mx = mn + 1.0f;
            }
        }
    }

    // Isosurface: a fresh mesh starts with the surface off and the threshold
    // centered on the new data range. (The ISO mesh is cleared on the render
    // thread by the null handoff in reset().)
    m_isoController.reset(m_state.dataScalarMin, m_state.dataScalarMax);
    m_isoController.setCurrentMesh(m_meshData.loadedMesh, m_state.activeScalarName);

    resetCamera();

    // Hand the immutable payload to the render thread (shared_ptr, no copy).
    m_renderer.setPendingMesh(m_meshData.loadedMesh);

    {
        QString absPath = QFileInfo(QString::fromStdString(m_loadingPath)).absoluteFilePath();
        recentFiles.removeAll(absPath);
        recentFiles.prepend(absPath);
        while (recentFiles.size() > 8) recentFiles.removeLast();
        saveRecentToSettings();
    }

    emit meshLoadStateChanged();
    markStateDirty(); emit meshDataUpdated();
    setStatus("");
    saveStateToSettings();
}

void RenderSettings::openRecent(const QString& filePath) {
    if (filePath.isEmpty()) return;
    loadMesh(filePath);
}

void RenderSettings::setQuickBarCollapsed(bool collapsed) {
    if (quickBarCollapsed == collapsed) return;
    quickBarCollapsed = collapsed;
    emit quickBarCollapsedChanged();
}

void RenderSettings::clearMeshes() {
    m_renderer.clearGpuMeshes();
    m_meshData.loadedMesh.reset();
    m_meshData = MeshData{};
    m_state.streamlineVectorField.clear();
    m_state.hasMeshLoaded = false;
    m_state.meshHasScalars = false;
    m_state.meshHasVectors = false;
    m_state.meshHasCellVectors = false;
    m_state.showVectors = false;
    m_state.showStreamlines = false;
    m_state.showVolume = false;
    m_isoController.clear();
    markStateDirty();
    m_state.qualityDegenerateTris.reset(); m_state.qualityOpenEdges.reset(); m_state.qualityNonManifoldEdges.reset();
    emit meshLoadStateChanged();
}

void RenderSettings::requestScreenshot(const QString& path) {
    if (path.isEmpty()) return;
    emit screenshotRequested(path);
}

void RenderSettings::recomputeScalarRange() {
    if (m_meshData.guiMeta.scalars.empty()) return;
    float mn = std::numeric_limits<float>::max();
    float mx = -std::numeric_limits<float>::max();
    for (float v : m_meshData.guiMeta.scalars) {
        if (!std::isfinite(v)) continue;
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    if (mn > mx) { mn = 0.0f; mx = 1.0f; }
    if (mx - mn < 1e-6f) mx = mn + 1.0f;
    m_state.dataScalarMin = mn; m_state.dataScalarMax = mx;
    m_state.scalarMin = mn; m_state.scalarMax = mx;
}

void RenderSettings::setActiveScalarField(const QString& fieldName) {
    if (fieldName.toStdString() == m_state.activeScalarName) return;
    if (!m_meshData.loadedMesh || !m_meshData.loadedMesh->attributes.has_value()) return;
    auto it = m_meshData.loadedMesh->attributes->pointScalars.find(fieldName.toStdString());
    if (it == m_meshData.loadedMesh->attributes->pointScalars.end()) return;

    m_state.activeScalarName = fieldName.toStdString();
    m_meshData.guiMeta.scalarName = m_state.activeScalarName;

    // Build the scalar payload ONCE as a shared_ptr (zero-copy across threads)
    // and reuse it for both the GUI meta copy and the render-thread handoff,
    // so the field is copied a single time rather than twice.
    auto payload = std::make_shared<const std::vector<float>>(it->second);
    m_meshData.guiMeta.scalars = *payload;
    recomputeScalarRange();
    setFilterMin(m_state.dataScalarMin); setFilterMax(m_state.dataScalarMax);
    emit meshLoadStateChanged();

    // Trigger a SCALAR-ONLY re-upload on the render thread (shared_ptr, no copy).
    m_renderer.markScalarDirty(payload);
    if (m_meshData.loadedMesh)
        m_renderer.markVolumeDirty(m_meshData.loadedMesh);
    markStateDirty(); emit meshDataUpdated();
    // The active scalar field IS the contour field, so a field switch that the
    // user has an isosurface enabled for must recontour at the (re-clamped) level.
    if (m_state.showIsosurface && m_state.meshHasScalars) {
        m_isoController.setCurrentField(m_state.activeScalarName);
        m_isoController.recompute();
    }
    // NOTE: do NOT emit meshLoadStateChanged() here — load state
    // (hasMeshLoaded / meshHasScalars) is unchanged; activeScalarName is
    // already covered by meshDataUpdated (render_settings.h:80).
}

void RenderSettings::setActiveVectorField(const QString& fieldName) {
    if (fieldName.isEmpty()) return;
    std::string name = fieldName.toStdString();
    const auto& pointNames = m_meshData.guiMeta.availableVectorNames;
    const auto& cellNames = m_meshData.guiMeta.availableCellVectorNames;
    bool found =
        (!pointNames.empty() && std::find(pointNames.begin(), pointNames.end(), name) != pointNames.end()) ||
        (!cellNames.empty() && std::find(cellNames.begin(), cellNames.end(), name) != cellNames.end());
    if (!found) {
        setStatus(QString("Unknown vector field: %1").arg(fieldName));
        return;
    }
    m_meshData.guiMeta.vectorName = name;
    m_state.vectorField = name;
    m_renderer.markVectorGlyphDirty();
    markStateDirty(); emit meshDataUpdated();
}

void RenderSettings::setStreamlineVectorField(const QString& fieldName) {
    if (fieldName.isEmpty()) return;
    if (m_meshData.guiMeta.availableVectorNames.empty() ||
        std::find(m_meshData.guiMeta.availableVectorNames.begin(),
                  m_meshData.guiMeta.availableVectorNames.end(),
                  fieldName.toStdString()) == m_meshData.guiMeta.availableVectorNames.end()) {
        setStatus(QString("Unknown vector field: %1").arg(fieldName));
        return;
    }
    m_state.streamlineVectorField = fieldName.toStdString();
    m_renderer.markStreamlineDirty();
    markStateDirty(); emit meshDataUpdated();
}

void RenderSettings::setColormapChoice(int choice) {
    if (m_state.colormapChoice == choice) return;
    m_state.colormapChoice = choice;
    markStateDirty(); emit viewChanged(ChangeFlag::Colormap);
}

void RenderSettings::setColormapReversed(bool reversed) {
    if (m_state.colormapReversed == reversed) return;
    m_state.colormapReversed = reversed;
    markStateDirty(); emit viewChanged(ChangeFlag::Colormap);
}

void RenderSettings::setVectorColormapReversed(bool reversed) {
    if (m_state.vectorColormapReversed == reversed) return;
    m_state.vectorColormapReversed = reversed;
    markStateDirty(); emit viewChanged(ChangeFlag::Colormap);
}

void RenderSettings::applyLightingPreset(int preset) {
    m_state.lighting.applyPreset(preset);
    markStateDirty(); emit viewChanged(ChangeFlag::Lighting);
}

void RenderSettings::resetLighting() {
    m_state.lighting.reset();
    markStateDirty(); emit viewChanged(ChangeFlag::Lighting);
}

QStringList RenderSettings::getAvailableScalars() const {
    QStringList list;
    for (const auto& name : m_meshData.guiMeta.availableScalarNames)
        list.append(QString::fromStdString(name));
    return list;
}



