#pragma once

#include "mesh/Mesh.h"
#include "mesh/BoundaryUtils.h"
#include "field/Field.h"
#include "field/VectorField.h"
#include "fvm/Types.h"
#include "fvm/SIMPLETypes.h"
#include "fvm/SIMPLEBoundaryConditions.h"
#include "fvm/Momentum.h"
#include "fvm/Divergence.h"
#include "fvm/SSTModel.h"
#include "fvm/Gradient.h"
#include "fvm/PressureCorrection.h"
#include "fvm/AMG.h"
#include "fvm/Residual.h"
#include "fvm/VelocityCorrection.h"
#include "io/Checkpoint.h"


#include <Eigen/Sparse>
#include <Eigen/IterativeLinearSolvers>
#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <optional>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>

namespace fvm {

template<std::size_t Dim>
class SIMPLE {
public:
    struct TransientConfig {
        bool enabled = false;
        double dt = 0.001;
        double tEnd = 1.0;
        TimeScheme scheme = TimeScheme::Euler;
        bool adaptive = false;
        double cfl = 0.5;
        double maxDt = 0.01;
        double minDt = 1e-6;
        int writeInterval = 1;
        int maxOuterCorrectors = 3;
        std::string pvdFile = "";
        bool piso = false; // DEPRECATED legacy: use Config::solverType==PISO instead. Kept for compat only - will be removed
    };

    struct Config {
        SolverType solverType = SolverType::SIMPLE; // explicit: SIMPLE (steady/transient), PISO, PIMPLE
        std::size_t maxIterations = 100;
        double tolerance = 1e-3;
        // Ignore the residual tolerance and run exactly maxIterations iterations.
        bool runFixedIterations = false;
        // Consecutive iterations the residual criterion must hold before the run
        // is declared converged.
        int convergenceHoldIterations = 3;
        double alphaP = 0.3;
        double alphaU = 0.7;
        std::size_t pressureRefCell = SIZE_MAX;
        ConvectionScheme convectionScheme = ConvectionScheme::Upwind;
        // Gradient operator used for the convective reconstruction.
        GradientScheme gradientScheme = GradientScheme::LeastSquares;
        bool rhieChow = true;
        bool simplec = false; // kept as requested: SIMPLE vs SIMPLEC via this flag
        // Number of pressure correctors per SIMPLE iteration (>=1).
        int pressureCorrectors = 3;
        // Bound on |p'| as a multiple of rho*U_ref^2. See limitPressureCorrection_.
        bool limitPressureCorrection = true;
        double pressureCorrectionLimit = 5.0;
        std::size_t printFrequency = 1;
        std::size_t residualHistoryWindow = 20;
        bool turbulenceEnabled = false;
        double kInf = 1e-3;
        double omegaInf = 1.0;
        double alphaK = 0.7;
        double alphaOmega = 0.7;

        // --- Pressure-correction linear solver ---------------------------
        // The pressure Poisson system is symmetric, so AMG is a good fit. It
        // is only built once the mesh exceeds pressureAmgThreshold cells;
        // below that CG+IC / BiCGSTAB+ILU are cheaper.
        bool pressureUseAMG = true;
        // true  -> AMG used as a preconditioner inside BiCGSTAB.
        // false -> a standalone AMG solve (then pressureAmgConfig's
        //          maxIterations/tolerance below decide convergence).
        bool pressureUseAMGPreconditioner = true;
        std::size_t pressureAmgThreshold = 10000;
        AMGHierarchy::Config pressureAmgConfig = []{
            AMGHierarchy::Config c;
            c.interpolationType = AMGHierarchy::InterpolationType::Direct;
            c.coarseningType = AMGHierarchy::CoarseningType::RS;
            c.cycleType = AMGHierarchy::CycleType::V;
            c.strengthThreshold = 0.25;
            c.truncThreshold = 0.2;
            c.preSweeps = 2;
            c.postSweeps = 2;
            c.maxIterations = 100;
            c.tolerance = 1e-8;
            return c;
        }();

