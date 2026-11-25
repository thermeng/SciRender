#pragma once

#include "cfd/cfd_types.h"
#include "cfd/convergence_policy.h"
#include "mesh/Mesh.h"
#include "mesh/BoundaryUtils.h"
#include "field/Field.h"
#include "field/VectorField.h"
#include "fvm/Cancellable.h"
#include "fvm/SIMPLETypes.h"
#include "fvm/BoundaryConditions.h"
#include "fvm/TransientDriver.h"
#include "fvm/Momentum.h"
#include "fvm/SSTModel.h"
#include "fvm/Gradient.h"
#include "fvm/PressureCorrection.h"
#include "fvm/LinearSolverDispatcher.h"
#include "fvm/Residual.h"
#include "fvm/VelocityCorrection.h"
#include "io/Checkpoint.h"

#include <Eigen/Sparse>
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fvm {

template<std::size_t Dim>
class SIMPLE;
template<std::size_t Dim>
class TransientDriver;

// Resident set size of this process in MiB (used by both the steady and the
// transient iteration tables).
double residentSetMiB();

// Shared context + machinery of the CFD solvers: the fields, the operators,
// and the residual/convergence/print bookkeeping. Both sides of the solver
// — the steady driver (SIMPLE) and the transient driver (TransientDriver) —
// operate on this base. New solver/scheme modules plug into the operators
// and into this context.
template<std::size_t Dim>
class CFDSolver : public Cancellable {
public:
    using TransientConfig = cfd::TransientSettings;

    struct InletTurbulence {
        std::string patch;          // patch name (legacy path)
        std::vector<std::size_t> faces; // face indices (face-index-based path)
        double k = 1e-3;
        double omega = 1.0;
    };

    struct Config : public cfd::CaseSettings {
        Config& operator=(const cfd::CaseSettings& s) {
            static_cast<cfd::CaseSettings&>(*this) = s;
            return *this;
        }
        // Freestream turbulence. Not part of CaseSettings: the session sets
        // these from Initialize(), and enableTurbulence() copies them in.
        double kInf = 1e-3;
        double omegaInf = 1.0;
        SolverType solverType = SolverType::SIMPLE;
        ConvectionScheme convectionScheme = ConvectionScheme::SecondOrderUpwind;
        GradientScheme gradientScheme = GradientScheme::LeastSquares;
        bool pressureUseAMGPreconditioner = true;
        // Override CaseSettings defaults where the solver needs different values:
        int pressureCorrectors = 1;           // standard steady SIMPLE; >1 is PIMPLE-style
        bool limitPressureCorrection = true;   // CaseSettings: false
        double pressureCorrectionLimit = 1e6;  // CaseSettings: 5.0
std::size_t pressureRefCell = SIZE_MAX; // CaseSettings: 0
    double turbulenceAmgMaxResidual = 1e-8; // CaseSettings: 0.1
        std::vector<InletTurbulence> inletTurbulence;

        std::function<void(int, double, double)> iterationCallback;
        std::function<void(int, double, const VectorField<Dim>&, const Field<Dim>&)> renderCallback;
        std::function<void(int, double, const VectorField<Dim>&, const Field<Dim>&)> transientCallback;
        CheckpointConfig checkpoint;
        TransientConfig transient;
        // Resolved stopping policy (populated by solver_config_bridge). The
        // iteration loop consults this instead of raw optional fields.
        cfd::ResolvedStoppingPolicy stoppingPolicy;
    };

    using VelocityBCType = ::fvm::VelocityBCType;
    using VelocityBC = ::fvm::VelocityBC;
    using VelocityPatch = ::fvm::VelocityPatch;
    using FaceVelocityBC = ::fvm::FaceVelocityBC;

    CFDSolver(const Mesh<Dim>& mesh, double rho, double nu, const Config& config = Config{});
    // Defined in CFDSolver.cpp: the transient driver member is incomplete here.
    virtual ~CFDSolver();

    // Why the run ended. Shared by both drivers: the transient side
    // finishes with Completed, the steady side with Converged.
    //
    // A run that reaches maxIterations without meeting the tolerance is NOT
    // a converged solution, and the difference has to be visible: the caller
    // (the application, a test) must be able to tell a steady state from a
    // field that merely stopped changing because the iteration budget ran
    // out. Before this existed every run reported "complete", so a diverged
    // or unconverged result was presented exactly like a converged one.
    enum class Status {
        NotConverged,  // the iteration budget ran out before the tolerance
        Converged,     // the tolerance was met (and held) before the budget
        Completed,     // a transient run reached its end time
        Aborted,       // NaN/Inf residual or a detected divergence
        Cancelled      // the user cancelled the run
    };
    // Why an aborted run stopped. `None` for every other status, so a caller
    // can tell "ran out of iterations" from "the numerics broke" without
    // scraping the log output.
    enum class Failure {
        None,
        NonFiniteContinuity,   // the continuity residual became NaN/Inf
        DivergingContinuity,   // the continuity residual grew for too long
        NonFiniteTurbulence,   // the k/omega transport produced NaN/Inf
        MomentumSolveFailed,   // a momentum linear solve broke down
        PressureSolveFailed    // a pressure linear solve broke down
    };
    Status status() const { return status_; }
    bool converged() const { return status_ == Status::Converged; }
    Failure failure() const { return failure_; }
    const char* statusText() const;
    const char* failureText() const;

    void bindVelocity(VectorField<Dim>& velocity);
    void bindPressure(Field<Dim>& pressure);
    VectorField<Dim>& boundVelocity();
    const VectorField<Dim>& boundVelocity() const;
    Field<Dim>& boundPressure();
    const Field<Dim>& boundPressure() const;

    SSTModel<Dim>* turbulence();
    const SSTModel<Dim>* turbulence() const;

    // Per-direction momentum linear residuals, in the same order as the
    // "x-velocity"/"y-velocity" columns of the iteration table. The summary
    // "momentum residual" is the max of these.
    std::array<double, Dim> finalMomentumResiduals() const {
        std::array<double, Dim> r{};
        r[0] = lastMomResidU_;
        if constexpr (Dim >= 2) r[1] = lastMomResidV_;
        if constexpr (Dim >= 3) r[2] = lastMomResidW_;
        return r;
    }

    // Momentum AMG diagnostics (for tests / profiling).
    std::size_t momentumAmgSolves() const { return linearSolverDispatcher_.amgSolves(); }
    std::size_t momentumAmgStandaloneSolves() const {
        return linearSolverDispatcher_.amgStandaloneSolves();
    }
    std::size_t momentumAmgBuilds() const { return linearSolverDispatcher_.amgBuilds(); }
    // Per-outer-iteration relative velocity change. Diagnostic only: it tracks
    // iteration progress rather than equation satisfaction.
    double finalMomentumChange() const { return lastMomChange_; }

    int lastIterations() const;
    bool updateStoppingPolicy(const cfd::ResolvedStoppingPolicy& policy);
    double finalContinuityResidual() const;
    double finalNormalizedContinuityResidual() const;
    double finalMassImbalance() const;
    double finalMomentumResidual() const;

    // Everything needed to resume a run from a saved case.
    struct RestartState {
        // Steady continuation: resume the iteration counter and residual state.
        bool hasSteady = false;
        int steadyIteration = 0;
        double steadyCont = 0.0;
        double steadyMom = 0.0;
        bool hasVelocity = false;
        std::array<std::vector<double>, Dim> velocity{};
        std::vector<double> pressure;
        bool hasTurbulence = false;
        std::vector<double> k;
        std::vector<double> omega;
        std::vector<double> nut;
        bool hasTransient = false;
        double time = 0.0;
        double dt = 0.0;
        double dtPrev = 0.0;
        int step = 0;
        bool hasVelocityHistory = false;
        std::array<std::vector<double>, Dim> velocityNM1{};
        bool hasTurbulenceHistory = false;
        std::vector<double> kNM1;
        std::vector<double> omegaNM1;
    };
    void setRestartState(RestartState state) { restartState_ = std::move(state); }
    bool hasRestartState() const { return restartState_.has_value(); }

    // The transient-side driver: SIMPLE::solveTransient and the SIMPLE
    // transient read accessors go through it.
    std::unique_ptr<TransientDriver<Dim>> transientDriver_;
    // Set when a RestartState with transient state was applied by
    // initializeSolve_ (drives SIMPLE::hasTransientState()).
    bool hasRestoredTransientState_ = false;

private:
    friend class SIMPLE<Dim>;
    friend class TransientDriver<Dim>;

    // Exposes the Cancellable flag so the drivers pass it to the
    // linear-solver calls without friendship reaching into the base class.
    std::atomic<bool>* cancelFlag() { return &cancelRequested_; }

    const Field<Dim>& zeroSource_() const;

    // The compiled boundary view is owned by the BC spec; callers never cache it.
    const BCDiscretization& bcDiscretization() const { return bc_.discretization(); }

    // Under-relax the current turbulence viscosity toward the supplied previous field.
    void relaxNut_(const Field<Dim>& previous, double alpha = nutUnderRelax);

    void initializeSolve_(VectorField<Dim>& velocity, Field<Dim>& pressure);
    // Steady loop internals (now shared by the base since the loop mechanics
    // are identical for both sides).
    void performIteration_(VectorField<Dim>& velocity, Field<Dim>& pressure, std::size_t iter);
    // One momentum solve for steady and transient callers alike (TransientParams).
    void solveMomentumComponent_(std::size_t d, VectorField<Dim>& uStar,
                                 const VectorField<Dim>& velocityOld,
                                 const std::array<Field<Dim>, Dim>& pressureGrad,
                                 const std::vector<double>* faceVolFlux = nullptr,
                                 const TransientParams<Dim>& tp = TransientParams<Dim>{});
    void applyStoppingPolicy_();
    bool isPiso_() const;
    bool aborted() const { return aborted_; }
    void fail_(Failure reason);
    void saveCheckpoint_(std::size_t iter);
    void saveCheckpoint_(std::size_t iter, const TransientBlock<Dim>* transient);

    // Shared pressure-correction core: solve p', optionally apply the p'
    // limiter, snapshot the diag for residual diagnostics, correct u*, and
    // add p' to the pressure with under-relaxation alphaP. The steady loop,
    // the transient iteration, and the PISO corrector all call this — one
    // place for pressure-correction bug fixes.
    void applyPressureCorrection_(VectorField<Dim>& uStar, double alphaP,
                                  bool applyLimiter, bool capturePreCorrector,
                                  const std::vector<double>* faceVolFlux = nullptr);
    void updateResiduals_();
    // Capture the mass imbalance of the momentum predictor u* (the field the
    // pressure correction is about to project).
    void capturePredictedContinuity_();
    // Provisional Rhie-Chow face volume flux (u_f* . S_f) for the current
    // outer iteration. The momentum assembly, the pressure-correction RHS and
    // the continuity residual all consume this same array. Empty/unused on the
    // transient path until Phase 1.5.
    // `velocity` defaults to the bound field; pass `&uStar_` after the momentum
    // predictor so the correction RHS sees the divergence of the fresh u*.
    void computeProvisionalFaceFlux_(const VectorField<Dim>* velocity = nullptr);
    // The pressure force is sum_f p_f A_f n with face pressures
    // interpolated with 1/a_P (= dCoeff) weights. This single operator feeds
    // momentum, Rhie-Chow and the cell-velocity correction.
    void computeFaceBasedPressureGradient_(const Field<Dim>& p,
                                           std::array<Field<Dim>, Dim>& grad) const;
    // Apply the pressure-correction flux to the stored face flux.
    void correctFaceFluxWithPressure_(const Field<Dim>& pPrime);
    // Normalised linear residual |A u - b| / sum|b| of the momentum system
    // for component d, evaluated at the current velocity.
    double momentumResidual_(std::size_t d) const;
    // Returns true when the caller should stop solving.
    bool checkConvergence_(std::size_t iter, bool checkTolerance = true,
                           bool checkCancel = true);

    void printHeader_(bool isTransient) const;
    void printIterationRow_(bool isTransient, int iter, double time, double dt);
    void printFinalStats_(bool isTransient) const;
    void printPressureSolverStats_() const;
    mutable int headerCounter_ = 0; // for repeating header every N lines

    // Scale p' down if it exceeds limit * rho * U_ref^2.
    bool limitPressureCorrection_(Field<Dim>& pPrime);

    void applyPressureSolverConfig_();
    void updateMaxInletSpeed_();

    // --- shared context ---
    const Mesh<Dim>& mesh_;
    double rho_, nu_;
    Config config_;
    // Gradient instances declared before pressureCorrection_ so they are
    // destroyed AFTER it (reverse declaration order).
    std::unique_ptr<Gradient<Dim>> gradientCalc_;
    std::unique_ptr<Gradient<Dim>> pressureGradientCalc_;

    Momentum<Dim> momentum_;
    PressureCorrection<Dim> pressureCorrection_;

    Field<Dim> pPrime_;
    Field<Dim> divBuffer_;
    Field<Dim> pressureBeforeCorrection_;
    std::array<Field<Dim>, Dim> pressureGradBefore_{};
    VectorField<Dim> uStar_;
    std::array<Field<Dim>, Dim> dpdx_{};
    std::array<Field<Dim>, Dim> diag_{};
    std::array<std::vector<double>, Dim> dCoeff_;
    // Provisional Rhie-Chow face volume flux, size mesh_.nFaces().
    std::vector<double> faceVolFlux_;
    // True on the steady path once computeProvisionalFaceFlux_ has run. The
    // transient path leaves it false and keeps the old divergence.
    bool useProvisionalFlux_ = false;

    std::optional<std::reference_wrapper<VectorField<Dim>>> velocity_;
    std::optional<std::reference_wrapper<Field<Dim>>> pressure_;

    BoundaryConditions<Dim> bc_;

    mutable std::array<SparseMatrix, Dim> momAsnap_;
    mutable std::array<Eigen::VectorXd, Dim> momBsnap_;
    mutable Eigen::VectorXd momentumWarmStart_;
    std::array<Eigen::VectorXd, Dim> momB0_;
    mutable Field<Dim> momDiag_;
    mutable std::array<Field<Dim>, Dim> velGrad_;
    mutable std::array<Field<Dim>, Dim> pressureGrad_;
    // Diagnosis is emitted once per run, not on every iteration row.
    bool amgDiagnosisPrinted_ = false;

    LinearSolverDispatcher<Dim> linearSolverDispatcher_;

    VectorField<Dim> velocityOld_;
    Field<Dim> nutOld_;
    std::unique_ptr<SSTModel<Dim>> turbulence_;

    // --- residual bookkeeping shared by both drivers ---
    double lastContResid_ = 0.0;
    double lastContResidRaw_ = 0.0;
    double lastContResidPost_ = 0.0;
    double predictedContinuity_ = 0.0;
    // Continuity imbalance the current PISO corrector is about to remove
    // (u~ before the pressure solve). Written by TransientDriver; read by
    // updateResiduals_().
    double lastPisoPreCorrectorResidual_ = 0.0;
    double contResidRef_ = 0.0;
    std::size_t contResidRefSamples_ = 0;
    std::array<double, Dim> momResidRaw_{};
    std::array<double, Dim> momResidRef_{};
    std::size_t momResidRefSamples_ = 0;
    double lastMomResid_ = 0.0;
    double lastMomResidU_ = 0.0;
    double lastMomResidV_ = 0.0;
    double lastMomResidW_ = 0.0;
    double kResidRef_ = 0.0;
    double lastKResid_ = 0.0;
    double omegaResidRef_ = 0.0;
    double lastOmegaResid_ = 0.0;
    std::size_t turbResidRefSamples_ = 0;
    double lastMomChange_ = 0.0;
    double lastMomChangeU_ = 0.0;
    double lastMomChangeV_ = 0.0;
    double lastMomChangeW_ = 0.0;
    int lastIterations_ = 0;
    std::size_t divergenceCount_ = 0;
    double initialMassImbalance_ = 0.0;
    bool initialMassImbalanceSet_ = false;
    std::vector<double> residualHistory_;

    // Why the run ended (see Status).
    Status status_ = Status::NotConverged;
    bool aborted_ = false;
    // First hard failure of the run; see Failure.
    Failure failure_ = Failure::None;

    double maxInletSpeed_ = 0.0;
    // Set when the p' magnitude limiter had to clip, so the run can be flagged.
    std::size_t pPrimeLimiterHits_ = 0;

    mutable Field<Dim> zeroSourceCache_;
    mutable bool zeroSourceInitialized_ = false;

    // --- transient-side state ---
    bool restarting_ = false;
    bool hasVelocityHistory_ = false;
    // Restart payload queued by setRestartState(), consumed by initializeSolve_.
    std::optional<RestartState> restartState_;
    // Run bookkeeping shared by the drivers: the iteration the run started
    // at, and the current time-level counter advanced by the transient
    // driver.
    std::size_t startIteration_ = 0;
    int timeStep_ = 0;

    // --- steady-side members (used by the steady driver / shared machinery) ---
    std::unique_ptr<cfd::ConvergenceMonitor> convergenceMonitor_;
    // Steady-path wall-time accumulators (seconds).
    double steadyTotalSeconds_ = 0.0;
    double steadyMomentumSeconds_ = 0.0;
    double steadyMomentumAssemblySeconds_ = 0.0;
    double steadyPredictedContinuitySeconds_ = 0.0;
    double steadyPressureSeconds_ = 0.0;
    double steadyResidualSeconds_ = 0.0;
    // Turbulence (k + omega transport) is not part of any other timer; without
    // this the steady breakdown leaves the largest phase unaccounted for.
    double steadyTurbulenceSeconds_ = 0.0;
    // Wall-clock time at the start of the current outer iteration.
    std::chrono::steady_clock::time_point m_iterStart;
};

extern template class CFDSolver<2>;
extern template class CFDSolver<3>;

} // namespace fvm
