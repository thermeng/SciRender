#include "render/render_settings.h"
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
    meshColor[0] = 0.4f; meshColor[1] = 0.9f; meshColor[2] = 0.4f;
    surfaceColor[0] = 1.0f; surfaceColor[1] = 1.0f; surfaceColor[2] = 1.0f;
    bgColor[0] = 0.12f; bgColor[1] = 0.12f; bgColor[2] = 0.12f;

    worldCenterX = 0.0; worldCenterY = 0.0; worldCenterZ = 0.0;
    worldRadius = 1.0;

    loadRecentFromSettings();

    connect(&m_meshWatcher, &QFutureWatcher<std::shared_ptr<const RenderMesh>>::finished,
            this, &RenderSettings::onMeshParsed);
}

RenderSettings::~RenderSettings() = default;

void RenderSettings::publishRenderState(::Renderer* scene) {
    // Double-buffered handoff (GUI thread, exclusive access during synchronize).
    // Re-assemble the ~75-field snapshot ONLY when the GUI state changed; on
    // idle frames we hand the previously-built buffer straight through, so the
    // per-frame cost collapses to a single struct copy into the Renderer.
    if (m_stateDirty) {
        buildRenderState();
        m_stateDirty = false;
    }
    if (scene) scene->setState(m_renderSnapshot); // Renderer deep-copies it
}

void RenderSettings::buildRenderState() {
    RenderRenderState& s = m_renderSnapshot;
    s.camera = camera;
    s.showWireframe = showWireframe;
    s.showSurface = showSurface;
    s.showGrid = showGrid;
    s.showGizmo = showGizmo;
    s.showPoints = showPoints;
    s.pointSize = pointSize;
    s.lineWidth = lineWidth;
    s.pointUseScalar = pointUseScalar;
    s.pointOpacity = pointOpacity;
    s.surfaceOpacity = surfaceOpacity;
    s.cullMode = cullMode;
    s.showBounds = showBounds;
    s.showQualityOverlay = showQualityOverlay;
    s.showCellEdges = showCellEdges;
    s.qualityDegenerateTris = qualityDegenerateTris;
    s.qualityOpenEdges = qualityOpenEdges;
    s.qualityNonManifoldEdges = qualityNonManifoldEdges;
    s.orthographic = orthographic;

    s.autoRotate = autoRotate;
    s.showFps = showFps;
    s.useLod = useLod;

    std::copy(std::begin(meshColor), std::end(meshColor), s.meshColor);
    std::copy(std::begin(surfaceColor), std::end(surfaceColor), s.surfaceColor);
    std::copy(std::begin(bgColor), std::end(bgColor), s.bgColor);

    s.worldCenterX = worldCenterX; s.worldCenterY = worldCenterY; s.worldCenterZ = worldCenterZ;
    s.worldRadius = worldRadius;
    s.worldMinX = worldMinX; s.worldMaxX = worldMaxX;
    s.worldMinY = worldMinY; s.worldMaxY = worldMaxY;
    s.worldMinZ = worldMinZ; s.worldMaxZ = worldMaxZ;

    s.lighting = lighting;

    s.colormapChoice = colormapChoice;
    s.colormapReversed = colormapReversed;
    s.vectorColormapChoice = vectorColormapChoice;
    s.vectorColormapReversed = vectorColormapReversed;
    s.meshHasScalars = meshHasScalars;
    s.scalarMin = scalarMin; s.scalarMax = scalarMax;

    s.dataScalarMin = dataScalarMin; s.dataScalarMax = dataScalarMax;
    s.filterMin = filterMin; s.filterMax = filterMax;
    s.showScalarColorbar = showScalarColorbar;
    s.colorbarTicks = colorbarTicks;

    s.activeScalarName = activeScalarName;
    s.clipEnabled = clipEnabled;
    s.sliceHeightX = sliceHeightX; s.sliceHeightY = sliceHeightY; s.sliceHeightZ = sliceHeightZ;
    s.sliceEnabledX = sliceEnabledX; s.sliceEnabledY = sliceEnabledY; s.sliceEnabledZ = sliceEnabledZ;
    s.invertX = invertX; s.invertY = invertY; s.invertZ = invertZ;
    s.showVectors = showVectors;
    s.vectorScale = vectorScale;
    s.vectorStride = vectorStride;
    std::copy(std::begin(vectorColor), std::end(vectorColor), s.vectorColor);
    s.vectorUseColormap = vectorUseColormap;
    s.vectorScaleByMagnitude = vectorScaleByMagnitude;
    s.vectorMagTransform = vectorMagTransform;
    s.vectorField = m_guiMeta.vectorName;
    s.screenshotTransparent = screenshotTransparent;
    s.hasMeshLoaded = hasMeshLoaded;
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
    if (showWireframe == enabled) return;
    showWireframe = enabled;
    markStateDirty();
    emit wireframeChanged();
}

