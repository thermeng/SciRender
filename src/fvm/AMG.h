#pragma once

#include "fvm/Types.h"
#include <Eigen/Sparse>
#include <Eigen/SparseLU>
#include <Eigen/LU>
#include <atomic>
#include <chrono>
#include <memory>
#include <vector>
#include <cstddef>
#include <iostream>
#include <string>

namespace fvm {

struct AMGLevel {
    SparseMatrix P;
    SparseMatrix R;
    Eigen::SparseMatrix<double, Eigen::RowMajor> ARM;
    std::vector<int> cfSplit;
    int nCoarse = 0;
};

struct AmgHandleBase { virtual ~AmgHandleBase() = default; };

class AMGHierarchy {
public:
    struct Config {
        double strengthThreshold = 0.25;
        double truncThreshold = 0.2;
        int maxLevels = 30;
        int minCoarseSize = 10;
        int preSweeps = 2;
        int postSweeps = 2;
        int maxIterations = 100;
        double tolerance = 1e-8;
        double stagnationFactor = 1.05;
        int stagnationMinIter = 10;
        double stagnationLooseFactor = 5.0;
        // Non-symmetric operator (momentum): use smoothed aggregation + SPAI.
        bool nonsymmetric = false;

        // --- Solver construction (amgcl) --------------------------------
        // 's AMG uses aggregation coarsening and a Gauss-Seidel
        // smoother on every level, a V-cycle, and a direct solve on the
        // coarsest level (its log shows the level sizes roughly halving down
        // to 2 unknowns). The legacy Ruge-Stuben + ILU0 combination is kept
        // selectable for comparison and as a robustness fallback.
        enum class Coarsening { RugeStuben, SmoothedAggregation };
        //  offers Jacobi / Gauss-Seidel / ILU. SPAI0 is kept as a
        // legacy option for the nonsymmetric momentum operator.
        enum class Smoother { Jacobi, GaussSeidel, ILU0, SPAI0 };
        // Feedback printed while the hierarchy is built and cycled.
        enum class Verbosity { None, Low, High, Diagnosis };
        // Multimesh loop strategy. amgcl supports V and W natively; F and
        // Flexible are approximated by a V-cycle (documented in the dialog).
        enum class CycleType { VCycle, WCycle, FCycle, Flexible };
        // Outer acceleration wrapped around the AMG hierarchy. None means the
        // hierarchy itself is the solver ( momentum behaviour).
        enum class Acceleration { None, ConjugateGradient, BiCGStab, GMRES };
        // Coarse-grid correction scaling. amgcl has no equivalent, so this is
        // stored for round-tripping and currently has no numerical effect.
        enum class ProportionalScaling { Automatic, Disabled };
        // Group (block) size used by aggregation coarsening.
        enum class GroupSizeControl { Automatic, Manual };

        Coarsening coarsening = Coarsening::SmoothedAggregation;
        Smoother smoother = Smoother::GaussSeidel;
        Verbosity verbosity = Verbosity::None;
        CycleType cycleType = CycleType::VCycle;
        Acceleration acceleration = Acceleration::ConjugateGradient;
        ProportionalScaling scaling = ProportionalScaling::Disabled;
        GroupSizeControl groupSizeControl = GroupSizeControl::Automatic;

        // Stop coarsening once a level has at most this many unknowns. The
        // coarsest level is then solved directly.  goes down to 2.
        int coarseEnough = 2;
        //  "Enable the direct solver" / "Maximum direct solver
        // equations". When directCoarse is false the coarsest level is relaxed
        // instead of factorised.
        bool directCoarse = true;
        // Absolute residual floor ( "Epsilon"). Below this the cycle
        // stops regardless of the relative convergence tolerance.
        double epsilon = 1e-8;
        // Block size for aggregation coarsening (1 for scalar equations).
        int blockSize = 1;
        // Flexible/variable-cycle controls (stored for round-tripping; amgcl
        // has no adaptive cycle, so these currently have no numerical effect).
        double flexCoarsenRatio = 0.5;
        double flexLevelTolerance = 0.1;
        // Cycles performed when the hierarchy is used as a preconditioner
        // (amgcl `pre_cycles`).
        int preCycles = 1;
    };

    AMGHierarchy() = default;
    explicit AMGHierarchy(Config config) : config_(config) {}

    void build(const SparseMatrix& A);

    bool solve(const Eigen::VectorXd& b, Eigen::VectorXd& x,
               const std::atomic<bool>* cancel = nullptr) const;

    // One multigrid cycle applied to the caller's current guess `x`. Pass a
    // zeroed x for an independent cycle; calling it repeatedly on the growing
    // x applies multiple cycles. Never discards the incoming x.
    bool applyCycle(const Eigen::VectorXd& b, Eigen::VectorXd& x) const;

    bool isBuilt() const { return built_; }
    void invalidate() { built_ = false; amgHandle_.reset(); levels_.clear(); n_ = 0; systemMatrix_.resize(0, 0); nlevels_ = 0; levelSizes_.clear(); report_.clear(); }
    void setConfig(Config config) { config_ = config; }
    const Config& config() const { return config_; }
    void updateFineValues(const SparseMatrix& A);

