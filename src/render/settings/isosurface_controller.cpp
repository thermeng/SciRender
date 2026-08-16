#include "render/settings/isosurface_controller.h"

#include <QtConcurrent/QtConcurrentRun>

IsosurfaceController::IsosurfaceController(Renderer& renderer, QObject* parent)
    : QObject(parent)
    , m_renderer(renderer) {
    connect(&m_watcher, &QFutureWatcher<RenderMesh>::finished,
            this, &IsosurfaceController::onComputed);
    m_debounceTimer.setSingleShot(true);
    connect(&m_debounceTimer, &QTimer::timeout,
            this, &IsosurfaceController::onDebounceTimeout);
}

bool IsosurfaceController::isAvailable() const {
    return m_currentMesh && isosurface::canExtract(*m_currentMesh);
}

void IsosurfaceController::setCurrentMesh(std::shared_ptr<const RenderMesh> mesh,
                                          const std::string& field) {
    m_currentMesh = std::move(mesh);
    m_currentField = field;
}

void IsosurfaceController::setShowIsosurface(bool v) {
    if (m_showIsosurface == v) return;
    m_showIsosurface = v;
    emit showIsosurfaceChanged(v);
    if (v) {
        // The isosurface is colored by the colormap LUT, which only emits
        // color when scalar coloring is enabled — turn it on so the surface
        // is visible immediately in the active colormap band.
        emit needsScalarColor();
        recompute();
    } else {
        // Disable immediately. The null handoff clears the GPU isosurface.
        m_renderer.setPendingIsosurface(nullptr);
        emit displayDirty();
    }
}

void IsosurfaceController::setIsovalue(float v, float lo, float hi) {
    if (hi < lo) std::swap(lo, hi);
    float clamped = (hi > lo) ? std::clamp(v, lo, hi) : lo;
    if (m_isovalue == clamped) return;
    m_isovalue = clamped;
    emit isovalueChanged(clamped);
    // Debounced: a slider scrub spawns no more than one task after the pause.
    m_debounceTimer.start(150);
}

void IsosurfaceController::reset(float dataMin, float dataMax) {
    m_showIsosurface = false;
    m_isovalue = (dataMin + dataMax) * 0.5f;
    ++m_loadToken;
    m_watcher.waitForFinished();
    m_renderer.setPendingIsosurface(nullptr);
    emit showIsosurfaceChanged(false);
    emit isovalueChanged(m_isovalue);
}

void IsosurfaceController::clear() {
    m_showIsosurface = false;
    ++m_loadToken;
    m_watcher.waitForFinished();
    m_currentMesh.reset();
    m_currentField.clear();
    m_renderer.setPendingIsosurface(nullptr);
}

void IsosurfaceController::recompute() {
    if (!m_showIsosurface || !m_currentMesh || !isosurface::canExtract(*m_currentMesh)) {
        m_renderer.setPendingIsosurface(nullptr);
        emit displayDirty();
        return;
    }
    launchAsync();
}

void IsosurfaceController::onDebounceTimeout() {
    recompute();
}

void IsosurfaceController::launchAsync() {
    // A new setFuture() supersedes any in-flight watcher future, so only the
    // latest result is emitted. The completion slot additionally verifies
    // m_showIsosurface + the token to drop results that finish after a toggle-off.
    const uint64_t token = ++m_loadToken;
    m_taskToken = std::make_shared<std::atomic<uint64_t>>(token);
    const float iso = m_isovalue;
    const auto meshPtr = m_currentMesh;
    const auto field = m_currentField;
    m_watcher.setFuture(QtConcurrent::run(
        [meshPtr, iso, field, taskToken = m_taskToken, token]() -> RenderMesh {
            taskToken->store(token);
            return isosurface::extractIsosurface(*meshPtr, {iso}, field);
        }));
}

void IsosurfaceController::onComputed() {
    // Dropped if the user toggled the isosurface off while this compute ran,
    // or if a newer recompute superseded this result.
    if (!m_showIsosurface) return;
    if (!m_taskToken || m_taskToken->load() != m_loadToken) return;

    RenderMesh iso = m_watcher.result();
    if (!iso.vertices.empty()) {
        auto sp = std::make_shared<const RenderMesh>(std::move(iso));
        m_renderer.setPendingIsosurface(sp);
    } else {
        // No crossing at this level -> keep an empty surface (clears prior one).
        m_renderer.setPendingIsosurface(nullptr);
    }
    emit displayDirty();
}


