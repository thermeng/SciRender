#pragma once
#include <QObject>
#include <QString>
#include <memory>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <functional>
#include <string>
#include <thread>
#include <chrono>

#include "core/mesh_loader.h"
#include "cfd/cfd_types.h"
#include "cfd/cfd_mesh_adapter.h"
#include "cfd/cfd_types.h"
#include "cfd/transient_frame.h"
#include "core/post_fields.h"

namespace fvm {
template<std::size_t Dim> class Mesh;
template<std::size_t Dim> class SIMPLE;
template<std::size_t Dim> class VectorField;
template<std::size_t Dim> class Field;
template<std::size_t Dim> class SSTModel;
}

namespace cfd { class CaseSettings; }

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

    bool loadMesh(const QString& path);
    bool saveCase(const QString& path);
    bool loadCase(const QString& path);
    const cfd::MeshLoadResult& meshResult() const { return m_mesh; }
    const cfd::CaseSettings& settings() const { return m_settings; }
    void setSettings(const cfd::CaseSettings& s) { m_settings = s; }
    void setPatchBCs(const std::vector<cfd::PatchBC>& bcs) { m_patchBCs = bcs; }
    const std::vector<cfd::PatchBC>& patchBCs() const { return m_patchBCs; }
    void run();
    void cancel();
    bool isCancelled() const { return m_cancel.load(); }
    // Zero the velocity/pressure fields and return the freshly initialized
    // result mesh so the user can confirm a clean start before re-running.
    // Returns nullptr when no mesh is loaded. Safe to call between runs; do
    // not call while a solve is in flight.
    std::shared_ptr<const RenderMesh> initialize();
    void exportVtu(const QString& path);
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
    cfd::MeshLoadResult m_mesh;
    cfd::CaseSettings m_settings;
    std::shared_ptr<const fvm::Mesh<2>> m_meshPtr;
    std::shared_ptr<fvm::VectorField<2>> m_velocity;
    std::shared_ptr<fvm::Field<2>> m_pressure;
    std::shared_ptr<fvm::SSTModel<2>> m_turb;
    std::shared_ptr<fvm::SIMPLE<2>> m_solver;
    mutable std::mutex m_solverMutex;
    std::vector<cfd::PatchBC> m_patchBCs;
    std::atomic<bool> m_cancel{false};
    std::atomic<double> m_lastTime{0.0}; // simulated time reached, for the summary
    QString m_meshPath;                  // dir used to resolve a relative .pvd
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