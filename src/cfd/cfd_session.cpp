#include "cfd/cfd_session.h"

#include "cfd/cfd_result_builder.h"
#include "cfd/field_utils.h"
#include "cfd/solver_config_bridge.h"

#include "field/Field.h"
#include "field/VectorField.h"
#include "fvm/SIMPLE.h"
#include "fvm/SSTModel.h"
#include "io/VTKXMLWriter.h"
#include "mesh/Mesh.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cmath>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace cfd {

namespace {

// Copy velocity + pressure from a snapshot into the live fields.
// (handled by cfd::field_utils::copyVelocityToVectorField / copyToField)

// Copy velocity + pressure from live fields into a snapshot.
// (handled by cfd::field_utils::copyVelocityFromVectorField / copyFromField)

template<std::size_t Dim>
class CfdSession final : public ICfdSession {
public:
    using Solver = fvm::SIMPLE<Dim>;
    using Velocity = fvm::VectorField<Dim>;
    using Scalar = fvm::Field<Dim>;

    explicit CfdSession(std::shared_ptr<const fvm::Mesh<Dim>> mesh)
        : m_mesh(std::move(mesh)) {}

    int dim() const override { return static_cast<int>(Dim); }
    std::size_t cells() const override { return m_mesh ? m_mesh->nCells() : 0; }

    void clear() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_solver.reset();
        m_solverMesh.reset();
        m_velocity.reset();
        m_pressure.reset();
        m_state.clear();
        m_transientRun = false;
    }

    bool setState(const SolutionSnapshot& state, std::string& error) override {
        if (state.dim != 0 && state.dim != static_cast<int>(Dim)) {
            error = "The saved solution is " + std::to_string(state.dim) +
                    "D but the mesh is " + std::to_string(Dim) + "D.";
            return false;
        }
        if (state.hasFields() && state.cells != m_mesh->nCells()) {
            error = "The saved fields do not match the mesh (" +
                    std::to_string(state.cells) + " cells in the file, " +
                    std::to_string(m_mesh->nCells()) + " in the mesh).";
            return false;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        m_solver.reset();
        m_solverMesh.reset();
        m_transientRun = false;
        m_velocity.reset();
        m_pressure.reset();
        if (state.hasFields()) seedFields(state);
        m_state = state.hasFields() ? state : SolutionSnapshot{};
        return true;
    }

    SolutionSnapshot initialize(const InitConditions& ic,
                                const CaseSettings& settings) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_solver.reset();
        m_solverMesh.reset();
        m_transientRun = false;

        m_velocity = std::make_shared<Velocity>(*m_mesh, 0.0);
        m_pressure = std::make_shared<Scalar>(*m_mesh, ic.pressure);
        m_velocity->assign(0, ic.ux);
        if constexpr (Dim >= 2) m_velocity->assign(1, ic.uy);
        if constexpr (Dim >= 3) m_velocity->assign(2, ic.uz);

        SolutionSnapshot state = snapshotFields();
        if (settings.turbulence) {
            typename fvm::SSTModel<Dim>::Config cfg{};
            double kInf = ic.k;
            double omegaInf = ic.omega;
            if (kInf <= 0.0 || omegaInf <= 0.0) {
                double U = std::sqrt(ic.ux*ic.ux + ic.uy*ic.uy + ic.uz*ic.uz);
                if (U < 1e-12) U = 1.0;
                const double I = 0.05; // 5% turbulence intensity
                kInf = 1.5 * I * I * U * U;
                if (kInf < cfg.kMin) kInf = cfg.kMin;
                const double nu = settings.nu;
                const double L = 0.07 * 1.0; // characteristic length scale
                omegaInf = std::sqrt(kInf) / (std::pow(0.09, 0.25) * std::max(L, 1e-6));
                if (omegaInf < cfg.omegaMin) omegaInf = cfg.omegaMin;
            }
            cfg.kInf = kInf;
            cfg.omegaInf = omegaInf;
            cfg.alphaK = settings.alphaK;
            cfg.alphaOmega = settings.alphaOmega;
            cfg.wallFunction = settings.wallFunction;
            fvm::SSTModel<Dim> model(*m_mesh, cfg);
            model.initialize(settings.nu * settings.rho, settings.rho);
            cfd::field_utils::fillTurbulence<Dim>(model, state.turbulence, state.cells);
        }
        m_state = state;
        return state;
    }

