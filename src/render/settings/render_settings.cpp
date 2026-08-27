#include "render/settings/render_settings.h"
#include "render/foundation/render_config.h"
#include "render/settings/AnimationExporter.h"
#include "core/Colormaps.h"
#include "core/FieldResolver.h"
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

    m_animationExporter = new AnimationExporter(this);
    m_animationExporter->wire(&m_animController, this);

    // Fly-to-face camera transitions: GUI-side 16 ms driver, see tickCameraTransition().
    m_camAnimTimer = new QTimer(this);
    m_camAnimTimer->setInterval(16);
    connect(m_camAnimTimer, &QTimer::timeout, this, &RenderSettings::tickCameraTransition);

    connect(&m_meshWatcher, &QFutureWatcher<MeshLoadResult>::finished,
            this, &RenderSettings::onMeshParsed);

    // PVD animation: frames arrive as immutable shared_ptr meshes; errors go
    // to the status bar.
    connect(&m_animController, &AnimationController::frameReady,
            this, &RenderSettings::onAnimationFrame);
    connect(&m_animController, &AnimationController::errorOccurred,
            this, [this](const QString& msg) { setStatus(msg); });

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
    if (!m_stateDirty && !m_store.isDirty()) return;
    m_store.state() = m_state;
    m_store.markDirty();
    if (m_store.publish(scene)) m_stateDirty = false;
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

void RenderSettings::toggleSurface(bool visible) {
    if (m_state.showSurface == visible) return;
    m_state.showSurface = visible;
    markStateDirty();
    emit viewChanged(ChangeFlag::Display);
}

void RenderSettings::snapToOrthoView(int axis) {
    snapToPresetAnimated(axis);
}

void RenderSettings::snapToAxisView(int axis, bool flip) {
    snapToPresetAnimated(flip ? (axis * 2 + 1) : (axis * 2));
}

void RenderSettings::snapGizmoAxis(int axis) {
    if (axis < 0 || axis > 2) return;
    // ParaView triad semantics: clicking an arrowhead aligns the camera to view
    // FROM that world axis; clicking it again flips to the opposite face.
    static const glm::dvec3 axes[3] = { {1,0,0}, {0,1,0}, {0,0,1} };
    const glm::dvec3 d = glm::normalize(m_state.camera.position - m_state.camera.focalPoint);
    const bool alreadyOnThisFace = glm::dot(d, axes[axis]) > 0.995;
    snapToAxisView(axis, alreadyOnThisFace);
}

// ---------------------------------------------------------------------------
// Animated camera transitions (fly-to-face)
//
// Snap paths hand the target pose to beginCameraTransition(); a 16 ms timer
// interpolates over ~400 ms with cubic ease-in-out. Each tick mirrors exactly
// what one mouse-move orbit step does (mutate m_state.camera,
// markCameraMoving, markStateDirty, emit viewChanged(Camera)), so LOD motion
// handling and repaint scheduling are indistinguishable from a hand orbit.
// Manual camera input calls cancelCameraTransition() first — no tug-of-war.
// ---------------------------------------------------------------------------
namespace {
constexpr double kCamAnimMs = 400.0;
}

void RenderSettings::snapToPresetAnimated(int preset) {
    Camera probe = m_state.camera;          // pure math on a copy
    probe.snapToOrthoView(preset);
    const CameraPose to{ probe.position, probe.focalPoint, probe.viewUp };
    const CameraPose from{ m_state.camera.position, m_state.camera.focalPoint, m_state.camera.viewUp };
    constexpr double kEps = 1e-12;
    if (glm::length(from.pos - to.pos) < kEps
        && glm::length(from.focal - to.focal) < kEps
        && glm::length(from.up - to.up) < kEps) {
        return;  // already on that face
    }
    beginCameraTransition(to);
}

void RenderSettings::beginCameraTransition(const CameraPose& to) {
    // Retargeting mid-flight starts from wherever the camera is right now.
    m_camAnimFrom = CameraPose{ m_state.camera.position, m_state.camera.focalPoint, m_state.camera.viewUp };
    m_camAnimTo = to;
    m_camAnimating = true;
    m_camAnimClock.restart();
    if (!m_camAnimTimer->isActive()) m_camAnimTimer->start();
}

void RenderSettings::cancelCameraTransition() {
    if (!m_camAnimating) return;
    m_camAnimating = false;
    m_camAnimTimer->stop();
}

void RenderSettings::schedulePostMotionRepaint() {
    // Mirrors ViewportWidget::mouseReleaseEvent's post-motion redraw. The LOD
    // debounce (0.14 s) must expire, then one frame clears the moving flag and
    // presents the full-resolution swap. Guarded: if a newer transition (or a
    // manual drag) is running when this fires, that path owns recovery.
    QTimer::singleShot(RenderConfig::defaults().postMotionRedrawMs, this, [this]() {
        if (!m_camAnimating) {
            markStateDirty();
            emit viewChanged(ChangeFlag::Camera);
        }
    });
}

void RenderSettings::tickCameraTransition() {
    if (!m_camAnimating) { m_camAnimTimer->stop(); return; }

    const double elapsed = static_cast<double>(m_camAnimClock.elapsed());
    const bool done = elapsed >= kCamAnimMs;
    // Land bit-exact on the requested pose rather than on the last sample.
    const CameraPose p = done ? m_camAnimTo
                              : interpolatePose(m_camAnimFrom, m_camAnimTo,
                                                easeInOutCubic(elapsed / kCamAnimMs));
    m_state.camera.position = p.pos;
    m_state.camera.focalPoint = p.focal;
    m_state.camera.viewUp = glm::normalize(p.up);
    m_state.camera.orthogonalizeViewUp();
    if (done) {
        m_camAnimating = false;
        m_camAnimTimer->stop();
        schedulePostMotionRepaint();
    }
    m_renderer.markCameraMoving();
    markStateDirty();
    emit viewChanged(ChangeFlag::Camera);
}

