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
    return isAvailable(m_placement);
}
bool IsosurfaceController::isAvailable(int placement) const {
    return m_currentMesh && isosurface::canExtract(*m_currentMesh, m_currentField, placement, m_snapshot.get());
}

void IsosurfaceController::setCurrentMesh(std::shared_ptr<const RenderMesh> mesh,
                                          const std::string& field) {
    m_currentMesh = std::move(mesh);
    m_currentField = field;
}
void IsosurfaceController::setCurrentMesh(std::shared_ptr<const RenderMesh> mesh,
                                          const std::string& field, int placement) {
    m_currentMesh = std::move(mesh);
    m_currentField = field;
    m_placement = std::clamp(placement, 0, 1);
}
void IsosurfaceController::setCurrentMesh(std::shared_ptr<const RenderMesh> mesh,
                                          const std::string& field, int placement,
                                          std::shared_ptr<const postfields::ComputedSnapshot> snap) {
    m_currentMesh = std::move(mesh);
    m_currentField = field;
    m_placement = std::clamp(placement, 0, 1);
    m_snapshot = std::move(snap);
}
void IsosurfaceController::setPlacement(int p) {
    int np = std::clamp(p, 0, 1);
    if (m_placement == np) return;
    m_placement = np;
    emit placementChanged(np);
    if (m_showIsosurface) recompute();
}

void IsosurfaceController::setShowIsosurface(bool v) {
    if (m_showIsosurface == v) return;
    m_showIsosurface = v;
    emit showIsosurfaceChanged(v);
    if (v) {
        // Flat user color (isosurfaceColor) — no colormap dependency.
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
    if (m_isovalue == clamped && m_isovalues.size()==1 && !m_isovalues.empty() && m_isovalues[0]==clamped) return;
    m_isovalue = clamped;
    m_isovalues = {clamped};
    emit isovalueChanged(clamped);
    emit isovaluesChanged(m_isovalues);
    // Debounced: a slider scrub spawns no more than one task after the pause.
    m_debounceTimer.start(150);
}

void IsosurfaceController::setIsovalues(const std::vector<float>& vals, float lo, float hi) {
    if (hi < lo) std::swap(lo, hi);
    std::vector<float> clamped;
    clamped.reserve(vals.size());
    for (float v: vals) {
        float c = (hi > lo) ? std::clamp(v, lo, hi) : lo;
        clamped.push_back(c);
    }
    std::sort(clamped.begin(), clamped.end());
    clamped.erase(std::unique(clamped.begin(), clamped.end(), [](float a,float b){ return std::abs(a-b)<1e-7f; }), clamped.end());
    if (clamped.empty()) { m_isovalue = (lo+hi)*0.5f; m_isovalues.clear(); emit isovaluesChanged(m_isovalues); return; }
    if (clamped == m_isovalues) return;
    m_isovalues = clamped;
    m_isovalue = m_isovalues.front();
    emit isovaluesChanged(m_isovalues);
    emit isovalueChanged(m_isovalue);
    m_debounceTimer.start(150);
}

void IsosurfaceController::reset(float dataMin, float dataMax) {
    const bool wasOn = m_showIsosurface;
    m_showIsosurface = false;
    m_livePending = false;
    m_isovalue = (dataMin + dataMax) * 0.5f;
    m_isovalues = {m_isovalue};
    ++m_loadToken;
    m_watcher.waitForFinished();
    m_renderer.setPendingIsosurface(nullptr);
    if (wasOn) {
        emit showIsosurfaceChanged(false);
    }
    emit isovalueChanged(m_isovalue);
    emit isovaluesChanged(m_isovalues);
}

void IsosurfaceController::clear() {
    m_showIsosurface = false;
    m_livePending = false;
    ++m_loadToken;
    if (m_taskToken) m_taskToken->store(m_loadToken);
    m_watcher.waitForFinished();
    m_watcher.setFuture(QFuture<RenderMesh>());
    m_taskToken.reset();
    m_currentMesh.reset();
    m_currentField.clear();
    m_currentField.shrink_to_fit();
    m_renderer.setPendingIsosurface(nullptr);
    emit displayDirty();
}

void IsosurfaceController::recompute() {
    if (!m_showIsosurface || !m_currentMesh || !isosurface::canExtract(*m_currentMesh, m_currentField, m_placement, m_snapshot.get())) {
        m_renderer.setPendingIsosurface(nullptr);
        emit displayDirty();
        return;
    }
    m_livePending = false;
    launchAsync();
}

void IsosurfaceController::requestLiveRecompute() {
    if (!m_showIsosurface || !m_currentMesh) return;
    if (!isosurface::canExtract(*m_currentMesh, m_currentField, m_placement, m_snapshot.get())) {
        m_renderer.setPendingIsosurface(nullptr);
        emit displayDirty();
        return;
    }
    if (m_watcher.isRunning()) { m_livePending = true; return; }
    m_livePending = false;
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
    const auto isos = m_isovalues.empty() ? std::vector<float>{m_isovalue} : m_isovalues;
    const auto meshPtr = m_currentMesh;
    const auto field = m_currentField;
    const int placement = m_placement;
    auto snapPtr = m_snapshot;
    m_watcher.setFuture(QtConcurrent::run(
        [meshPtr, isos, field, placement, snapPtr, taskToken = m_taskToken, token]() -> RenderMesh {
            taskToken->store(token);
            return isosurface::extractIsosurface(*meshPtr, isos, field, placement, snapPtr.get());
        }));
}

void IsosurfaceController::onComputed() {
    // Dropped if the user toggled the isosurface off while this compute ran,
    // or if a newer recompute superseded this result.
    if (!m_showIsosurface) { m_livePending = false; return; }
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
    // Playback chained relaunch: a frame arrived mid-extraction, so run once
    // more against the latest mesh instead of freezing until pause.
    if (m_livePending) {
        m_livePending = false;
        requestLiveRecompute();
    }
}