    // Number of levels actually built by amgcl (was hard-coded to 1, which
    // made the  level-by-level comparison impossible).
    int numLevels() const { return nlevels_; }
    int fineSize() const { return n_; }
    const std::vector<int>& levelSizes() const { return levelSizes_; }
    // amgcl's own hierarchy report ("Number of levels", operator complexity,
    // per-level unknowns/nonzeros). Printed when solver statistics are on.
    const std::string& hierarchyReport() const { return report_; }
    // Diagnostics of the last solve() call: cycles performed and the final
    // relative residual (|b-Ax|/|b|), the same pair  logs.
    int lastIterations() const { return lastIterations_; }
    double lastResidual() const { return lastResidual_; }
    double buildTime() const { return buildTimeSec_; }
    double solveTime() const { return solveTimeSec_; }

private:
    Config config_;
    std::vector<AMGLevel> levels_;
    std::shared_ptr<void> amgHandle_;
    bool built_ = false;
    int n_ = 0;
    SparseMatrix systemMatrix_;
    int nlevels_ = 0;
    std::vector<int> levelSizes_;
    std::string report_;
    mutable int lastIterations_ = 0;
    mutable double lastResidual_ = 0.0;
    mutable double buildTimeSec_ = 0.0;
    mutable double solveTimeSec_ = 0.0;

    void strengthOfConnection(const SparseMatrix& A, std::vector<std::vector<int>>& strongNeighbors) const;

    void printSolveDiagnosis() const;

    void rsCoarsening(const std::vector<std::vector<int>>& strongNeighbors,
                      std::vector<int>& cfSplit, int& nCoarse) const;

    void pmisCoarsening(const std::vector<std::vector<int>>& strongNeighbors,
                        std::vector<int>& cfSplit, int& nCoarse) const;

    void directInterpolation(const SparseMatrix& A,
                             const std::vector<std::vector<int>>& strongNeighbors,
                             const std::vector<int>& cfSplit, int nCoarse,
                             SparseMatrix& P) const;

    void extendedInterpolation(const SparseMatrix& A,
                               const std::vector<std::vector<int>>& strongNeighbors,
                               const std::vector<int>& cfSplit, int nCoarse,
                               SparseMatrix& P) const;

    void truncateInterpolation(SparseMatrix& P) const;

    SparseMatrix galerkinProduct(const SparseMatrix& A, const SparseMatrix& P) const;

    void vCycle(int level, const Eigen::VectorXd& b, Eigen::VectorXd& x) const;

    void fCycle(int level, const Eigen::VectorXd& b, Eigen::VectorXd& x) const;

    void cycle(int level, const Eigen::VectorXd& b, Eigen::VectorXd& x) const;

    // Direction-aware smoother. `forward == true` sweeps i = 0..n-1 only
    // (correct for nonsymmetric operators); `forward == false` sweeps in
    // reverse and, combined with a preceding forward sweep, gives symmetric
    // Gauss-Seidel (only valid for SPD). omega implements SOR on the
    // diagonal: x_i = x_i + omega * ((b_i - sigma)/a_ii - x_i).
    void gaussSeidel(const Eigen::SparseMatrix<double, Eigen::RowMajor>& ARM,
                     const Eigen::VectorXd& b, Eigen::VectorXd& x,
                     int numSweeps, bool forward) const;

    // Pre/post smoothing according to config_.smootherType. For the
    // nonsymmetric variants both are forward sweeps so the composed
    // preconditioner stays a fixed linear operator.
    void smoothPre_(const Eigen::SparseMatrix<double, Eigen::RowMajor>& ARM,
                    const Eigen::VectorXd& b, Eigen::VectorXd& x, int numSweeps) const;
    void smoothPost_(const Eigen::SparseMatrix<double, Eigen::RowMajor>& ARM,
                     const Eigen::VectorXd& b, Eigen::VectorXd& x, int numSweeps) const;
};

class AMGPreconditioner {
public:
    AMGPreconditioner() : amg_(nullptr) {}

    void setAMG(const AMGHierarchy* amg) { amg_ = amg; }

    // Multigrid cycles applied per preconditioner application. More cycles
    // make the preconditioner stronger at proportional cost; 1 (a single
    // V/F-cycle) is the usual choice for a Krylov preconditioner.
    void setCycles(int cycles) { cycles_ = cycles < 1 ? 1 : cycles; }
    inline bool isReady() const { return amg_ && amg_->isBuilt(); }

    template<typename MatrixType>
    void analyzePattern(const MatrixType&) const {}

    template<typename MatrixType>
    void factorize(const MatrixType&) const {}

    template<typename MatrixType>
    void compute(const MatrixType&) const {}

    inline Eigen::ComputationInfo info() const { return Eigen::Success; }

    template<typename Rhs>
    inline const Rhs solve(const Rhs& b) const {
        Eigen::VectorXd b_vec = b;
        Eigen::VectorXd x_vec = Eigen::VectorXd::Zero(b_vec.rows());
        if (amg_ && amg_->isBuilt()) {
            for (int i = 0; i < cycles_; ++i) amg_->applyCycle(b_vec, x_vec);
            if (!x_vec.allFinite()) x_vec.setZero();
        }
        return x_vec;
    }

    template<typename Rhs>
    inline void solveInPlace(Rhs& x) const {
        if (amg_ && amg_->isBuilt()) {
            Rhs b = x;
            x.setZero();
            for (int i = 0; i < cycles_; ++i) amg_->applyCycle(b, x);
            if (!x.allFinite()) x.setZero();
        }
    }

    inline int rows() const { return amg_ ? amg_->fineSize() : 0; }
    inline int cols() const { return amg_ ? amg_->fineSize() : 0; }

private:
    const AMGHierarchy* amg_;
    int cycles_ = 1;
};

} // namespace fvm
