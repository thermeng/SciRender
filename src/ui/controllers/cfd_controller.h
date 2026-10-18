#pragma once
#include <QObject>
#include <QString>
#include <memory>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <functional>

#include "core/mesh_loader.h"
#include "cfd/cfd_types.h"
#include "cfd/cfd_mesh_adapter.h"
#include "cfd/cfd_types.h"

namespace fvm {
template<std::size_t Dim> class Mesh;
template<std::size_t Dim> class SIMPLE;
template<std::size_t Dim> class VectorField;
template<std::size_t Dim> class Field;
template<std::size_t Dim> class SSTModel;
}

namespace cfd { class CaseSettings; }

class CfdController : public QObject {
    Q_OBJECT
public:
    explicit CfdController(QObject* parent=nullptr);
    ~CfdController();

    bool loadMesh(const QString& path);
    const cfd::MeshLoadResult& meshResult() const { return m_mesh; }
    void setSettings(const cfd::CaseSettings& s) { m_settings = s; }
    void setPatchBCs(const std::vector<cfd::PatchBC>& bcs) { m_patchBCs = bcs; }
    void run();
    void cancel();
    // Zero the velocity/pressure fields and return the freshly initialized
    // result mesh so the user can confirm a clean start before re-running.
    // Returns nullptr when no mesh is loaded. Safe to call between runs; do
    // not call while a solve is in flight.
    std::shared_ptr<const RenderMesh> initialize();
    void exportVtu(const QString& path);
    std::shared_ptr<const RenderMesh> resultMesh() const;

signals:
    void meshLoaded();
    void progress(int iteration, double continuity, double momentum);
    // One frame per time step of a transient run, published from the solver's
    // worker thread. Deliberately independent of the on-disk write cadence so
    // the viewport keeps up even when "write interval" is large. The receiving
    // connection is queued, so the payload must be immutable — which is why it
    // is a shared_ptr<const RenderMesh>.
    void frameReady(std::shared_ptr<const RenderMesh> mesh, int step, double time);
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
    std::mutex m_solverMutex;
    std::vector<cfd::PatchBC> m_patchBCs;
    std::atomic<bool> m_cancel{false};
    std::atomic<double> m_lastTime{0.0}; // simulated time reached, for the summary
    QString m_meshPath;                  // dir used to resolve a relative .pvd
    uint64_t m_gen = 0;
};