// Target pose for "reset camera": world-center focal point, fit-all distance,
// classic CAD isometric vantage — camera in the (+X,+Y,+Z) corner so all three
// axis faces read equally, world +Y kept screen-up (matches the historical
// default view's orientation). Computed entirely on a probe copy so callers can
// diff it against the live camera; only the distance/maxDistance guards are
// written through immediately — they are clamps consumed by dolly, not
// interpolated visual state, and manual input during a flight cancels anyway.
CameraPose RenderSettings::computeFitAllIsoPose() {
    Camera probe = m_state.camera;
    probe.focalPoint = glm::dvec3(m_state.worldCenterX, m_state.worldCenterY, m_state.worldCenterZ);

    const double dx = m_state.worldMaxX - m_state.worldMinX;
    const double dy = m_state.worldMaxY - m_state.worldMinY;
    const double dz = m_state.worldMaxZ - m_state.worldMinZ;
    const double diag = std::sqrt(dx * dx + dy * dy + dz * dz);
    const double fitRadius = diag * 0.5;

    const double fov = glm::radians(45.0);
    double dist = fitRadius / std::tan(fov * 0.5);
    dist *= RenderConfig::defaults().cameraFitMultiplier;
    probe.distance = dist < 1.0 ? 1.0 : dist;
    probe.maxDistance = std::max(1000.0, probe.distance * 50.0);

    const glm::dvec3 isoDir = glm::normalize(glm::dvec3(1.0, 1.0, 1.0));
    probe.position = probe.focalPoint + isoDir * probe.distance;
    probe.viewUp = glm::dvec3(0.0, 1.0, 0.0);
    probe.orthogonalizeViewUp();

    m_state.camera.distance = probe.distance;
    m_state.camera.maxDistance = probe.maxDistance;

    return { probe.position, probe.focalPoint, probe.viewUp };
}

// User-facing reset (menu, button, quick-bar, R key): flies smoothly to the
// isometric fit-all pose. Retargeting mid-flight starts from the current pose.
void RenderSettings::resetCamera() {
    // Capture the start FIRST — computeFitAllIsoPose writes the dolly guards
    // into the live camera, and the from/to diff below is what decides
    // whether a transition runs at all.
    const CameraPose from{ m_state.camera.position, m_state.camera.focalPoint, m_state.camera.viewUp };
    const CameraPose to = computeFitAllIsoPose();
    constexpr double kEps = 1e-12;
    if (glm::length(from.pos - to.pos) < kEps
        && glm::length(from.focal - to.focal) < kEps
        && glm::length(from.up - to.up) < kEps) {
        return;  // already framed
    }
    beginCameraTransition(to);
}

