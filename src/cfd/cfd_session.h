#pragma once

#include "cfd/cfd_mesh_adapter.h"
#include "cfd/cfd_types.h"
#include "cfd/solution_state.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <array>

namespace cfd {

// Initial condition entered in the Initialize dialog.
struct InitConditions {
    double ux = 0.0, uy = 0.0, uz = 0.0;
    double pressure = 0.0;
    double k = 1e-3;
    double omega = 1.0;
};

// Dimension-free view of a solver's termination status.
enum class SessionStatus {
    Idle,
    NotConverged,
    Converged,
    Completed,
    Aborted,
    Cancelled
};

// Callbacks a run may publish.
//
// `iteration` is invoked from the solver thread for the residual table.
// `frame` is invoked from the solver thread once per transient time step with a
// complete snapshot (velocity, pressure and turbulence already copied), so the
// consumer never reaches back into the solver.
struct RunHooks {
    std::function<void(int, double, double)> iteration;
    std::function<void(const SolutionSnapshot&)> frame;
};

// A solver bound to one mesh, at one dimension.
//
// The controller owns a single ICfdSession and never sees SIMPLE<2> or
// SIMPLE<3>. Everything that needs the compile-time dimension lives behind
// this interface: field storage, the solver, restart payloads and the result
// exporter. The interface is virtual only at coarse boundaries (start/stop,
// capture, build, export) — never inside an iteration.
class ICfdSession {
public:
    virtual ~ICfdSession() = default;

    virtual int dim() const = 0;
    virtual std::size_t cells() const = 0;

    // Drops the solver and all state, keeping the mesh.
    virtual void clear() = 0;

    // Installs a loaded state. An empty snapshot clears the fields; a snapshot
    // whose dimension or cell count does not match the mesh is rejected.
    virtual bool setState(const SolutionSnapshot& state, std::string& error) = 0;

    // Zero/initial fields for the loaded mesh, returned as a snapshot.
    virtual SolutionSnapshot initialize(const InitConditions& ic,
                                        const CaseSettings& settings) = 0;

    // Prepares fields and a solver for a run. The `restart` snapshot (may be
    // empty) seeds the fields and the transient clock. Returns false without
    // changing the state when the run cannot be started (e.g. it is already at
    // the configured end time).
    virtual bool startRun(const CaseSettings& settings,
                          const std::vector<PatchBC>& bcs,
                          const SolutionSnapshot& restart,
                          const RunHooks& hooks,
                          std::string& error) = 0;

    // Blocking solve; called on the controller's worker future.
    virtual void solve() = 0;

    virtual void requestCancel() = 0;
    virtual void requestPause() = 0;
    virtual void requestResume() = 0;
    virtual void requestStep() = 0;
    virtual bool isPaused() const = 0;
    virtual bool isPausedSettled() const = 0;
    virtual int currentStep() const = 0;
    virtual double currentTime() const = 0;
    virtual SessionStatus status() const = 0;
    virtual int lastIterations() const = 0;
    virtual double finalContinuityResidual() const = 0;
    virtual std::array<double, 3> finalMomentumResiduals() const = 0;
    virtual double finalKResidual() const = 0;
    virtual double finalOmegaResidual() const = 0;
    virtual bool hasTurbulence() const = 0;

    // Live state (solver fields if one exists, otherwise the installed state).
    // Safe to call at a pause boundary or after the run finished; it must not
    // be called while the solver is actively writing.
    virtual SolutionSnapshot capture() const = 0;

    virtual std::shared_ptr<const RenderMesh> buildResult(
        const std::shared_ptr<const RenderMesh>& base,
        const SolutionSnapshot& state,
        const CaseSettings& settings,
        const std::vector<PatchBC>& bcs) const = 0;

    virtual void exportVtu(const std::string& path, const SolutionSnapshot& state) const = 0;
    virtual void exportTurbulence(const std::string& path, const SolutionSnapshot& state) const = 0;
    // ".vtu" for unstructured meshes, ".vti"/".vtr" for structured ones.
    virtual std::string outputExtension() const = 0;
};

// Runtime dispatch: mesh3 -> CfdSession<3>, mesh2 -> CfdSession<2>.
std::unique_ptr<ICfdSession> makeSession(const MeshLoadResult& mesh);

} // namespace cfd