        // --- Momentum linear solver -------------------------------------
        // The momentum (convection-diffusion) matrix is nonsymmetric and, for
        // upwind/SOU/TVD, an M-matrix. It is default-solved with
        // BiCGSTAB + IncompleteLUT. Optionally an AMG-preconditioned solve is
        // used instead, which scales far better on large meshes but pays an
        // O(nnz) setup per rebuild.
        //
        // Disabled by default: below the threshold ILU wins, and on steady
        // problems the hierarchy is built once so the cost is amortised, but
        // on transient problems the convective coefficients change every time
        // step. Measure before enabling (simple.momentum_solver_stats).
        bool useAMGMomentum = false;
        std::size_t momentumAmgThreshold = 20000;
        // How many momentum solves may reuse the same AMG hierarchy before it
        // is rebuilt. The coarse levels depend on the momentum matrix through
        // the Galerkin product and the matrix changes every solve (convective
        // flux, pressure gradient, under-relaxation, rho*V/dt), so the
        // hierarchy is technically stale after every solve - but rebuilding it
        // every time costs more than the solve it accelerates.
        std::size_t momentumAmgRebuildInterval = 5;
        // Number of AMG cycles per preconditioner application. >1 makes the
        // preconditioner stronger at linear cost.
        int momentumAmgCycles = 1;
        // Relative residual above which the AMG result is rejected and the
        // solve is redone with ILU. Guards against the BiCGSTAB breakdown
        // anomaly, which can otherwise return a silently wrong answer.
        // Must be at least as tight as the ILU tolerance (1e-8): a loose
        // momentum solve compounds through the SIMPLE outer loop.
        double momentumAmgMaxResidual = 1e-8;
        // Emit per-run momentum linear-solver statistics.
        bool momentumSolverStats = false;
        // Full multigrid configuration for the momentum hierarchy. Defaults
        // keep the forward-only Gauss-Seidel smoother the nonsymmetric operator
        // requires; see AMGHierarchy::Config for the individual knobs.
        AMGHierarchy::Config momentumAmgConfig = []{
            AMGHierarchy::Config c;
            c.interpolationType = AMGHierarchy::InterpolationType::Direct;
            c.coarseningType = AMGHierarchy::CoarseningType::RS;
            c.cycleType = AMGHierarchy::CycleType::V;
            c.strengthThreshold = 0.25;
            c.truncThreshold = 0.2;
            c.preSweeps = 1;
            c.postSweeps = 1;
            // Momentum operator is nonsymmetric: forward-only sweep.
            c.smootherType = AMGHierarchy::SmootherType::ForwardGaussSeidel;
            return c;
        }();

        std::function<void(int, double, double)> iterationCallback;
        std::function<void(int, double, const VectorField<Dim>&, const Field<Dim>&)> transientCallback;
        CheckpointConfig checkpoint;
        TransientConfig transient;
    };

    using VelocityBCType = ::fvm::VelocityBCType;
    using VelocityBC = ::fvm::VelocityBC;
    using VelocityPatch = ::fvm::VelocityPatch;
    using FaceVelocityBC = ::fvm::FaceVelocityBC;

    void setVelocityBCOnFacesWithSlip(const std::vector<std::size_t>& faceIndices,
                                      const std::string& typeStr,
                                      double u, double v, double w,
                                      bool slip,
                                      double pressure = 0.0);

    SIMPLE(const Mesh<Dim>& mesh, double rho, double nu, const Config& config = Config{});

    void setVelocityBC(std::size_t d, int sign, VelocityBCType type,
                       double val1 = 0.0, double val2 = 0.0, double val3 = 0.0);
    void setMin(std::size_t d, VelocityBCType type, double u = 0.0, double v = 0.0, double w = 0.0);
    void setMax(std::size_t d, VelocityBCType type, double u = 0.0, double v = 0.0, double w = 0.0);

    void addVelocityPatch(const std::string& name, std::size_t d, int sign,
                          VelocityBCType type, double u = 0.0, double v = 0.0, double w = 0.0);

    const VelocityPatch* findVelocityPatch(const std::string& name) const;
    const std::vector<VelocityPatch>& velocityPatches() const;

    void setVelocityBCOnFaces(const std::vector<std::size_t>& faceIndices,
                               VelocityBCType type,
                               double u = 0.0, double v = 0.0, double w = 0.0);