// Instant variant for contexts where a swoosh would be wrong — notably the
// mesh-load path, where the data itself changed and the user expects a fresh,
// immediate framing rather than a 400 ms glide from the previous model's view.
void RenderSettings::resetCameraInstant() {
    const CameraPose p = computeFitAllIsoPose();
    m_state.camera.position = p.pos;
    m_state.camera.focalPoint = p.focal;
    m_state.camera.viewUp = p.up;
    m_renderer.markCameraMoving();
    markStateDirty(); emit viewChanged(ChangeFlag::Camera);
    schedulePostMotionRepaint();  // same LOD-recovery need as any motion burst
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

void RenderSettings::clearRecentFiles() {
    recentFiles.clear();
    saveRecentToSettings();
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
        add("colorbarFontFamily",  [](const RenderSettings& r) { return QVariant(r.m_state.colorbarFontFamily); },   [](RenderSettings& r, const QVariant& v) { r.m_state.colorbarFontFamily = v.toString(); });
        add("colorbarFontBold",    [](const RenderSettings& r) { return QVariant(r.m_state.colorbarFontBold); },     [](RenderSettings& r, const QVariant& v) { r.m_state.colorbarFontBold = v.toBool(); });
        add("colorbarFontItalic",  [](const RenderSettings& r) { return QVariant(r.m_state.colorbarFontItalic); },    [](RenderSettings& r, const QVariant& v) { r.m_state.colorbarFontItalic = v.toBool(); });
        add("colorbarFontScale",   [](const RenderSettings& r) { return QVariant(r.m_state.colorbarFontScale); },            [](RenderSettings& r, const QVariant& v) { r.m_state.colorbarFontScale = v.toFloat(); });
        add("colorbarTickFontScale", [](const RenderSettings& r) { return QVariant(r.m_state.colorbarTickFontScale); },       [](RenderSettings& r, const QVariant& v) { r.m_state.colorbarTickFontScale = v.toFloat(); });
        add("colorbarLengthScale", [](const RenderSettings& r) { return QVariant(r.m_state.colorbarLengthScale); },          [](RenderSettings& r, const QVariant& v) { r.m_state.colorbarLengthScale = v.toFloat(); });
        add("colorbarThicknessScale", [](const RenderSettings& r) { return QVariant(r.m_state.colorbarThicknessScale); },    [](RenderSettings& r, const QVariant& v) { r.m_state.colorbarThicknessScale = v.toFloat(); });
        add("colorbarPanelOpacity",[](const RenderSettings& r) { return QVariant(r.m_state.colorbarPanelOpacity); },         [](RenderSettings& r, const QVariant& v) { r.m_state.colorbarPanelOpacity = v.toFloat(); });
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
        add("colorbarPanelEnabled", [](const RenderSettings& r) { return QVariant(r.m_state.colorbarPanelEnabled); },         [](RenderSettings& r, const QVariant& v) { r.m_state.colorbarPanelEnabled = v.toBool(); });
        add("colorbarShowAnnotation", [](const RenderSettings& r) { return QVariant(r.m_state.colorbarShowAnnotation); },     [](RenderSettings& r, const QVariant& v) { r.m_state.colorbarShowAnnotation = v.toBool(); });
        add("filterEnabled",       [](const RenderSettings& r) { return QVariant(r.m_state.filterEnabled); },                 [](RenderSettings& r, const QVariant& v) { r.m_state.filterEnabled = v.toBool(); });
        add("quickBarCollapsed",   [](const RenderSettings& r) { return QVariant(r.quickBarCollapsed); },                     [](RenderSettings& r, const QVariant& v) { r.quickBarCollapsed = v.toBool(); });
        add("showGizmo",           [](const RenderSettings& r) { return QVariant(r.m_state.showGizmo); },                     [](RenderSettings& r, const QVariant& v) { r.m_state.showGizmo = v.toBool(); });
        add("showBounds",          [](const RenderSettings& r) { return QVariant(r.m_state.showBounds); },                    [](RenderSettings& r, const QVariant& v) { r.m_state.showBounds = v.toBool(); });
        add("showLightMarkers",    [](const RenderSettings& r) { return QVariant(r.m_state.lighting.showLightMarkers); },     [](RenderSettings& r, const QVariant& v) { r.m_state.lighting.showLightMarkers = v.toBool(); });
        add("colorRangeOverrideEnabled", [](const RenderSettings& r) { return QVariant(r.m_state.colorRangeOverrideEnabled); }, [](RenderSettings& r, const QVariant& v) { r.m_state.colorRangeOverrideEnabled = v.toBool(); });
        add("volumeColorRangeOverrideEnabled", [](const RenderSettings& r) { return QVariant(r.m_state.volumeColorRangeOverrideEnabled); }, [](RenderSettings& r, const QVariant& v) { r.m_state.volumeColorRangeOverrideEnabled = v.toBool(); });
        add("sliceColorRangeOverrideEnabled", [](const RenderSettings& r) { return QVariant(r.m_state.sliceColorRangeOverrideEnabled); }, [](RenderSettings& r, const QVariant& v) { r.m_state.sliceColorRangeOverrideEnabled = v.toBool(); });
        add("glyphMagRangeOverrideEnabled", [](const RenderSettings& r) { return QVariant(r.m_state.glyphMagRangeOverrideEnabled); }, [](RenderSettings& r, const QVariant& v) { r.m_state.glyphMagRangeOverrideEnabled = v.toBool(); });
        add("streamlineMagRangeOverrideEnabled", [](const RenderSettings& r) { return QVariant(r.m_state.streamlineMagRangeOverrideEnabled); }, [](RenderSettings& r, const QVariant& v) { r.m_state.streamlineMagRangeOverrideEnabled = v.toBool(); });
        // int members
        add("colormapChoice",      [](const RenderSettings& r) { return QVariant(r.m_state.colormapChoice); },                [](RenderSettings& r, const QVariant& v) { r.m_state.colormapChoice = v.toInt(); });
        add("gizmoCorner",         [](const RenderSettings& r) { return QVariant(r.m_state.gizmoCorner); },                   [](RenderSettings& r, const QVariant& v) { r.m_state.gizmoCorner = v.toInt(); });
        add("gizmoSizeChoice",     [](const RenderSettings& r) { return QVariant(r.m_state.gizmoSizeChoice); },               [](RenderSettings& r, const QVariant& v) { r.m_state.gizmoSizeChoice = v.toInt(); });
        add("colorRangeLo",        [](const RenderSettings& r) { return QVariant(r.m_state.colorRangeLo); },                  [](RenderSettings& r, const QVariant& v) { r.m_state.colorRangeLo = v.toFloat(); });
        add("colorRangeHi",        [](const RenderSettings& r) { return QVariant(r.m_state.colorRangeHi); },                  [](RenderSettings& r, const QVariant& v) { r.m_state.colorRangeHi = v.toFloat(); });
        add("volumeColorRangeLo",  [](const RenderSettings& r) { return QVariant(r.m_state.volumeColorRangeLo); },            [](RenderSettings& r, const QVariant& v) { r.m_state.volumeColorRangeLo = v.toFloat(); });
        add("volumeColorRangeHi",  [](const RenderSettings& r) { return QVariant(r.m_state.volumeColorRangeHi); },            [](RenderSettings& r, const QVariant& v) { r.m_state.volumeColorRangeHi = v.toFloat(); });
        add("sliceColorRangeLo",   [](const RenderSettings& r) { return QVariant(r.m_state.sliceColorRangeLo); },             [](RenderSettings& r, const QVariant& v) { r.m_state.sliceColorRangeLo = v.toFloat(); });
        add("sliceColorRangeHi",   [](const RenderSettings& r) { return QVariant(r.m_state.sliceColorRangeHi); },             [](RenderSettings& r, const QVariant& v) { r.m_state.sliceColorRangeHi = v.toFloat(); });
        add("glyphMagRangeLo",     [](const RenderSettings& r) { return QVariant(r.m_state.glyphMagRangeLo); },               [](RenderSettings& r, const QVariant& v) { r.m_state.glyphMagRangeLo = v.toFloat(); });
        add("glyphMagRangeHi",     [](const RenderSettings& r) { return QVariant(r.m_state.glyphMagRangeHi); },               [](RenderSettings& r, const QVariant& v) { r.m_state.glyphMagRangeHi = v.toFloat(); });
        add("streamlineMagRangeLo",[](const RenderSettings& r) { return QVariant(r.m_state.streamlineMagRangeLo); },          [](RenderSettings& r, const QVariant& v) { r.m_state.streamlineMagRangeLo = v.toFloat(); });
        add("streamlineMagRangeHi",[](const RenderSettings& r) { return QVariant(r.m_state.streamlineMagRangeHi); },          [](RenderSettings& r, const QVariant& v) { r.m_state.streamlineMagRangeHi = v.toFloat(); });
        add("volumeColormapChoice",[](const RenderSettings& r) { return QVariant(r.m_state.volumeColormapChoice); },          [](RenderSettings& r, const QVariant& v) { r.m_state.volumeColormapChoice = v.toInt(); });
        add("volumeSliceAxis",     [](const RenderSettings& r) { return QVariant(r.m_state.volumeSliceAxis); },               [](RenderSettings& r, const QVariant& v) { r.m_state.volumeSliceAxis = v.toInt(); });
        add("volumeSliceColormapChoice", [](const RenderSettings& r) { return QVariant(r.m_state.volumeSliceColormapChoice); }, [](RenderSettings& r, const QVariant& v) { r.m_state.volumeSliceColormapChoice = v.toInt(); });
        add("vectorPlacement",     [](const RenderSettings& r) { return QVariant(r.m_state.vectorPlacement); },               [](RenderSettings& r, const QVariant& v) { r.m_state.vectorPlacement = v.toInt(); });
        add("maxPeelLayers",       [](const RenderSettings& r) { return QVariant(r.m_state.maxPeelLayers); },                 [](RenderSettings& r, const QVariant& v) { r.m_state.maxPeelLayers = v.toInt(); });
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

    // PVD collections are time sequences, not single meshes — they are routed
    // to AnimationController, which streams frames through onAnimationFrame().
    const QFileInfo pvdInfo(QString::fromStdString(stdPath));
    if (pvdInfo.suffix().compare(QLatin1String("pvd"), Qt::CaseInsensitive) == 0) {
        m_animSequenceActive = false; // fresh sequence re-initializes ranges
        {
            QString absPath = pvdInfo.absoluteFilePath();
            recentFiles.removeAll(absPath);
            recentFiles.prepend(absPath);
            while (recentFiles.size() > 8) recentFiles.removeLast();
            saveRecentToSettings();
        }
        setStatus(QString("Loading animation %1…").arg(pvdInfo.fileName()));
        m_animController.loadPvd(QString::fromStdString(stdPath));
        return;
    }

    // A plain mesh replaces any running animation.
    m_animController.clear();
    m_animSequenceActive = false;

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
    m_state.vectorColorMode = 0;
    m_state.glyphMagRangeOverrideEnabled = false;
    m_state.glyphMagRangeLo = 0.0f;
    m_state.glyphMagRangeHi = -1.0f;
    for (int i = 0; i < 3; ++i) {
        m_state.glyphCompRangeOverrideEnabled[i] = false;
        m_state.glyphCompRangeLo[i] = 0.0f;
        m_state.glyphCompRangeHi[i] = -1.0f;
    }
    m_state.clipEnabled = false;
    m_state.crinkleClipMode = false;
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
        m_state.filterEnabled = false;
        setFilterMin(m_state.dataScalarMin); setFilterMax(m_state.dataScalarMax);
        resetColorRangeOverride();
        resetVolumeColorRangeOverride();
        resetSliceColorRangeOverride();
        resetGlyphMagRangeOverride();
        resetStreamlineMagRangeOverride();
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
        m_state.filterEnabled = false;
        setFilterMin(m_state.dataScalarMin); setFilterMax(m_state.dataScalarMax);
        resetColorRangeOverride();
        resetVolumeColorRangeOverride();
        resetSliceColorRangeOverride();
        resetGlyphMagRangeOverride();
        resetStreamlineMagRangeOverride();
    }

    // Isosurface: a fresh mesh starts with the surface off and the threshold
    // centered on the new data range. (The ISO mesh is cleared on the render
    // thread by the null handoff in reset().)
    m_isoController.reset(m_state.dataScalarMin, m_state.dataScalarMax);
    m_isoController.setCurrentMesh(m_meshData.loadedMesh, m_state.activeScalarName);

    resetCameraInstant();

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

void RenderSettings::onAnimationFrame(std::shared_ptr<const RenderMesh> mesh, int frameIndex, double time) {
    if (!mesh || mesh->vertices.empty()) return;

    const bool firstFrame = !m_animSequenceActive;
    m_animSequenceActive = true;

    // LIGHT per-frame publish: swap the geometry payload + metadata, keep the
    // camera, display toggles, field selections, filter window, recent files
    // and quality overlay exactly as the user left them.
    // Capture the PREVIOUS mesh BEFORE overwriting — the topology fast-path
    // below must compare against what the GPU actually holds, not against the
    // frame we just received (a self-compare would always report "unchanged").
    std::shared_ptr<const RenderMesh> prevMesh = m_meshData.loadedMesh;
    m_meshData.loadedMesh = mesh;
    m_meshData.triangleCount = static_cast<int>(mesh->indices.size() / 3);
    m_meshData.pointCount = mesh->sourcePointCount >= 0
        ? mesh->sourcePointCount
        : static_cast<int>(mesh->vertices.size() / 3);
    m_meshData.datasetType = mesh->datasetType;
    m_meshData.meshFormat = mesh->fileFormat;

    // World bounds track the frame (usually constant across a sequence) but
    // the camera is NOT reset — an animation must not yank the view.
    m_state.worldMinX = mesh->bounds.minX; m_state.worldMaxX = mesh->bounds.maxX;
    m_state.worldMinY = mesh->bounds.minY; m_state.worldMaxY = mesh->bounds.maxY;
    m_state.worldMinZ = mesh->bounds.minZ; m_state.worldMaxZ = mesh->bounds.maxZ;
    m_state.worldCenterX = mesh->bounds.centerX;
    m_state.worldCenterY = mesh->bounds.centerY;
    m_state.worldCenterZ = mesh->bounds.centerZ;
    m_state.worldRadius  = mesh->bounds.worldRadius;
    m_state.hasMeshLoaded = true;

    // Vector availability gates (same fixups as onMeshParsed, but preserving
    // show* toggles so playback doesn't switch overlays off every frame).
    m_state.meshHasVectors = !mesh->pointVectorsData.empty();
    m_state.meshHasCellVectors = !mesh->cellVectorsData.empty();

    // Field continuity: keep the user's active scalar/vector when this frame
    // carries it; otherwise fall back to the frame's first available.
    const std::string resolvedScalar = FieldResolver::resolveActiveScalar(*mesh, m_state.activeScalarName);
    if (!resolvedScalar.empty()) m_state.activeScalarName = resolvedScalar;
    m_meshData.guiMeta.scalarName = m_state.activeScalarName;
    m_meshData.guiMeta.availableScalarNames = mesh->availableScalarNames;
    m_meshData.guiMeta.availableVectorNames = mesh->availableVectorNames;
    m_meshData.guiMeta.availableCellVectorNames = mesh->availableCellVectorNames;
    if (m_state.meshHasVectors) {
        if (m_state.vectorField.empty()
            || std::find(mesh->availableVectorNames.begin(), mesh->availableVectorNames.end(),
                         m_state.vectorField) == mesh->availableVectorNames.end())
            m_state.vectorField = FieldResolver::resolveVectorName(*mesh, "", 0);
        if (m_state.streamlineVectorField.empty()
            || std::find(mesh->availableVectorNames.begin(), mesh->availableVectorNames.end(),
                         m_state.streamlineVectorField) == mesh->availableVectorNames.end())
            m_state.streamlineVectorField = m_state.vectorField;
        m_meshData.guiMeta.vectorName = m_state.vectorField;
    } else if (m_state.meshHasCellVectors) {
        m_meshData.guiMeta.vectorName = mesh->cellVectorName;
    }

    // Scalar range: normalize against the ACTIVE field resolved through the
    // same seam as the GPU payload (FieldResolver::scalarData) — NOT the
    // parser-default mesh->scalars, which desyncs as soon as the user selects
    // another field and knows nothing about cell-only or derived fields.
    // Whole-sequence mode (default) holds a GLOBAL range so colormap, colorbar
    // and filters don't flicker between frames: seeded on the first frame or a
    // mid-sequence field switch, expand-only afterwards. Per-frame mode
    // rescales to each frame's own extent.
    float mn = 0.0f, mx = 1.0f;
    const bool haveRange =
        FieldResolver::scalarData(*mesh, m_state.activeScalarName, mn, mx) != nullptr;
    if (!haveRange) { mn = 0.0f; mx = 1.0f; }
    if (mx - mn < 1e-6f) mx = mn + 1.0f;

    // Reseeding on a mid-sequence field switch lives inside advance(): expanding
    // a union across a field switch would map one field's values onto another
    // field's range.
    float effMin = mn, effMax = mx;
    m_animRange.advance(firstFrame, m_state.activeScalarName, mn, mx, effMin, effMax);

    if (firstFrame) {
        m_state.meshHasScalars = haveRange;
        // Do not auto-enable scalar visualization — user must enable manually
        m_state.meshUseScalarColor = false;
        m_state.showScalarColorbar = m_state.meshHasScalars;
        m_state.filterEnabled = false;
        setFilterMin(effMin); setFilterMax(effMax);
        m_isoController.reset(effMin, effMax);
        resetColorRangeOverride(effMin, effMax);
        resetVolumeColorRangeOverride();
        resetSliceColorRangeOverride();
        resetGlyphMagRangeOverride();
        resetStreamlineMagRangeOverride();
    }
    m_state.dataScalarMin = effMin;
    m_state.dataScalarMax = effMax;
    m_state.scalarMin = effMin;
    m_state.scalarMax = effMax;

    // Isosurface follows the animated mesh (debounced async extraction).
    m_isoController.setCurrentMesh(mesh, m_state.activeScalarName);

    // Phase 1.1: scalar-only fast path for fixed-mesh animations.
    // Compare the incoming frame against the mesh the GPU last received
    // (prevMesh), NOT against m_meshData.loadedMesh (already == mesh above —
    // that self-compare always reported "unchanged" and silently disabled the
    // full-upload path for topology-varying sequences).
    bool topologyUnchanged = false;
    if (!firstFrame && prevMesh && mesh->geometryHash != 0
        && prevMesh->geometryHash == mesh->geometryHash
        && prevMesh->vertices.size() == mesh->vertices.size()
        && prevMesh->indices.size() == mesh->indices.size()
        && prevMesh->gridDimX == mesh->gridDimX
        && prevMesh->gridDimY == mesh->gridDimY
        && prevMesh->gridDimZ == mesh->gridDimZ) {
        topologyUnchanged = true;
    }
    if (topologyUnchanged) {
        // Only scalars (and derived) changed — re-upload SBO (+ volume texture).
        float rmn, rmx;
        if (auto* d = FieldResolver::scalarData(*mesh, m_state.activeScalarName, rmn, rmx)) {
            auto payload = std::make_shared<const std::vector<float>>(*d);
            m_renderer.markScalarDirty(payload);
            if (mesh->hasVolumeData()) {
                m_renderer.markVolumeDirty(mesh);
                // Renderer consumes volumeDirty and uploads via
                // uploadVolumeFromScalarDirty with the new mesh's dims.
            }
        }
    } else {
        // Full upload for topology-changing frames (adaptive mesh, first frame)
        m_renderer.setPendingMesh(mesh);
    }

    markStateDirty();
    emit meshLoadStateChanged();
    emit meshDataUpdated();
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
    m_animController.clear();
    m_animSequenceActive = false;
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
    float mn = 0.f, mx = 1.f;
    bool have = false;
    if (!m_meshData.guiMeta.scalars.empty()) {
        mn = std::numeric_limits<float>::max();
        mx = std::numeric_limits<float>::lowest();
        for (float v : m_meshData.guiMeta.scalars) {
            if (!std::isfinite(v)) continue;
            mn = std::min(mn, v);
            mx = std::max(mx, v);
            have = true;
        }
    }
    // Structured-grid surface extract clears guiMeta.scalars but the field
    // lives in loadedMesh->attributes->pointScalars (node-space). Fall back
    // to the authoritative per-field range via FieldResolver.
    if (!have && m_meshData.loadedMesh) {
        float rmn, rmx;
        if (auto* d = FieldResolver::scalarData(*m_meshData.loadedMesh, m_state.activeScalarName, rmn, rmx)) {
            mn = rmn; mx = rmx; have = true;
        } else if (m_meshData.loadedMesh->attributes) {
            auto it = m_meshData.loadedMesh->attributes->pointScalarRanges.find(m_state.activeScalarName);
            if (it != m_meshData.loadedMesh->attributes->pointScalarRanges.end()) {
                mn = it->second.first; mx = it->second.second; have = true;
            } else {
                auto cit = m_meshData.loadedMesh->attributes->cellScalarRanges.find(m_state.activeScalarName);
                if (cit != m_meshData.loadedMesh->attributes->cellScalarRanges.end()) {
                    mn = cit->second.first; mx = cit->second.second; have = true;
                }
            }
        }
    }
    if (!have) {
        // Keep previous range if we still have no data (should not happen)
        if (m_state.dataScalarMin != m_state.dataScalarMax) return;
        mn = 0.f; mx = 1.f;
    }
    if (mn > mx) { mn = 0.f; mx = 1.f; }
    if (std::abs(mx - mn) < 1e-6f) mx = mn + 1.0f;
    m_state.dataScalarMin = mn; m_state.dataScalarMax = mx;
    m_state.scalarMin = mn; m_state.scalarMax = mx;
}

void RenderSettings::setActiveScalarField(const QString& fieldName) {
    if (fieldName.toStdString() == m_state.activeScalarName) return;
    if (!m_meshData.loadedMesh) return;
    float mn, mx;
    const auto* data = FieldResolver::scalarData(*m_meshData.loadedMesh, fieldName.toStdString(), mn, mx);
    if (!data) return;
    m_state.activeScalarName = fieldName.toStdString();
    m_meshData.guiMeta.scalarName = m_state.activeScalarName;

    auto payload = std::make_shared<const std::vector<float>>(*data);
    m_meshData.guiMeta.scalars = *payload;
    m_state.dataScalarMin = mn; m_state.dataScalarMax = mx;
    m_state.scalarMin = mn;     m_state.scalarMax = mx;
    recomputeScalarRange();
    m_state.filterMin = m_state.dataScalarMin;
    m_state.filterMax = m_state.dataScalarMax;
    resetColorRangeOverride();
    resetVolumeColorRangeOverride();
    resetSliceColorRangeOverride();
    // Mid-sequence switch: reseed the animation range accumulator from THIS
    // frame so playback normalizes the new field against its own range, not
    // the previous field's union. (AnimRangeState::advance also reseeds on any
    // field-name mismatch — this covers a paused sequence.)
    if (m_animSequenceActive) {
        m_animRange.field = fieldName.toStdString();
        m_animRange.rangeMin = mn;
        m_animRange.rangeMax = mx;
    }
    emit meshLoadStateChanged();

    m_renderer.markScalarDirty(payload);
    if (m_meshData.loadedMesh)
        m_renderer.markVolumeDirty(m_meshData.loadedMesh);
    markStateDirty();
    emit meshDataUpdated();
    // Also notify viewChanged so isosurface slider (volume_page) and any
    // display that depends on dataScalarMin/Max refreshes. meshLoadStateChanged
    // alone does not reach syncVolumePage.
    emit viewChanged(ChangeFlag::Display);
    if (m_state.showIsosurface && m_state.meshHasScalars) {
        m_isoController.setCurrentField(m_state.activeScalarName);
        m_isoController.recompute();
    }
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
    resetGlyphMagRangeOverride();
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
    resetStreamlineMagRangeOverride();
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

void RenderSettings::setFilterMin(float v) {
    v = qBound(m_state.dataScalarMin, v, m_state.dataScalarMax);
    v = std::min(v, m_state.filterMax);
    if (m_state.filterMin == v) return;
    m_state.filterMin = v;
    markStateDirty(); emit viewChanged(ChangeFlag::Display);
}
void RenderSettings::setFilterMax(float v) {
    v = qBound(m_state.dataScalarMin, v, m_state.dataScalarMax);
    v = std::max(v, m_state.filterMin);
    if (m_state.filterMax == v) return;
    m_state.filterMax = v;
    markStateDirty(); emit viewChanged(ChangeFlag::Display);
}
void RenderSettings::setFilterEnabled(bool v) {
    if (m_state.filterEnabled == v) return;
    m_state.filterEnabled = v;
    markStateDirty(); emit viewChanged(ChangeFlag::Display);
}

// ---- fixed custom colormap range -------------------------------------------

void RenderSettings::setColorRangeOverrideEnabled(bool v) {
    if (m_state.colorRangeOverrideEnabled == v) return;
    // Enabling with a degenerate/untouched [0,1] window: seed it from the data
    // range so the first toggle visibly does something sane.
    if (v && m_state.colorRangeHi - m_state.colorRangeLo <= 0.0f) {
        m_state.colorRangeLo = m_state.dataScalarMin;
        m_state.colorRangeHi = m_state.dataScalarMax;
    }
    m_state.colorRangeOverrideEnabled = v;
    markStateDirty(); emit viewChanged(ChangeFlag::Colormap);
}

void RenderSettings::setColorRangeLo(float v) {
    v = qBound(m_state.dataScalarMin, v, m_state.dataScalarMax);
    v = std::min(v, m_state.colorRangeHi);
    if (m_state.colorRangeLo == v) return;
    m_state.colorRangeLo = v;
    markStateDirty(); emit viewChanged(ChangeFlag::Colormap);
}

void RenderSettings::setColorRangeHi(float v) {
    v = qBound(m_state.dataScalarMin, v, m_state.dataScalarMax);
    v = std::max(v, m_state.colorRangeLo);
    if (m_state.colorRangeHi == v) return;
    m_state.colorRangeHi = v;
    markStateDirty(); emit viewChanged(ChangeFlag::Colormap);
}

void RenderSettings::resetColorRangeOverride() {
    resetColorRangeOverride(m_state.dataScalarMin, m_state.dataScalarMax);
}

void RenderSettings::resetColorRangeOverride(float lo, float hi) {
    const bool wasDirty = m_state.colorRangeOverrideEnabled
        || m_state.colorRangeLo != lo
        || m_state.colorRangeHi != hi;
    m_state.colorRangeOverrideEnabled = false;
    m_state.colorRangeLo = lo;
    m_state.colorRangeHi = hi;
    if (wasDirty) { markStateDirty(); emit viewChanged(ChangeFlag::Colormap); }
}

// ---- per-pass fixed colormap windows ---------------------------------------
// Volume/slice clamp into the scalar data range (GUI-owned copy). Magnitude
// pairs enforce ordering only and seed from the renderer's scan values on
// first enable; their resets park the pair in a degenerate state so a later
// enable re-seeds against whatever data is current.

void RenderSettings::resetColorRangeOverrideImpl(bool& enabled, float& lo, float& hi,
                                                 float snapLo, float snapHi) {
    const bool wasDirty = enabled || lo != snapLo || hi != snapHi;
    enabled = false;
    lo = snapLo;
    hi = snapHi;
    if (wasDirty) { markStateDirty(); emit viewChanged(ChangeFlag::Colormap); }
}

void RenderSettings::setVolumeColorRangeOverrideEnabled(bool v) {
    if (m_state.volumeColorRangeOverrideEnabled == v) return;
    if (v && m_state.volumeColorRangeHi - m_state.volumeColorRangeLo <= 0.0f) {
        m_state.volumeColorRangeLo = m_state.dataScalarMin;
        m_state.volumeColorRangeHi = m_state.dataScalarMax;
    }
    m_state.volumeColorRangeOverrideEnabled = v;
    markStateDirty(); emit viewChanged(ChangeFlag::Colormap);
}
void RenderSettings::setVolumeColorRangeLo(float v) {
    v = qBound(m_state.dataScalarMin, v, m_state.dataScalarMax);
    v = std::min(v, m_state.volumeColorRangeHi);
    if (m_state.volumeColorRangeLo == v) return;
    m_state.volumeColorRangeLo = v;
    markStateDirty(); emit viewChanged(ChangeFlag::Colormap);
}
void RenderSettings::setVolumeColorRangeHi(float v) {
    v = qBound(m_state.dataScalarMin, v, m_state.dataScalarMax);
    v = std::max(v, m_state.volumeColorRangeLo);
    if (m_state.volumeColorRangeHi == v) return;
    m_state.volumeColorRangeHi = v;
    markStateDirty(); emit viewChanged(ChangeFlag::Colormap);
}
void RenderSettings::resetVolumeColorRangeOverride() {
    resetColorRangeOverrideImpl(m_state.volumeColorRangeOverrideEnabled,
                                m_state.volumeColorRangeLo, m_state.volumeColorRangeHi,
                                m_state.dataScalarMin, m_state.dataScalarMax);
}

void RenderSettings::setSliceColorRangeOverrideEnabled(bool v) {
    if (m_state.sliceColorRangeOverrideEnabled == v) return;
    if (v && m_state.sliceColorRangeHi - m_state.sliceColorRangeLo <= 0.0f) {
        m_state.sliceColorRangeLo = m_state.dataScalarMin;
        m_state.sliceColorRangeHi = m_state.dataScalarMax;
    }
    m_state.sliceColorRangeOverrideEnabled = v;
    markStateDirty(); emit viewChanged(ChangeFlag::Colormap);
}
void RenderSettings::setSliceColorRangeLo(float v) {
    v = qBound(m_state.dataScalarMin, v, m_state.dataScalarMax);
    v = std::min(v, m_state.sliceColorRangeHi);
    if (m_state.sliceColorRangeLo == v) return;
    m_state.sliceColorRangeLo = v;
    markStateDirty(); emit viewChanged(ChangeFlag::Colormap);
}
void RenderSettings::setSliceColorRangeHi(float v) {
    v = qBound(m_state.dataScalarMin, v, m_state.dataScalarMax);
    v = std::max(v, m_state.sliceColorRangeLo);
    if (m_state.sliceColorRangeHi == v) return;
    m_state.sliceColorRangeHi = v;
    markStateDirty(); emit viewChanged(ChangeFlag::Colormap);
}
void RenderSettings::resetSliceColorRangeOverride() {
    resetColorRangeOverrideImpl(m_state.sliceColorRangeOverrideEnabled,
                                m_state.sliceColorRangeLo, m_state.sliceColorRangeHi,
                                m_state.dataScalarMin, m_state.dataScalarMax);
}

void RenderSettings::setGlyphMagRangeOverrideEnabled(bool v) {
    if (m_state.glyphMagRangeOverrideEnabled == v) return;
    if (v && m_state.glyphMagRangeHi - m_state.glyphMagRangeLo <= 0.0f) {
        m_state.glyphMagRangeLo = m_renderer.vectorMagMin();
        m_state.glyphMagRangeHi = std::max(m_renderer.vectorMagMax(), m_renderer.vectorMagMin() + 1e-6f);
    }
    m_state.glyphMagRangeOverrideEnabled = v;
    markStateDirty(); emit viewChanged(ChangeFlag::Colormap);
}
void RenderSettings::setGlyphMagRangeLo(float v) {
    v = std::min(v, m_state.glyphMagRangeHi);
    if (m_state.glyphMagRangeLo == v) return;
    m_state.glyphMagRangeLo = v;
    markStateDirty(); emit viewChanged(ChangeFlag::Colormap);
}
void RenderSettings::setGlyphMagRangeHi(float v) {
    v = std::max(v, m_state.glyphMagRangeLo);
    if (m_state.glyphMagRangeHi == v) return;
    m_state.glyphMagRangeHi = v;
    markStateDirty(); emit viewChanged(ChangeFlag::Colormap);
}
void RenderSettings::resetGlyphMagRangeOverride() {
    const bool wasDirty = m_state.glyphMagRangeOverrideEnabled
        || m_state.glyphMagRangeHi >= m_state.glyphMagRangeLo;
    // Park degenerate so the next enable re-seeds against current data.
    m_state.glyphMagRangeOverrideEnabled = false;
    m_state.glyphMagRangeLo = 0.0f;
    m_state.glyphMagRangeHi = -1.0f;
    if (wasDirty) { markStateDirty(); emit viewChanged(ChangeFlag::Colormap); }
}

void RenderSettings::setStreamlineMagRangeOverrideEnabled(bool v) {
    if (m_state.streamlineMagRangeOverrideEnabled == v) return;
    if (v && m_state.streamlineMagRangeHi - m_state.streamlineMagRangeLo <= 0.0f) {
        m_state.streamlineMagRangeLo = m_renderer.streamlineMagMin();
        m_state.streamlineMagRangeHi = std::max(m_renderer.streamlineMagMax(), m_renderer.streamlineMagMin() + 1e-6f);
    }
    m_state.streamlineMagRangeOverrideEnabled = v;
    markStateDirty(); emit viewChanged(ChangeFlag::Colormap);
}
void RenderSettings::setStreamlineMagRangeLo(float v) {
    v = std::min(v, m_state.streamlineMagRangeHi);
    if (m_state.streamlineMagRangeLo == v) return;
    m_state.streamlineMagRangeLo = v;
    markStateDirty(); emit viewChanged(ChangeFlag::Colormap);
}
void RenderSettings::setStreamlineMagRangeHi(float v) {
    v = std::max(v, m_state.streamlineMagRangeLo);
    if (m_state.streamlineMagRangeHi == v) return;
    m_state.streamlineMagRangeHi = v;
    markStateDirty(); emit viewChanged(ChangeFlag::Colormap);
}
void RenderSettings::resetStreamlineMagRangeOverride() {
    const bool wasDirty = m_state.streamlineMagRangeOverrideEnabled
        || m_state.streamlineMagRangeHi >= m_state.streamlineMagRangeLo;
    m_state.streamlineMagRangeOverrideEnabled = false;
    m_state.streamlineMagRangeLo = 0.0f;
    m_state.streamlineMagRangeHi = -1.0f;
    if (wasDirty) { markStateDirty(); emit viewChanged(ChangeFlag::Colormap); }
}

void RenderSettings::setGlyphCompRangeOverrideEnabled(int comp, bool v) {
    comp = std::clamp(comp, 0, 2);
    if (m_state.glyphCompRangeOverrideEnabled[comp] == v) return;
    if (v && m_state.glyphCompRangeHi[comp] - m_state.glyphCompRangeLo[comp] <= 0.0f) {
        const float cMin = m_state.vectorCompMin[comp];
        const float cMax = m_state.vectorCompMax[comp];
        m_state.glyphCompRangeLo[comp] = cMin;
        m_state.glyphCompRangeHi[comp] = std::max(cMax, cMin + 1e-6f);
    }
    m_state.glyphCompRangeOverrideEnabled[comp] = v;
    markStateDirty(); emit viewChanged(ChangeFlag::Colormap);
}
void RenderSettings::setGlyphCompRangeLo(int comp, float v) {
    comp = std::clamp(comp, 0, 2);
    v = std::min(v, m_state.glyphCompRangeHi[comp]);
    if (m_state.glyphCompRangeLo[comp] == v) return;
    m_state.glyphCompRangeLo[comp] = v;
    markStateDirty(); emit viewChanged(ChangeFlag::Colormap);
}
void RenderSettings::setGlyphCompRangeHi(int comp, float v) {
    comp = std::clamp(comp, 0, 2);
    v = std::max(v, m_state.glyphCompRangeLo[comp]);
    if (m_state.glyphCompRangeHi[comp] == v) return;
    m_state.glyphCompRangeHi[comp] = v;
    markStateDirty(); emit viewChanged(ChangeFlag::Colormap);
}
void RenderSettings::resetGlyphCompRangeOverride(int comp) {
    comp = std::clamp(comp, 0, 2);
    const bool wasDirty = m_state.glyphCompRangeOverrideEnabled[comp]
        || m_state.glyphCompRangeHi[comp] >= m_state.glyphCompRangeLo[comp];
    m_state.glyphCompRangeOverrideEnabled[comp] = false;
    m_state.glyphCompRangeLo[comp] = 0.0f;
    m_state.glyphCompRangeHi[comp] = -1.0f;
    if (wasDirty) { markStateDirty(); emit viewChanged(ChangeFlag::Colormap); }
}

void RenderSettings::setAnimScaleGlobal(bool global) {
    if (m_animRange.global == global) return;
    m_animRange.global = global;
    // Drop the accumulated union so the next frame reseeds under the new mode
    // instead of expanding a range accumulated for the other mode.
    m_animRange.invalidate();
    markStateDirty(); emit viewChanged(ChangeFlag::Display);
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
    if (m_meshData.loadedMesh) {
        auto names = FieldResolver::availableScalarNamesWithDerived(*m_meshData.loadedMesh);
        QStringList list; for (auto& n: names) list.append(QString::fromStdString(n));
        return list;
    }
    QStringList list;
    for (const auto& name : m_meshData.guiMeta.availableScalarNames)
        list.append(QString::fromStdString(name));
    return list;
}