void RenderSettings::setUseLod(bool enabled) {
    if (useLod == enabled) return;
    useLod = enabled;
    m_renderer.markCameraMoving();
    markStateDirty(); emit viewChanged();
}

void RenderSettings::setMsaaSamples(int n) {
    n = qBound(0, n, 4); // ponytail: only 0/2/4 offered; clamp guards stray QML
    if (msaaSamples == n) return;
    msaaSamples = n;
    markStateDirty(); emit viewChanged();
}

void RenderSettings::toggleGrid(bool visible) {
    if (showGrid == visible) return;
    showGrid = visible;
    markStateDirty();
    emit gridVisibilityChanged();
}

void RenderSettings::toggleSurface(bool visible) {
    if (showSurface == visible) return;
    showSurface = visible;
    markStateDirty();
    emit surfaceVisibilityChanged();
}

void RenderSettings::snapToOrthoView(int axis) {
    camera.snapToOrthoView(axis);
    m_renderer.markCameraMoving();
    markStateDirty(); emit viewChanged();
}

void RenderSettings::snapToAxisView(int axis, bool flip) {
    int preset = flip ? (axis * 2 + 1) : (axis * 2);
    camera.snapToOrthoView(preset);
    m_renderer.markCameraMoving();
    markStateDirty(); emit viewChanged();
}