    bool startRun(const CaseSettings& settings,
                  const std::vector<PatchBC>& bcs,
                  const SolutionSnapshot& restart,
                  const RunHooks& hooks,
                  std::string& error) override {
        if (restart.dim != 0 && restart.dim != static_cast<int>(Dim)) {
            error = "The saved solution does not match the mesh dimension.";
            return false;
        }
        if (settings.transient.enabled) {
            if (!(settings.transient.tEnd > 0.0)) {
                error = "End time must be greater than zero.";
                return false;
            }
            if (restart.transientValid &&
                restart.time >= settings.transient.tEnd - 1e-12) {
                char buf[320];
                std::snprintf(buf, sizeof(buf),
                              "The saved state is already at t = %g s (end time %g s). "
                              "Raise the end time to continue the run.",
                              restart.time, settings.transient.tEnd);
                error = buf;
                return false;
            }
        }

        std::lock_guard<std::mutex> lock(m_mutex);

        // Continue from the supplied snapshot when it belongs to this mesh;
        // otherwise start from zero. Initialize is the explicit reset.
        if (restart.hasFields() && restart.cells == m_mesh->nCells()) {
            seedFields(restart);
        } else {
            m_velocity = std::make_shared<Velocity>(*m_mesh, 0.0);
            m_pressure = std::make_shared<Scalar>(*m_mesh, 0.0);
        }

        typename Solver::Config sc = toFvmConfig<Dim>(settings);
        if (settings.turbulence) {
            for (const auto& pbc : bcs) {
                if (pbc.type != BCType::VelocityInlet) continue;
                typename Solver::InletTurbulence it;
                it.patch = pbc.patchName;
                it.k = pbc.kInlet;
                it.omega = pbc.omegaInlet;
                sc.inletTurbulence.push_back(it);
            }
        }
        if (hooks.iteration) {
            sc.iterationCallback = [hooks](int iter, double cont, double mom) {
                hooks.iteration(iter, cont, mom);
            };
        }

        // The render callback copies velocity, pressure and turbulence into a
        // self-contained snapshot on the solver thread. Reading turbulence
        // through the atomic pointer set below means the consumer never has to
        // query the solver while it is running.
        auto turbSlot = std::make_shared<std::atomic<const fvm::SSTModel<Dim>*>>(nullptr);
        if (settings.transient.enabled && hooks.frame) {
            const std::size_t n = m_mesh->nCells();
            sc.renderCallback = [n, turbSlot, hooks](
                int step, double time, const Velocity& velocity, const Scalar& pressure) {
                SolutionSnapshot state;
                state.dim = static_cast<int>(Dim);
                state.cells = n;
                cfd::field_utils::copyVelocityFromVectorField<Dim>(velocity, state.velocity);
                cfd::field_utils::copyFromField<Dim>(pressure, state.pressure);

                if (const auto* turb = turbSlot->load(std::memory_order_acquire)) {
                    cfd::field_utils::fillTurbulence<Dim>(*turb, state.turbulence, state.cells);
                }
                state.transientValid = true;
                state.time = time;
                state.step = step;
                hooks.frame(state);
            };
        }

        auto solver = std::make_shared<Solver>(*m_mesh, settings.rho, settings.nu, sc);
        if (settings.turbulence) {
            solver->enableTurbulence(settings.kInf, settings.omegaInf,
                                     settings.alphaK, settings.alphaOmega,
                                     settings.wallFunction);
        }
        solver->bindVelocity(*m_velocity);
        solver->bindPressure(*m_pressure);

        configureBoundaryConditions(*solver, bcs);

        queueRestartState(*solver, restart, settings);

        *turbSlot = solver->turbulence();
        m_solver = std::move(solver);
        m_solverMesh = m_mesh;
        m_transientRun = settings.transient.enabled;
        m_state = restart;
        return true;
    }

    void solve() override {
        std::shared_ptr<Solver> solver;
        std::shared_ptr<Velocity> velocity;
        std::shared_ptr<Scalar> pressure;
        bool transient = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            solver = m_solver;
            velocity = m_velocity;
            pressure = m_pressure;
            transient = m_transientRun;
        }
        if (!solver || !velocity || !pressure) return;