    void setPressureOutlet(std::size_t d, int sign, double pressure);
    void setPressureOutletOnFaces(const std::vector<std::size_t>& faceIndices, double pressure);
    void setPressureOutletOnFacesWithBackflow(const std::vector<std::size_t>& faceIndices, double pressure,
                                              bool suppressBackflow, double u, double v, double w,
                                              double k, double omega);

    void enableTurbulence(double kInf = 1e-3, double omegaInf = 1.0,
                          double alphaK = 0.7, double alphaOmega = 0.7);

    void solve(VectorField<Dim>& velocity, Field<Dim>& pressure);
    void solveTransient(VectorField<Dim>& velocity, Field<Dim>& pressure);

    // Why the run ended.
    //
    // A run that reaches maxIterations without meeting the tolerance is NOT a
    // converged solution, and the difference has to be visible: the caller
    // (the application, a test) must be able to tell a steady state from a
    // field that merely stopped changing because the iteration budget ran
    // out. Before this existed every run reported "complete", so a diverged
    // or unconverged result was presented exactly like a converged one.
    enum class Status {
        NotConverged,  // the iteration budget ran out before the tolerance
        Converged,     // the tolerance was met (and held) before the budget
        Aborted,       // NaN/Inf residual or a detected divergence
        Cancelled      // the user cancelled the run
    };
    Status status() const { return status_; }
    bool converged() const { return status_ == Status::Converged; }
    // A short human-readable description of the status, for the UI and the
    // solver's own summary.
    const char* statusText() const;

    void bindVelocity(VectorField<Dim>& velocity);
    void bindPressure(Field<Dim>& pressure);
    VectorField<Dim>& boundVelocity();
    const VectorField<Dim>& boundVelocity() const;
    Field<Dim>& boundPressure();
    const Field<Dim>& boundPressure() const;

    const Field<Dim>& pressure() const;
    double currentTime() const { return time_; }
    double currentDt() const { return dt_; }
    int currentStep() const { return timeStep_; }
    int lastIterations() const;
    double finalContinuityResidual() const;
    double finalMomentumResidual() const;
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
    // Per-outer-iteration relative velocity change. Diagnostic only: it tracks
    // iteration progress rather than equation satisfaction.
    double finalMomentumChange() const { return lastMomChange_; }
    SSTModel<Dim>* turbulence();
    const SSTModel<Dim>* turbulence() const;

    void requestCancel();
    void resetCancel();
    bool isCancelRequested() const;

private:
    const Field<Dim>& zeroSource_() const;

    void solveMomentum_(VectorField<Dim>& uStar, const VectorField<Dim>& velocityOld);
    void solveMomentumComponent_(std::size_t d, VectorField<Dim>& uStar,
                                 const VectorField<Dim>& velocityOld,
                                 const std::array<Field<Dim>, Dim>& pressureGrad);
    void solveMomentumTransient_(VectorField<Dim>& uStar, const VectorField<Dim>& velocityOld);
    void solveMomentumComponentTransient_(std::size_t d, VectorField<Dim>& uStar,
                                         const VectorField<Dim>& velocityOld,
                                         const std::array<Field<Dim>, Dim>& pressureGrad);

