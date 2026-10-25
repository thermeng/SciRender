#pragma once
#include <QObject>
#include <QString>
#include <QByteArray>
#include <memory>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <chrono>

#include "core/mesh_loader.h"
#include "cfd/cfd_types.h"
#include "cfd/cfd_mesh_adapter.h"
#include "cfd/cfd_session.h"
#include "cfd/solution_state.h"
#include "cfd/transient_frame.h"
#include "core/post_fields.h"

class QTemporaryDir;
template<typename T> class QFutureWatcher;

// One immutable frame as published by the render-prep worker.
//
// The mesh, its computed post-field snapshot and the extraction status are
// bundled so the GUI can never observe a mesh paired with the PREVIOUS frame's
// snapshot (which is what happens when they are published as separate atomics),
// and so the worker never has to write into RenderSettings to report status.
struct CfdFrameSlot {
    std::shared_ptr<const RenderMesh> mesh;
    std::shared_ptr<const postfields::ComputedSnapshot> computed;
    std::string postStatus;      // empty on success
    bool postExtracted = false;  // false when auto re-extract carried the old snapshot
    int step = 0;
    double time = 0.0;
    uint64_t gen = 0;
};

class CfdController : public QObject {
    Q_OBJECT
public:
    explicit CfdController(QObject* parent=nullptr);
    ~CfdController();

    // File operations. All of them refuse while a solve is in flight (a Save
    // is allowed once a pause has settled at a step boundary).
    bool loadMesh(const QString& path);
    bool saveCase(const QString& path);
    bool loadCase(const QString& path);
    void exportVtu(const QString& path);

    const cfd::MeshLoadResult& meshResult() const { return m_mesh; }
    const cfd::CaseSettings& settings() const { return m_settings; }
    void setSettings(const cfd::CaseSettings& s) { m_settings = s; }
    void setPatchBCs(const std::vector<cfd::PatchBC>& bcs) { m_patchBCs = bcs; }
    const std::vector<cfd::PatchBC>& patchBCs() const { return m_patchBCs; }
    // Display name of the loaded mesh (the embedded name for a .scfd file,
    // never the temporary extraction path).
    QString meshName() const { return m_meshName; }
    QString casePath() const { return m_casePath; }
    QString lastError() const { return m_lastError; }

    // Starts a solve. Returns false (and emits failed()) when the request was
    // refused, e.g. while another run is active or a restored time already
    // reached the configured end time.
    bool run();
    void cancel();
    void pause();
    void resume();
    void step();

    bool isRunning() const { return m_running.load(std::memory_order_acquire); }
    bool isPaused() const;
    // True when the solver is parked at an iteration/time-step boundary, i.e.
    // the in-memory state is a consistent restart point.
    bool isPausedSettled() const;
    bool isCancelled() const { return m_cancel.load(); }
    int currentStep() const;
    double currentTime() const;

    // Zero the velocity/pressure fields and return the freshly initialized
    // result mesh so the user can confirm a clean start before re-running.
    // "Initialize" is the explicit reset in the Run-continues-from-state
    // model. Returns nullptr when no mesh is loaded or a run is active.
    std::shared_ptr<const RenderMesh> initialize(double ux = 0.0, double uy = 0.0, double p = 0.0,
                                                 double k = 1e-3, double omega = 1.0);
    // Dimension-aware form used by the 3D UI path (supplies uz as well).
    std::shared_ptr<const RenderMesh> initialize(const cfd::InitConditions& ic);
    std::shared_ptr<const RenderMesh> resultMesh() const;
    // Frame bus (Phase 2): latest immutable frame produced off the UI thread.
    // The slot is the single publish point — mesh, snapshot and status always
    // belong to the same time step.
    std::shared_ptr<const CfdFrameSlot> latestFrameSlot() const { return m_latestFrameSlot.load(); }
    std::shared_ptr<const RenderMesh> latestFrame() const {
        auto slot = m_latestFrameSlot.load();
        return slot ? slot->mesh : nullptr;
    }
    std::shared_ptr<const postfields::ComputedSnapshot> latestComputed() const {
        auto slot = m_latestFrameSlot.load();
        return slot ? slot->computed : nullptr;
    }
    int latestStep() const {
        auto slot = m_latestFrameSlot.load();
        return slot ? slot->step : 0;
    }
    double latestTime() const {
        auto slot = m_latestFrameSlot.load();
        return slot ? slot->time : 0.0;
    }
    // The extractor runs on the render-prep worker: it must be pure and must not
    // touch RenderSettings (see postfields::extractSnapshot).
    void setPostExtractor(std::function<postfields::SnapshotResult(const RenderMesh&)> fn) {
        m_postExtractor = std::move(fn);
    }

signals:
    void meshLoaded();
    void caseLoaded();
    void progress(int iteration, double continuity, double momentum);
    // Frames are NOT emitted from the worker: it publishes into latestFrameSlot()
    // and the page polls it on a timer. That keeps the whole GUI-side field
    // pipeline (RenderSettings + renderer) on the GUI thread.
    void finished(const QString& summary);
    void failed(const QString& error);

private:
    // Lifecycle helpers (GUI thread only).
    void stopWorkers();
    void resetSolutionState();
    void setError(const QString& msg);
    // Copies the live solver state into m_state so Save and a subsequent Run
    // both see the state a paused/just-finished run wrote.
    void captureLiveState();

    cfd::MeshLoadResult m_mesh;
    cfd::CaseSettings m_settings;
    // Dimension-dispatched solver ownership (2D or 3D).
    std::unique_ptr<cfd::ICfdSession> m_session;
    // Last known solution state (fields + transient clock + turbulence).
    cfd::SolutionSnapshot m_state;
    std::vector<cfd::PatchBC> m_patchBCs;
    std::atomic<bool> m_cancel{false};
    std::atomic<bool> m_running{false};
    std::atomic<double> m_lastTime{0.0}; // simulated time reached, for the summary
    QString m_meshPath;                  // temp/current mesh source
    QString m_meshName;                  // display name (never the temp path)
    QByteArray m_embeddedMesh;           // exact .scfd mesh bytes, if loaded
    QString m_casePath;                  // last loaded/saved .scfd
    QString m_caseDir;                   // dir used to resolve relative .pvd paths
    std::unique_ptr<QTemporaryDir> m_tempDir; // owns the extracted .scfd mesh
    QString m_lastError;

    QFutureWatcher<void>* m_solveWatcher = nullptr;
    // Read from the solver and both worker threads; must be atomic.
    std::atomic<uint64_t> m_gen{0};
    // Frame bus (Phase 2)
    std::atomic<std::shared_ptr<const TransientStep>> m_latestStep;
    std::atomic<std::shared_ptr<const CfdFrameSlot>> m_latestFrameSlot;
    std::function<postfields::SnapshotResult(const RenderMesh&)> m_postExtractor;
    std::atomic<bool> m_renderPrepRunning{false};
    std::thread m_renderPrepThread;
    std::atomic<bool> m_diskWriterRunning{false};
    std::thread m_diskWriterThread;
};