        if (transient) {
            solver->solveTransient(*velocity, *pressure);
        } else {
            solver->solve(*velocity, *pressure);
        }
    }

    void requestCancel() override { withSolver([](Solver& s) { s.requestCancel(); }); }
    void requestPause() override { withSolver([](Solver& s) { s.requestPause(); }); }
    void requestResume() override { withSolver([](Solver& s) { s.requestResume(); }); }
    void requestStep() override { withSolver([](Solver& s) { s.requestStep(); }); }

    bool isPaused() const override {
        return withSolver([](const Solver& s) { return s.isPaused(); });
    }
    bool isPausedSettled() const override {
        return withSolver([](const Solver& s) { return s.isPauseSettled(); });
    }

    int currentStep() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_solver) return m_solver->currentStep();
        return m_state.transientValid ? m_state.step : 0;
    }

    double currentTime() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_solver) return m_solver->currentTime();
        return m_state.transientValid ? m_state.time : 0.0;
    }

    SessionStatus status() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_solver) return SessionStatus::Idle;
        switch (m_solver->status()) {
            case Solver::Status::NotConverged: return SessionStatus::NotConverged;
            case Solver::Status::Converged: return SessionStatus::Converged;
            case Solver::Status::Completed: return SessionStatus::Completed;
            case Solver::Status::Aborted: return SessionStatus::Aborted;
            case Solver::Status::Cancelled: return SessionStatus::Cancelled;
        }
        return SessionStatus::Idle;
    }

    int lastIterations() const override {
        return withSolver([](const Solver& s) { return s.lastIterations(); });
    }

    double finalContinuityResidual() const override {
        return withSolver([](const Solver& s) { return s.finalContinuityResidual(); });
    }
    std::array<double, 3> finalMomentumResiduals() const override {
        return withSolver([](const Solver& s) {
            auto r = s.finalMomentumResiduals();
            std::array<double, 3> out{};
            out[0] = r[0];
            if constexpr (Dim >= 2) out[1] = r[1];
            if constexpr (Dim >= 3) out[2] = r[2];
            return out;
        });
    }
    double finalKResidual() const override {
        return withSolver([](const Solver& s) {
            const auto* t = s.turbulence();
            return t ? t->finalKResid() : 0.0;
        });
    }
    double finalOmegaResidual() const override {
        return withSolver([](const Solver& s) {
            const auto* t = s.turbulence();
            return t ? t->finalOmegaResid() : 0.0;
        });
    }
    bool hasTurbulence() const override {
        return withSolver([](const Solver& s) { return s.turbulence() != nullptr; });
    }

    SolutionSnapshot capture() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_solver && m_solverMesh == m_mesh && m_velocity && m_pressure) {
            SolutionSnapshot state = snapshotFields();
            if (const auto* turb = m_solver->turbulence()) {
                cfd::field_utils::fillTurbulence<Dim>(*turb, state.turbulence, state.cells);
            }
            if (m_transientRun) {
                state.transientValid = m_solver->hasTransientState();
                state.time = m_solver->currentTime();
                state.dt = m_solver->currentDt();
                state.dtPrev = m_solver->currentDtPrev();
                state.step = m_solver->currentStep();
                if (state.transientValid) {
                    const auto& vnm1 = m_solver->velocityNM1();
                    bool complete = vnm1.size() == m_mesh->nCells();
                    if constexpr (Dim >= 2) {
                        complete = complete && vnm1[1].size() == m_mesh->nCells();
                    }
                    if constexpr (Dim >= 3) {
                        complete = complete && vnm1[2].size() == m_mesh->nCells();
                    }
                    if (complete) {
                        state.hasVelocityHistory = true;
                        state.velocityNM1.resize(Dim);
                        for (std::size_t d = 0; d < Dim; ++d) {
                            state.velocityNM1[d].assign(vnm1[d].values().begin(),
                                                       vnm1[d].values().begin() + m_mesh->nCells());
                        }
                    }
                    if (m_solver->turbulence()) {
                        const auto& kn = m_solver->kNM1();
                        const auto& on = m_solver->omegaNM1();
                        if (kn.size() == m_mesh->nCells() && on.size() == m_mesh->nCells()) {
                            state.turbulence.hasHistory = true;
                            state.turbulence.kNM1.assign(kn.values().begin(), kn.values().end());
                            state.turbulence.omegaNM1.assign(on.values().begin(), on.values().end());
                        }
                    }
                }
            }
            return state;
        }
        return m_state;
    }

    std::shared_ptr<const RenderMesh> buildResult(
        const std::shared_ptr<const RenderMesh>& base,
        const SolutionSnapshot& state,
        const CaseSettings& settings,
        const std::vector<PatchBC>& bcs) const override {
        if (!base || !state.hasFields() || state.cells != m_mesh->nCells()) return nullptr;
        return buildResultMesh<Dim>(base, *m_mesh, state, settings, bcs);
    }

    void exportVtu(const std::string& path, const SolutionSnapshot& state) const override {
        if (!state.hasFields() || state.cells != m_mesh->nCells()) return;

        Scalar pressure(*m_mesh, 0.0);
        cfd::field_utils::copyToField<Dim>(state.pressure, pressure);

        std::array<Scalar, Dim> velocity;
        std::array<const Scalar*, Dim> velocityPtrs{};
        for (std::size_t d = 0; d < Dim; ++d) {
            velocity[d] = Scalar(*m_mesh, 0.0);
            // copy state.velocity[d] into velocity[d]
            cfd::field_utils::copyToField<Dim>(state.velocity[d], velocity[d]);
            velocityPtrs[d] = &velocity[d];
        }

        Scalar kField, omegaField, nutField;
        const Scalar* kPtr = nullptr;
        const Scalar* omegaPtr = nullptr;
        const Scalar* nutPtr = nullptr;
        if (state.hasTurbulence()) {
            kField = Scalar(*m_mesh, 0.0);
            omegaField = Scalar(*m_mesh, 0.0);
            nutField = Scalar(*m_mesh, 0.0);
            cfd::field_utils::scatterTurbulence<Dim>(state.turbulence, kField, omegaField, nutField);
            kPtr = &kField;
            omegaPtr = &omegaField;
            nutPtr = &nutField;
        }

        fvm::VTKXMLWriter<Dim>::writeWithVelocity(path, *m_mesh, pressure, velocityPtrs,
                                                  "pressure", kPtr, omegaPtr, nutPtr);
    }

    void exportTurbulence(const std::string& path,
                          const SolutionSnapshot& state) const override {
        if (!state.hasTurbulence()) return;
        Scalar kField(*m_mesh, 0.0);
        Scalar omegaField(*m_mesh, 0.0);
        Scalar nutField(*m_mesh, 0.0);
        cfd::field_utils::scatterTurbulence<Dim>(state.turbulence, kField, omegaField, nutField);
        fvm::VTKXMLWriter<Dim>::writeTurbulence(path, *m_mesh, kField, omegaField, nutField);
    }

    std::string outputExtension() const override {
        if (m_mesh->isUnstructured()) return ".vtu";
        return m_mesh->isUniform() ? ".vti" : ".vtr";
    }