    void initializeSolve_(VectorField<Dim>& velocity, Field<Dim>& pressure);
    // Rebuild the pressure-correction linear-solver configuration from
    // Config. Called at construction and again whenever the transient path
    // forces Rhie-Chow on, so the two sites cannot drift apart.
    void applyPressureSolverConfig_();
    void performIteration_(VectorField<Dim>& velocity, Field<Dim>& pressure, std::size_t iter);
    void performTransientIteration_(VectorField<Dim>& velocity, Field<Dim>& pressure);
    // Fluent-like PISO: predictor + frozen-A correctors (robust)
    void performPISOPredictor_(VectorField<Dim>& velocity, Field<Dim>& pressure);
    void performPISOCorrector_(VectorField<Dim>& velocity, Field<Dim>& pressure);
    void updateResiduals_();
    // Normalised linear residual |A u - b| / sum|b| of the momentum system
    // for component d, evaluated at the current velocity. Uses the
    // per-direction snapshot, not the shared (last-component) momA_/momB_.
    double momentumResidual_(std::size_t d) const;
    // Solve momA_ x = momB_ for velocity component d, dispatching between
    // BiCGSTAB+ILU (default) and AMG-preconditioned BiCGSTAB. Collects solver
    // statistics. Returns the solution vector.
    Eigen::VectorXd solveMomentumLinear_(std::size_t d);
    // Cheap O(nnz) hash of the sparsity structure, used to decide whether an
    // AMG hierarchy can be reused or must be rebuilt.
    static std::size_t sparsityHash_(const SparseMatrix& A);
    // True if any off-diagonal entry is positive, i.e. the matrix is not an
    // M-matrix. AMG interpolation weights go negative in that case and the
    // coarse operator loses diagonal dominance, so we refuse to use it.
    static bool hasPositiveOffDiagonals_(const SparseMatrix& A);
    void printMomentumSolverStats_() const;
    // Solve with ILU. Used as the default and as the fallback when the
    // AMG-preconditioned solve produces an unacceptable residual.
    Eigen::VectorXd solveMomentumILU_(std::size_t d, std::size_t& itersOut);
    // Returns true when the caller should stop solving. `checkTolerance` gates
    // the "residuals are below tolerance" clause: the steady path wants it, the
    // transient path does not — a converged corrector loop at one time step
    // says nothing about whether the transient simulation is finished, so a
    // transient run must keep marching to tEnd (NaN/divergence and Cancel still
    // stop it in both modes).
    bool checkConvergence_(std::size_t iter, bool checkTolerance = true);
    bool aborted() const { return aborted_; }
    void saveCheckpoint_(std::size_t iter);
    void saveCheckpoint_(std::size_t iter, const TransientBlock<Dim>* transient);
    void saveTransientCheckpoint_(int step, double time);
    void printIterationOutput_(std::size_t iter);
    void printTransientOutput_(int step, double time);
    void printFinalStats_() const;
    void printHeader_(bool isTransient) const;
    mutable int headerCounter_ = 0; // for repeating header every N lines
    double computeAdaptiveDt_() const;
    // Refresh the reference driving speed. Must be re-run after any BC is set,
    // otherwise the clamp/limiter scales are derived from a stale value.
    void updateMaxInletSpeed_();
    void updatePVDFile_();

    // Scale p' down if it exceeds limit * rho * U_ref^2. This is NOT a no-op:
    // it rescales p' uniformly, so it silently imposes an effective
    // alphaP = alphaU = scale < 1 and breaks the PISO pressure/velocity
    // coupling (continuity is no longer satisfied at the end of the corrector).
    // Gated by config_.limitPressureCorrection so the trade-off is explicit,
    // and counted in pPrimeLimiterHits_ so it can be reported.
    bool limitPressureCorrection_(Field<Dim>& pPrime);

    const Mesh<Dim>& mesh_;
    double rho_, nu_;
    Config config_;

    Momentum<Dim> momentum_;
    PressureCorrection<Dim> pressureCorrection_;

    Field<Dim> pPrime_;
    Field<Dim> divBuffer_;
    Field<Dim> pressureBeforeCorrection_;
    std::array<Field<Dim>, Dim> pressureGradBefore_{};
    VectorField<Dim> uStar_;
    std::array<Field<Dim>, Dim> dpdx_{};
    std::array<Field<Dim>, Dim> diag_{};
    std::array<std::vector<double>, Dim> dCoeff_{};

    std::unique_ptr<Gradient<Dim>> gradientCalc_;

    std::optional<std::reference_wrapper<VectorField<Dim>>> velocity_;
    std::optional<std::reference_wrapper<Field<Dim>>> pressure_;

    SIMPLEBoundaryConditions<Dim> bc_;
    BCDiscretization bcDiscretization_;

    int lastIterations_ = 0;
    double lastContResid_ = 0.0;
    double lastMomResid_ = 0.0;
    double lastMomResidU_ = 0.0;
    double lastMomResidV_ = 0.0;
    double lastMomResidW_ = 0.0;
    // Per-outer-iteration relative velocity change. Diagnostic only: it tracks
    // iteration progress, not how well the momentum equation is satisfied.
    double lastMomChange_ = 0.0;
    std::unique_ptr<SSTModel<Dim>> turbulence_;
    std::atomic<bool> cancelRequested_{false};
    bool aborted_ = false;
    double adaptiveAlphaP_ = 0.0;
    double maxInletSpeed_ = 0.0;
    double prevContResid_ = 0.0;
    std::size_t stallCount_ = 0;
    // Number of consecutive iterations the residual criterion has held.
    int convergedStreak_ = 0;
    bool momPatternAnalyzed_ = false;
    std::size_t startIteration_ = 0;
    std::vector<double> residualHistory_;
    std::size_t divergenceCount_ = 0;
    double initialMassImbalance_ = 0.0;
    bool initialMassImbalanceSet_ = false;