void RenderSettings::resetCamera() {
    camera.focalPoint = glm::dvec3(worldCenterX, worldCenterY, worldCenterZ);
    const double dx = worldMaxX - worldMinX;
    const double dy = worldMaxY - worldMinY;
    const double dz = worldMaxZ - worldMinZ;
    const double diag = std::sqrt(dx * dx + dy * dy + dz * dz);
    const double fitRadius = diag * 0.5;
    const double aspect = 1.0;
    const double fov = glm::radians(45.0);
    const double effFov = fov;
    double dist = fitRadius / std::tan(effFov * 0.5);
    dist *= 1.3;
    camera.distance = dist < 1.0 ? 1.0 : dist;
    camera.maxDistance = std::max(1000.0, camera.distance * 50.0);
    camera.position = camera.focalPoint + glm::dvec3(0.0, 0.0, camera.distance);
    camera.viewUp = glm::dvec3(0.0, 1.0, 0.0);
    camera.orthogonalizeViewUp();
    markStateDirty(); emit viewChanged();
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
    s.setValue("camDistance", camera.distance);
    s.setValue("camFocal", QVariantList{ camera.focalPoint.x, camera.focalPoint.y, camera.focalPoint.z });
    s.setValue("camPos", QVariantList{ camera.position.x, camera.position.y, camera.position.z });
    s.setValue("camUp", QVariantList{ camera.viewUp.x, camera.viewUp.y, camera.viewUp.z });
    s.setValue("bgColor", QVariantList{ bgColor[0], bgColor[1], bgColor[2] });
    s.setValue("matSpecular", lighting.matSpecular);
    s.setValue("matShininess", lighting.matShininess);
    s.setValue("lightKeyIntensity", lighting.lightKeyIntensity);
    s.setValue("lightWarm", lighting.lightWarm);
    s.setValue("lightKitEnabled", lighting.lightKitEnabled);
    s.setValue("colormapChoice", colormapChoice);
    s.setValue("colormapReversed", colormapReversed);
    s.setValue("vectorScale", vectorScale);
    s.setValue("vectorScaleByMagnitude", vectorScaleByMagnitude);
    s.setValue("quickBarCollapsed", quickBarCollapsed);
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
        camera.distance = s.value("camDistance").toDouble();
        readVec3("camFocal", camera.focalPoint);
        readVec3("camPos", camera.position);
        readVec3("camUp", camera.viewUp);
        camera.maxDistance = std::max(1000.0, camera.distance * 50.0);
        camera.orthogonalizeViewUp();
    }
    readFColor("bgColor", bgColor);
    if (s.contains("matSpecular")) {
        lighting.matSpecular = s.value("matSpecular").toFloat();
        lighting.matShininess = s.value("matShininess").toFloat();
        lighting.lightKeyIntensity = s.value("lightKeyIntensity").toFloat();
        lighting.lightWarm = s.value("lightWarm").toFloat();
        lighting.lightKitEnabled = s.value("lightKitEnabled").toBool();
    }
    if (s.contains("colormapChoice")) {
        colormapChoice = s.value("colormapChoice").toInt();
        colormapReversed = s.value("colormapReversed").toBool();
    }
    if (s.contains("vectorScale")) {
        vectorScale = s.value("vectorScale").toFloat();
        vectorScaleByMagnitude = s.value("vectorScaleByMagnitude").toBool();
    }
    if (s.contains("quickBarCollapsed")) {
        quickBarCollapsed = s.value("quickBarCollapsed").toBool();
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
    QFuture<std::shared_ptr<const RenderMesh>> f =
        QtConcurrent::run([stdPath, taskToken, token]() -> std::shared_ptr<const RenderMesh> {
            RenderMesh loaded = loadMeshFile(stdPath);
            taskToken->store(token); // mark this task's generation on completion
            return std::make_shared<const RenderMesh>(std::move(loaded));
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
    std::shared_ptr<const RenderMesh> loaded = m_meshWatcher.result();
    if (!loaded) return;

    if (loaded->vertices.empty()) {
        setStatus(QString("Could not load: unsupported format or empty file"));
        return;
    }

    // The single heavy CPU payload now lives in m_loadedMesh (shared, immutable).
    m_loadedMesh = loaded;

    // Build a LIGHT GUI meta copy: keep only the cheap metadata (names, bounds,
    // active scalar array) so field switches work without the heavy vertex/
    // normal/index/vector payloads. The heavy geometry stays ONLY in m_loadedMesh
    // (immutable, shared with the renderer); attributes (all scalar/vector field
    // arrays) also stay ONLY in m_loadedMesh and are read from there on switch.
    m_guiMeta = *loaded;
    {
        MeshQuality mq = analyzeMeshQuality(*loaded);
        degenerateFaces  = mq.degenerateFaces;
        openEdges        = mq.openEdges;
        nonManifoldEdges = mq.nonManifoldEdges;
        nonManifoldVerts = mq.nonManifoldVerts;
        watertight       = mq.watertight;
        qualityDegenerateTris  = std::move(mq.degenerateTriVerts);
        qualityOpenEdges        = std::move(mq.openEdgeVerts);
        qualityNonManifoldEdges = std::move(mq.nonManifoldEdgeVerts);
    }

    m_guiMeta.vertices.clear();   m_guiMeta.vertices.shrink_to_fit();
    m_guiMeta.normals.clear();    m_guiMeta.normals.shrink_to_fit();
    m_guiMeta.indices.clear();    m_guiMeta.indices.shrink_to_fit();
    m_guiMeta.pointVectorsData.clear(); m_guiMeta.pointVectorsData.shrink_to_fit();
    m_guiMeta.attributes.reset();

    worldMinX = loaded->bounds.minX; worldMaxX = loaded->bounds.maxX;
    worldMinY = loaded->bounds.minY; worldMaxY = loaded->bounds.maxY;
    worldMinZ = loaded->bounds.minZ; worldMaxZ = loaded->bounds.maxZ;
    worldCenterX = loaded->bounds.centerX;
    worldCenterY = loaded->bounds.centerY;
    worldCenterZ = loaded->bounds.centerZ;
    worldRadius  = loaded->bounds.worldRadius;

    QFileInfo fileInfo(QString::fromStdString(m_loadingPath));
    currentMeshName = fileInfo.fileName().toStdString();
    triangleCount = static_cast<int>(loaded->indices.size() / 3);
    pointCount = loaded->sourcePointCount >= 0
        ? loaded->sourcePointCount
        : static_cast<int>(loaded->vertices.size() / 3);
    meshDataType = loaded->datasetType;
    meshFormat = loaded->fileFormat;
    supportsCellGrid = loaded->supportsCellGrid;
    hasMeshLoaded = true;

    // Reset per-mesh vector state.
    showVectors = false;
    vectorUseColormap = false;
    clipEnabled = false;
    sliceEnabledX = sliceEnabledY = sliceEnabledZ = false;
    if (!loaded->pointVectorsData.empty()) {
        m_guiMeta.vectorName = loaded->availableVectorNames.front();
    } else {
        m_guiMeta.vectorName.clear();
    }
    markStateDirty(); emit meshDataUpdated();

    if (!loaded->scalars.empty()) {
        meshHasScalars = true;
        showScalarColorbar = true;
        activeScalarName = loaded->scalarName;
        recomputeScalarRange();
    } else {
        meshHasScalars = false;
        showScalarColorbar = false;
        dataScalarMin = 0.0f;
        dataScalarMax = 1.0f;
    }

    resetCamera();

    // Hand the immutable payload to the render thread (shared_ptr, no copy).
    m_renderer.setPendingMesh(m_loadedMesh);

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
    m_loadedMesh.reset();
    m_guiMeta = RenderMesh{};
    hasMeshLoaded = false;
    meshHasScalars = false;
    markStateDirty();
    triangleCount = 0;
    pointCount = 0;
    degenerateFaces = 0; openEdges = 0; nonManifoldEdges = 0; nonManifoldVerts = 0; watertight = false;
    qualityDegenerateTris.clear(); qualityOpenEdges.clear(); qualityNonManifoldEdges.clear();
    meshDataType = "";
    meshFormat = "";
    currentMeshName = "";
    emit meshLoadStateChanged();
}

void RenderSettings::requestScreenshot(const QString& path) {
    if (path.isEmpty()) return;
    emit screenshotRequested(path);
}

void RenderSettings::recomputeScalarRange() {
    if (m_guiMeta.scalars.empty()) return;
    float mn = m_guiMeta.scalars[0], mx = m_guiMeta.scalars[0];
    for (float v : m_guiMeta.scalars) { if (v < mn) mn = v; if (v > mx) mx = v; }
    if (mx - mn < 1e-6f) mx = mn + 1.0f;
    dataScalarMin = mn; dataScalarMax = mx;
    scalarMin = mn; scalarMax = mx;
    // ponytail: pin filter to full range on load so Max thumb sits at slider max (unfiltered)
    filterMin = dataScalarMin; filterMax = dataScalarMax;
}

void RenderSettings::setActiveScalarField(const QString& fieldName) {
    if (fieldName.toStdString() == activeScalarName) return;
    if (!m_loadedMesh || !m_loadedMesh->attributes.has_value()) return;
    auto it = m_loadedMesh->attributes->pointScalars.find(fieldName.toStdString());
    if (it == m_loadedMesh->attributes->pointScalars.end()) return;

    activeScalarName = fieldName.toStdString();
    m_guiMeta.scalarName = activeScalarName;

    // Build the scalar payload ONCE as a shared_ptr (zero-copy across threads)
    // and reuse it for both the GUI meta copy and the render-thread handoff,
    // so the field is copied a single time rather than twice.
    auto payload = std::make_shared<const std::vector<float>>(it->second);
    m_guiMeta.scalars = *payload;
    recomputeScalarRange();

    // Trigger a SCALAR-ONLY re-upload on the render thread (shared_ptr, no copy).
    m_renderer.markScalarDirty(payload);
    markStateDirty(); emit meshDataUpdated();
    // NOTE: do NOT emit meshLoadStateChanged() here — load state
    // (hasMeshLoaded / meshHasScalars) is unchanged; activeScalarName is
    // already covered by meshDataUpdated (render_settings.h:80).
}

void RenderSettings::setActiveVectorField(const QString& fieldName) {
    if (fieldName.isEmpty()) return;
    if (m_guiMeta.availableVectorNames.empty() ||
        std::find(m_guiMeta.availableVectorNames.begin(),
                  m_guiMeta.availableVectorNames.end(),
                  fieldName.toStdString()) == m_guiMeta.availableVectorNames.end()) {
        setStatus(QString("Unknown vector field: %1").arg(fieldName));
        return;
    }
    m_guiMeta.vectorName = fieldName.toStdString();
    m_renderer.markVectorGlyphDirty();
    markStateDirty(); emit meshDataUpdated();
}

void RenderSettings::setColormapChoice(int choice) {
    if (colormapChoice == choice) return;
    colormapChoice = choice;
    markStateDirty(); emit colormapChanged();
}

void RenderSettings::setColormapReversed(bool reversed) {
    if (colormapReversed == reversed) return;
    colormapReversed = reversed;
    markStateDirty(); emit colormapChanged();
}

void RenderSettings::setVectorColormapReversed(bool reversed) {
    if (vectorColormapReversed == reversed) return;
    vectorColormapReversed = reversed;
    markStateDirty(); emit vectorColormapChanged();
}

void RenderSettings::applyLightingPreset(int preset) {
    lighting.applyPreset(preset);
    markStateDirty(); emit lightingParametersChanged();
}

void RenderSettings::resetLighting() {
    lighting.reset();
    markStateDirty(); emit lightingParametersChanged();
}

QStringList RenderSettings::getAvailableScalars() const {
    QStringList list;
    for (const auto& name : m_guiMeta.availableScalarNames)
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
        float s = colormapReversed ? (1.0f - t) : t;
        glm::vec3 c = Colormaps::evaluate(s, static_cast<ColormapType>(colormapChoice));
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
        float s = vectorColormapReversed ? (1.0f - t) : t;
        glm::vec3 c = Colormaps::evaluate(s, static_cast<ColormapType>(vectorColormapChoice));
        QVariantList stop;
        stop << t << c.r << c.g << c.b;
        out.append(QVariant(stop));
    }
    return out;
}