private:
    void seedFields(const SolutionSnapshot& state) {
        m_velocity = std::make_shared<Velocity>(*m_mesh, 0.0);
        m_pressure = std::make_shared<Scalar>(*m_mesh, 0.0);
        cfd::field_utils::copyVelocityToVectorField<Dim>(state.velocity, *m_velocity);
        cfd::field_utils::copyToField<Dim>(state.pressure, *m_pressure);
    }

    SolutionSnapshot snapshotFields() const {
        SolutionSnapshot state;
        state.dim = static_cast<int>(Dim);
        state.cells = m_mesh->nCells();
        cfd::field_utils::copyVelocityFromVectorField<Dim>(*m_velocity, state.velocity);
        cfd::field_utils::copyFromField<Dim>(*m_pressure, state.pressure);
        return state;
    }

    void configureBoundaryConditions(Solver& solver,
                                    const std::vector<PatchBC>& bcs) {
        for (const auto& pbc : bcs) {
            auto it = m_mesh->boundaryGroups().find(pbc.patchName);
            if (it == m_mesh->boundaryGroups().end()) continue;
            const std::vector<std::size_t>& faces = it->second;
            std::string typeStr;
            switch (pbc.type) {
                case BCType::VelocityInlet: typeStr = "inlet"; break;
                case BCType::PressureOutlet: typeStr = "pressure_outlet"; break;
                case BCType::Outlet: typeStr = "outlet"; break;
                case BCType::WallNoSlip: typeStr = "wall"; break;
                case BCType::SlipWall: typeStr = "slip_wall"; break;
                case BCType::Symmetry: typeStr = "symmetry"; break;
                case BCType::MovingWall: typeStr = "moving_wall"; break;
            }
            if (pbc.type == BCType::PressureOutlet) {
                solver.setPressureOutletOnFacesWithBackflow(
                    faces, pbc.pressure, pbc.suppressBackflow, pbc.backflowU,
                    pbc.backflowV, pbc.backflowW, pbc.backflowK, pbc.backflowOmega);
            } else {
                const bool slip = (pbc.type == BCType::SlipWall ||
                                   pbc.type == BCType::Symmetry);
                solver.setVelocityBCOnFacesWithSlip(faces, typeStr, pbc.u, pbc.v,
                                                   pbc.w, slip, pbc.pressure);
            }
        }
    }

    void queueRestartState(Solver& solver, const SolutionSnapshot& restart,
                           const CaseSettings& settings) {
        if (!(restart.hasFields() || restart.hasTurbulence() ||
              (settings.transient.enabled && restart.transientValid))) return;
        typename Solver::RestartState rs;
        if (restart.hasFields()) {
            rs.hasVelocity = true;
            for (std::size_t d = 0; d < Dim; ++d) rs.velocity[d] = restart.velocity[d];
            rs.pressure = restart.pressure;
        }
        if (restart.hasTurbulence()) {
            rs.hasTurbulence = true;
            rs.k = restart.turbulence.k;
            rs.omega = restart.turbulence.omega;
            rs.nut = restart.turbulence.nut;
        }
        if (settings.transient.enabled && restart.transientValid) {
            rs.hasTransient = true;
            rs.time = restart.time;
            rs.dt = restart.dt;
            rs.dtPrev = restart.dtPrev;
            rs.step = restart.step;
            if (restart.hasVelocityHistorySized()) {
                rs.hasVelocityHistory = true;
                for (std::size_t d = 0; d < Dim; ++d)
                    rs.velocityNM1[d] = restart.velocityNM1[d];
            }
            if (restart.hasTurbulenceHistory()) {
                rs.hasTurbulenceHistory = true;
                rs.kNM1 = restart.turbulence.kNM1;
                rs.omegaNM1 = restart.turbulence.omegaNM1;
            }
        }
        solver.setRestartState(std::move(rs));
    }