    // Reusable buffers for momentum solver
    mutable SparseMatrix momA_;
    mutable Eigen::VectorXd momB_;
    // momA_/momB_ are REUSED for every velocity component, so after
    // solveMomentum_() they hold only the last component's system. The
    // momentum residual must be evaluated per direction against that
    // direction's own operator, hence these snapshots.
    mutable std::array<SparseMatrix, Dim> momAsnap_;
    mutable std::array<Eigen::VectorXd, Dim> momBsnap_;
    mutable std::vector<Triplet> momTriplets_;
    mutable Field<Dim> momDiag_;
    mutable std::array<Field<Dim>, Dim> velGrad_;
    mutable std::array<Field<Dim>, Dim> pressureGrad_;
    mutable Eigen::BiCGSTAB<SparseMatrix, Eigen::IncompleteLUT<double>> momSolver_;

    // --- Momentum AMG path (opt-in) -------------------------------------
    // One hierarchy and one Krylov solver per velocity component: the
    // convective stencil differs between U, V and W so they are not
    // interchangeable.
    mutable std::array<AMGHierarchy, Dim> momAmg_;
    mutable std::array<AMGPreconditioner, Dim> momAmgPrec_;
    mutable std::array<Eigen::BiCGSTAB<SparseMatrix, AMGPreconditioner>, Dim> momAmgSolver_;
    mutable std::array<bool, Dim> momAmgBuilt_{};
    mutable std::array<std::size_t, Dim> momAmgPatternHash_{};
    // Number of momentum solves since the hierarchy was last rebuilt (see
    // Config::momentumAmgRebuildInterval).
    mutable std::array<std::size_t, Dim> momAmgValueUpdates_{};
    mutable std::array<bool, Dim> momAmgPatternAnalyzed_{};
    // Set once we have decided the momentum operator is unsuitable for AMG, so
    // we do not rescan for positive off-diagonals every iteration.
    mutable bool momAmgDisabledReason_ = false;

    struct MomentumSolverStats {
        std::size_t solves = 0;
        std::size_t amgSolves = 0;
        std::size_t iluSolves = 0;
        std::size_t amgBuilds = 0;
        std::size_t amgRejected = 0;
        std::size_t krylovIters = 0;
        std::size_t amgKrylovIters = 0;
        double iluSeconds = 0.0;
        double amgSeconds = 0.0;
        double buildSeconds = 0.0;
    };
    mutable MomentumSolverStats momStats_{};

    // Reusable buffers to avoid per-iteration allocation
    VectorField<Dim> velocityOld_;
    Field<Dim> nutOld_;

    // Transient state
    double time_ = 0.0;
    double dt_ = 0.0;
    // Previous step size. BDF2 coefficients are derived from dt_/dtPrev_; 0
    // means "no usable history" and forces explicit Euler for this step.
    double dtPrev_ = 0.0;
    int timeStep_ = 0;
    bool restarting_ = false;
    VectorField<Dim> velocityN_;
    VectorField<Dim> velocityNM1_;
    Field<Dim> pressureN_;
    std::vector<std::string> pvdEntries_;
    std::string pvdBaseName_;
    // Turbulence transient storage
    Field<Dim> kN_;
    Field<Dim> omegaN_;
    Field<Dim> kNM1_;
    Field<Dim> omegaNM1_;
    // Set when the p' magnitude limiter had to clip, so the run can be flagged.
    std::size_t pPrimeLimiterHits_ = 0;
    // Why the run ended (see Status).
    Status status_ = Status::NotConverged;

    // Cached zero source to avoid O(Dn) alloc per iteration (was Field(mesh,0) per call)
    mutable Field<Dim> zeroSourceCache_;
    mutable bool zeroSourceInitialized_ = false;
};

extern template class SIMPLE<2>;
extern template class SIMPLE<3>;

} // namespace fvm
