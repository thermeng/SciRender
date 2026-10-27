#include "cfd/cfd_session.h"

#include "cfd/cfd_result_builder.h"
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
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace cfd {

namespace {

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
            cfg.kInf = ic.k;
            cfg.omegaInf = ic.omega;
            cfg.alphaK = settings.alphaK;
            cfg.alphaOmega = settings.alphaOmega;
            fvm::SSTModel<Dim> model(*m_mesh, cfg);
            model.initialize(settings.nu * settings.rho, settings.rho);
            fillTurbulence(state, model);
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
                state.velocity.resize(Dim);
                for (std::size_t d = 0; d < Dim; ++d) {
                    const auto values = velocity[d].values();
                    state.velocity[d].assign(values.begin(), values.end());
                }
                state.pressure.assign(pressure.values().begin(), pressure.values().end());

                if (const auto* turb = turbSlot->load(std::memory_order_acquire)) {
                    fillTurbulence(state, *turb);
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
                                     settings.alphaK, settings.alphaOmega);
        }
        solver->bindVelocity(*m_velocity);
        solver->bindPressure(*m_pressure);

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
                solver->setPressureOutletOnFacesWithBackflow(
                    faces, pbc.pressure, pbc.suppressBackflow, pbc.backflowU,
                    pbc.backflowV, pbc.backflowW, pbc.backflowK, pbc.backflowOmega);
            } else {
                const bool slip = (pbc.type == BCType::SlipWall ||
                                   pbc.type == BCType::Symmetry);
                solver->setVelocityBCOnFacesWithSlip(faces, typeStr, pbc.u, pbc.v,
                                                     pbc.w, slip, pbc.pressure);
            }
        }

        // Queue the restart payload. initializeSolve_ applies it after the
        // turbulence model exists, so restored k/omega/nut survive.
        if (restart.hasFields() || restart.hasTurbulence() ||
            (settings.transient.enabled && restart.transientValid)) {
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
            solver->setRestartState(std::move(rs));
        }

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
                fillTurbulence(state, *turb);
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
                            state.velocityNM1[d].resize(m_mesh->nCells());
                            for (std::size_t i = 0; i < m_mesh->nCells(); ++i)
                                state.velocityNM1[d][i] = vnm1[d][i];
                        }
                    }
                    if (m_solver->turbulence()) {
                        const auto& kn = m_solver->kNM1();
                        const auto& on = m_solver->omegaNM1();
                        if (kn.size() == m_mesh->nCells() && on.size() == m_mesh->nCells()) {
                            state.turbulence.hasHistory = true;
                            state.turbulence.kNM1.resize(m_mesh->nCells());
                            state.turbulence.omegaNM1.resize(m_mesh->nCells());
                            for (std::size_t i = 0; i < m_mesh->nCells(); ++i) {
                                state.turbulence.kNM1[i] = kn[i];
                                state.turbulence.omegaNM1[i] = on[i];
                            }
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
        for (std::size_t i = 0; i < state.cells; ++i) pressure[i] = state.pressure[i];

        std::array<Scalar, Dim> velocity;
        std::array<const Scalar*, Dim> velocityPtrs{};
        for (std::size_t d = 0; d < Dim; ++d) {
            velocity[d] = Scalar(*m_mesh, 0.0);
            for (std::size_t i = 0; i < state.cells; ++i)
                velocity[d][i] = state.velocity[d][i];
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
            for (std::size_t i = 0; i < state.cells; ++i) {
                kField[i] = state.turbulence.k[i];
                omegaField[i] = state.turbulence.omega[i];
                nutField[i] = state.turbulence.nut[i];
            }
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
        for (std::size_t i = 0; i < state.cells; ++i) {
            kField[i] = state.turbulence.k[i];
            omegaField[i] = state.turbulence.omega[i];
            nutField[i] = state.turbulence.nut[i];
        }
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
        for (std::size_t d = 0; d < Dim; ++d) {
            const auto& source = state.velocity[d];
            auto& target = (*m_velocity)[d];
            for (std::size_t i = 0; i < state.cells && i < target.size(); ++i)
                target[i] = source[i];
        }
        for (std::size_t i = 0; i < state.cells && i < m_pressure->size(); ++i)
            (*m_pressure)[i] = state.pressure[i];
    }

    SolutionSnapshot snapshotFields() const {
        SolutionSnapshot state;
        state.dim = static_cast<int>(Dim);
        state.cells = m_mesh->nCells();
        state.velocity.resize(Dim);
        for (std::size_t d = 0; d < Dim; ++d) {
            const auto values = (*m_velocity)[d].values();
            state.velocity[d].assign(values.begin(), values.end());
        }
        state.pressure.assign(m_pressure->values().begin(), m_pressure->values().end());
        return state;
    }

    static void fillTurbulence(SolutionSnapshot& state,
                               const fvm::SSTModel<Dim>& turbulence) {
        const std::size_t n = state.cells;
        state.turbulence.valid = true;
        state.turbulence.k.resize(n);
        state.turbulence.omega.resize(n);
        state.turbulence.nut.resize(n);
        const auto& k = turbulence.k();
        const auto& omega = turbulence.omega();
        const auto& nut = turbulence.nut();
        for (std::size_t i = 0; i < n; ++i) {
            state.turbulence.k[i] = k[i];
            state.turbulence.omega[i] = omega[i];
            state.turbulence.nut[i] = nut[i];
        }
    }

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

} // namespace

std::unique_ptr<ICfdSession> makeSession(const MeshLoadResult& mesh) {
    if (mesh.mesh3) return std::make_unique<CfdSession<3>>(mesh.mesh3);
    if (mesh.mesh2) return std::make_unique<CfdSession<2>>(mesh.mesh2);
    return nullptr;
}

} // namespace cfd