// fillTurbulence now lives in cfd::field_utils.

    // Calls `fn` with the live solver, holding the session mutex only long
    // enough to take a reference. Returns a value-initialised result when no
    // solver exists (false/0/0.0), which is what every caller expects.
    template<typename Fn>
    std::invoke_result_t<Fn, Solver&> withSolver(Fn&& fn) const {
        using Result = std::invoke_result_t<Fn, Solver&>;
        std::shared_ptr<Solver> solver;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            solver = m_solver;
        }
        if constexpr (std::is_void_v<Result>) {
            if (solver) fn(*solver);
        } else {
            if (!solver) return Result{};
            return fn(*solver);
        }
    }

    std::shared_ptr<const fvm::Mesh<Dim>> m_mesh;
    std::shared_ptr<Velocity> m_velocity;
    std::shared_ptr<Scalar> m_pressure;

    std::shared_ptr<Solver> m_solver;
    std::shared_ptr<const fvm::Mesh<Dim>> m_solverMesh;
    bool m_transientRun = false;

    SolutionSnapshot m_state;
    mutable std::mutex m_mutex;
};

}

std::unique_ptr<ICfdSession> makeSession(const MeshLoadResult& mesh) {
    if (mesh.mesh3) return std::make_unique<CfdSession<3>>(mesh.mesh3);
    if (mesh.mesh2) return std::make_unique<CfdSession<2>>(mesh.mesh2);
    return nullptr;
}

} // namespace cfd
