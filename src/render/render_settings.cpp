#include "render/render_settings.h"
#include "render/render_config.h"
#include "core/Colormaps.h"
#include "core/mesh_loader.h"
#include "core/mesh_quality.h"

#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <iterator>
#include <QFileInfo>
#include <QSettings>
#include <QImage>
#include <QBuffer>
#include <QPainter>
#include <QFont>

RenderSettings::RenderSettings(QObject* parent)
    : QObject(parent) {
    loadRecentFromSettings();

    connect(&m_meshWatcher, &QFutureWatcher<MeshLoadResult>::finished,
            this, &RenderSettings::onMeshParsed);
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

void RenderSettings::toggleGrid(bool visible) {
    if (m_state.showGrid == visible) return;
    m_state.showGrid = visible;
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

void RenderSettings::saveStateToSettings() const {
    QSettings s;
    s.beginGroup("state");
    s.setValue("camDistance", m_state.camera.distance);
    s.setValue("camFocal", QVariantList{ m_state.camera.focalPoint.x, m_state.camera.focalPoint.y, m_state.camera.focalPoint.z });
    s.setValue("camPos", QVariantList{ m_state.camera.position.x, m_state.camera.position.y, m_state.camera.position.z });
    s.setValue("camUp", QVariantList{ m_state.camera.viewUp.x, m_state.camera.viewUp.y, m_state.camera.viewUp.z });
    s.setValue("bgColor", QVariantList{ m_state.bgColor[0], m_state.bgColor[1], m_state.bgColor[2] });
    s.setValue("matSpecular", m_state.lighting.matSpecular);
    s.setValue("matShininess", m_state.lighting.matShininess);
    s.setValue("matRoughness", m_state.lighting.matRoughness);
    s.setValue("matMetallic", m_state.lighting.matMetallic);
    s.setValue("lightKeyIntensity", m_state.lighting.lightKeyIntensity);
    s.setValue("lightWarm", m_state.lighting.lightWarm);
    s.setValue("lightKitEnabled", m_state.lighting.lightKitEnabled);
    s.setValue("colormapChoice", m_state.colormapChoice);
    s.setValue("colormapReversed", m_state.colormapReversed);
    s.setValue("meshUseScalarColor", m_state.meshUseScalarColor);
    s.setValue("showVolume", m_state.showVolume);
    s.setValue("volumeUseColormap", m_state.volumeUseColormap);
    s.setValue("volumeColormapChoice", m_state.volumeColormapChoice);
    s.setValue("volumeColormapReversed", m_state.volumeColormapReversed);
    s.setValue("volumeStepSize", m_state.volumeStepSize);
    s.setValue("volumeOpacity", m_state.volumeOpacity);
    s.setValue("vectorScale", m_state.vectorScale);
    s.setValue("vectorScaleByMagnitude", m_state.vectorScaleByMagnitude);
    s.setValue("quickBarCollapsed", quickBarCollapsed);
    s.setValue("msaaSamples", msaaSamples);
    s.setValue("theme", static_cast<int>(m_theme));
    s.endGroup();
}

void RenderSettings::setTheme(AppTheme v) {
    if (m_theme != v) {
        m_theme = v;
        QSettings s;
        s.setValue("theme", static_cast<int>(v));
        emit themeChanged();
    }
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
    if (s.contains("matSpecular")) {
        m_state.lighting.matSpecular = s.value("matSpecular").toFloat();
        m_state.lighting.matShininess = s.value("matShininess").toFloat();
        if (s.contains("matRoughness")) {
            m_state.lighting.matRoughness = s.value("matRoughness").toFloat();
            m_state.lighting.matMetallic  = s.value("matMetallic").toFloat();
        }
        m_state.lighting.lightKeyIntensity = s.value("lightKeyIntensity").toFloat();
        m_state.lighting.lightWarm = s.value("lightWarm").toFloat();
        m_state.lighting.lightKitEnabled = s.value("lightKitEnabled").toBool();
    }
    if (s.contains("meshUseScalarColor")) {
        m_state.meshUseScalarColor = s.value("meshUseScalarColor").toBool();
    }
    if (s.contains("colormapChoice")) {
        m_state.colormapChoice = s.value("colormapChoice").toInt();
        m_state.colormapReversed = s.value("colormapReversed").toBool();
    }
    if (s.contains("showVolume")) {
        m_state.showVolume = s.value("showVolume").toBool();
    }
    if (s.contains("volumeStepSize")) {
        m_state.volumeStepSize = s.value("volumeStepSize").toFloat();
    }
    if (s.contains("volumeOpacity")) {
        m_state.volumeOpacity = s.value("volumeOpacity").toFloat();
    }
    if (s.contains("volumeColormapChoice")) {
        m_state.volumeColormapChoice = s.value("volumeColormapChoice").toInt();
        m_state.volumeColormapReversed = s.value("volumeColormapReversed").toBool();
    }
    if (s.contains("vectorScale")) {
        m_state.vectorScale = s.value("vectorScale").toFloat();
        m_state.vectorScaleByMagnitude = s.value("vectorScaleByMagnitude").toBool();
    }
    if (s.contains("quickBarCollapsed")) {
        quickBarCollapsed = s.value("quickBarCollapsed").toBool();
    }
    if (s.contains("theme")) {
        m_theme = (s.value("theme").toInt() == 1) ? AppTheme::Light : AppTheme::Dark;
    }
    if (s.contains("msaaSamples")) {
        const int n = s.value("msaaSamples").toInt();
        msaaSamples = (n <= 0) ? 0 : (n <= 2 ? 2 : 4);
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
    QFuture<MeshLoadResult> f =
        QtConcurrent::run([stdPath, taskToken, token]() -> MeshLoadResult {
            RenderMesh loaded = loadMeshFile(stdPath);
            taskToken->store(token); // mark this task's generation on completion
            MeshLoadResult res;
            res.mesh = std::make_shared<const RenderMesh>(std::move(loaded));
            // ponytail: analyze off-thread so the heavy weld/sort/traversal never
            // blocks the GUI thread. flatVerts is stable on the shared mesh.
            res.quality = analyzeMeshQuality(*res.mesh);
            return res;
        });
    m_loadingPath = stdPath;
    m_meshWatcher.setFuture(f);
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
    m_meshData.supportsCellGrid = loaded->supportsCellGrid;
    m_state.hasMeshLoaded = true;

    // Reset per-mesh vector state.
    m_state.showVectors = false;
    m_state.vectorUseColormap = false;
    m_state.clipEnabled = false;
    m_state.sliceEnabledX = m_state.sliceEnabledY = m_state.sliceEnabledZ = false;
    if (!loaded->pointVectorsData.empty()) {
        m_meshData.guiMeta.vectorName = loaded->availableVectorNames.front();
        m_state.vectorField = loaded->availableVectorNames.front();
        m_state.streamlineVectorField = loaded->availableVectorNames.front();
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
        m_state.dataScalarMin = 0.0f;
        m_state.dataScalarMax = 1.0f;
    }

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
    m_state.showVolume = false;
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
    float mn = m_meshData.guiMeta.scalars[0], mx = m_meshData.guiMeta.scalars[0];
    for (float v : m_meshData.guiMeta.scalars) { if (v < mn) mn = v; if (v > mx) mx = v; }
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
    markStateDirty(); emit meshDataUpdated();
    // NOTE: do NOT emit meshLoadStateChanged() here — load state
    // (hasMeshLoaded / meshHasScalars) is unchanged; activeScalarName is
    // already covered by meshDataUpdated (render_settings.h:80).
}

void RenderSettings::setActiveVectorField(const QString& fieldName) {
    if (fieldName.isEmpty()) return;
    if (m_meshData.guiMeta.availableVectorNames.empty() ||
        std::find(m_meshData.guiMeta.availableVectorNames.begin(),
                  m_meshData.guiMeta.availableVectorNames.end(),
                  fieldName.toStdString()) == m_meshData.guiMeta.availableVectorNames.end()) {
        setStatus(QString("Unknown vector field: %1").arg(fieldName));
        return;
    }
    m_meshData.guiMeta.vectorName = fieldName.toStdString();
    m_state.vectorField = fieldName.toStdString();
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

QStringList RenderSettings::getColormapNames() const {
    QStringList list;
    for (int i = 0; i < static_cast<int>(ColormapType::Count); ++i)
        list.append(QString::fromUtf8(Colormaps::getName(static_cast<ColormapType>(i))));
    return list;
}

QString RenderSettings::getColormapPreviewUri(int index) const {
    auto it = m_colormapPreviewCache.find(index);
    if (it != m_colormapPreviewCache.end()) return it->second;

    const int w = 100, h = 32;
    QImage img(w, h, QImage::Format_RGB888);
    ColormapType type = static_cast<ColormapType>(index);
    for (int x = 0; x < w; ++x) {
        float t = static_cast<float>(x) / static_cast<float>(w - 1);
        glm::vec3 c = Colormaps::evaluate(t, type);
        int r = static_cast<int>(glm::clamp(c.r, 0.0f, 1.0f) * 255.0f);
        int g = static_cast<int>(glm::clamp(c.g, 0.0f, 1.0f) * 255.0f);
        int b = static_cast<int>(glm::clamp(c.b, 0.0f, 1.0f) * 255.0f);
        for (int y = 0; y < h; ++y) img.setPixel(x, y, qRgb(r, g, b));
    }
    {
        QPainter p(&img);
        p.setRenderHint(QPainter::TextAntialiasing, true);
        QFont f("Sans", 10, QFont::Bold);
        f.setStretch(QFont::Condensed);
        f.setStyleStrategy(QFont::PreferAntialias);
        p.setFont(f);
        QRect r(0, 0, w, h);
        QString name = QString::fromUtf8(Colormaps::getName(type));
        p.setPen(Qt::black);
        p.drawText(r.translated(1, 1), Qt::AlignCenter, name);
        p.setPen(Qt::white);
        p.drawText(r, Qt::AlignCenter, name);
    }
    QByteArray ba;
    QBuffer buf(&ba);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    QString uri = QString("data:image/png;base64,") + QString::fromLatin1(ba.toBase64());
    m_colormapPreviewCache[index] = uri;
    return uri;
}

QVariantList RenderSettings::getColormapStops() const {
    QVariantList out;
    const int steps = 16;
    for (int i = 0; i <= steps; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(steps);
        float s = m_state.colormapReversed ? (1.0f - t) : t;
        glm::vec3 c = Colormaps::evaluate(s, static_cast<ColormapType>(m_state.colormapChoice));
        QVariantList stop;
        stop << t << c.r << c.g << c.b;
        out.append(QVariant(stop));
    }
    return out;
}

QVariantList RenderSettings::getVectorColormapStops() const {
    QVariantList out;
    const int steps = 16;
    for (int i = 0; i <= steps; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(steps);
        float s = m_state.vectorColormapReversed ? (1.0f - t) : t;
        glm::vec3 c = Colormaps::evaluate(s, static_cast<ColormapType>(m_state.vectorColormapChoice));
        QVariantList stop;
        stop << t << c.r << c.g << c.b;
        out.append(QVariant(stop));
    }
    return out;
}
