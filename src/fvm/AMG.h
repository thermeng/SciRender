#pragma once

#include "cfd/cfd_types.h"
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
    using Config = cfd::AmgSettings;

    AMGHierarchy() = default;
    explicit AMGHierarchy(Config config) : config_(config) {}

    void build(const SparseMatrix& A);

    bool solve(const Eigen::VectorXd& b, Eigen::VectorXd& x,
               const std::atomic<bool>* cancel = nullptr) const;

    // One multigrid cycle applied to the caller's current guess `x`. Pass a
    // zeroed x for an independent cycle; calling it repeatedly on the growing
    // x applies multiple cycles. Never discards the incoming x.
    bool applyCycle(const Eigen::VectorXd& b, Eigen::VectorXd& x) const;

    // Recursive style flexible cycle. Evaluates alpha (termination) and
    // beta (restriction) gates at every level, allowing multi-level traversal
    // (e.g. 0-1-2-3-2-3-2-1-0) instead of a depth-capped static walk.
    // Returns the correction at this level (x - x_start); x is modified in place.
    Eigen::VectorXd flexCycle(int level, const Eigen::VectorXd& b, Eigen::VectorXd& x) const;

    bool isBuilt() const { return built_; }
    void invalidate() { built_ = false; amgHandle_.reset(); hypreMat_.reset(); hypreSolver_.reset(); hypreScale_.clear(); useHypreActive_ = false; levels_.clear(); cgrid_.clear(); n_ = 0; systemMatrix_.resize(0, 0); nlevels_ = 0; levelSizes_.clear(); report_.clear(); scale_.clear(); lastIterations_ = 0; lastResidual_ = 0.0; lastResidualHistory_.clear(); buildTimeSec_ = 0.0; solveTimeSec_ = 0.0; fineRows_.resize(0, 0); fineRowsReady_ = false; lazyBuild_ = false; hypreHierarchyReady_ = false; }
    void setConfig(Config config) { config_ = config; invalidate(); }
    const Config& config() const { return config_; }
    void updateFineValues(const SparseMatrix& A);

    // Number of levels actually built by amgcl (was hard-coded to 1, which
    // made the  level-by-level comparison impossible).
    int numLevels() const { return nlevels_; }
    int fineSize() const { return n_; }
    const std::vector<int>& levelSizes() const { return levelSizes_; }
    const std::string& hierarchyReport() const { return report_; }
    int lastIterations() const { return lastIterations_; }
    double lastResidual() const { return lastResidual_; }
    const std::vector<double>& lastResidualHistory() const { return lastResidualHistory_; }
    double buildTime() const { return buildTimeSec_; }
    double solveTime() const { return solveTimeSec_; }

private:
    Config config_;
    mutable std::vector<AMGLevel> levels_;
    bool built_ = false;
    std::shared_ptr<void> amgHandle_;
    // Hypre-only
    mutable std::shared_ptr<void> hypreMat_;
    mutable std::shared_ptr<void> hypreSolver_;
    mutable std::vector<double> hypreScale_;
    mutable bool useHypreActive_ = false;
    int n_ = 0;
    SparseMatrix systemMatrix_;
    // Row-major copy of the fine operator used by the residual-gated Flexible
    // cycle's level-0 Gauss-Seidel sweep. Rebuilt together with the hierarchy.
    mutable Eigen::SparseMatrix<double, Eigen::RowMajor> fineRows_;
    mutable bool fineRowsReady_ = false;
    // A Flexible cycle is built lazily: the fine level is enough whenever one
    // Gauss-Seidel sweep meets the tolerance (STAR reports exactly one level
    // for momentum). The BoomerAMG hierarchy is only set up when the residual
    // gate asks for a coarse correction.
    bool lazyBuild_ = false;
    mutable bool hypreHierarchyReady_ = false;
    std::vector<double> scale_;
    mutable     int nlevels_ = 0;
    mutable std::vector<int> levelSizes_;
    // cgrid[i] = deepest level containing fine row i (from BoomerAMG grid
    // hierarchy). Used to build per-level prolongation/restriction operators.
    mutable std::vector<int> cgrid_;
    mutable std::string report_;
    // Hypre-only: workspaces per applyCycle
    mutable int lastIterations_ = 0;
    mutable double lastResidual_ = 0.0;
    mutable std::vector<double> lastResidualHistory_;
    mutable double buildTimeSec_ = 0.0;
    mutable double solveTimeSec_ = 0.0;

    void strengthOfConnection(const Eigen::SparseMatrix<double, Eigen::RowMajor>& A, std::vector<std::vector<int>>& strongNeighbors) const;

    // Lazily set up the BoomerAMG hierarchy for a deferred Flexible build.
    // Returns true when the hierarchy is ready (possibly already was).
    bool ensureHypreBuilt_() const;

    // Build per-level prolongation/restriction/operator from cgrid_ for the
    // recursive Flexible cycle. Called from build() after the Hypre hierarchy
    // is constructed.
    void buildLevelOperators_() const;

    // Level-0 Gauss-Seidel smoother for the residual-gated Flexible cycle.
    void fineGaussSeidel_(const Eigen::VectorXd& b, Eigen::VectorXd& x,
                          int sweeps) const;

    // Flexible solve: one fine sweep is the whole cycle when it reduces the
    // residual below beta*R_{i-1}; otherwise a coarse AMG correction is
    // applied (and the hierarchy is built on demand).
    bool solveFlexible_(const Eigen::VectorXd& b, Eigen::VectorXd& x,
                        const std::atomic<bool>* cancel) const;

    // One residual-gated Flexible cycle. Returns false on failure.
    bool applyFlexibleCycle_(const Eigen::VectorXd& b, Eigen::VectorXd& x) const;

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

    void setAMG(const AMGHierarchy* amg) {
        amg_ = amg;
        if (amg_ && amg_->isBuilt()) {
            const int n = amg_->fineSize();
            workspace_b_.resize(n);
            workspace_x_.resize(n);
            workspace_x_.setZero();
        }
    }

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
        ensureWorkspace_(b.size());
        if (!amg_ || !amg_->isBuilt()) {
            return Eigen::VectorXd::Zero(b.size());
        }
        workspace_b_ = b;
        workspace_x_.setZero();
        for (int i = 0; i < cycles_; ++i) amg_->applyCycle(workspace_b_, workspace_x_);
        if (!workspace_x_.allFinite()) workspace_x_.setZero();
        return workspace_x_;
    }

    template<typename Rhs>
    inline void solveInPlace(Rhs& x) const {
        ensureWorkspace_(x.size());
        if (!amg_ || !amg_->isBuilt()) return;
        workspace_b_ = x;
        workspace_x_.setZero();
        for (int i = 0; i < cycles_; ++i) amg_->applyCycle(workspace_b_, workspace_x_);
        if (!workspace_x_.allFinite()) workspace_x_.setZero();
        x = workspace_x_;
    }

    inline int rows() const { return amg_ ? amg_->fineSize() : 0; }
    inline int cols() const { return amg_ ? amg_->fineSize() : 0; }

private:
    void ensureWorkspace_(std::size_t n) const {
        if (workspace_b_.size() != static_cast<Eigen::Index>(n))
            workspace_b_.resize(n);
        if (workspace_x_.size() != static_cast<Eigen::Index>(n)) {
            workspace_x_.resize(n);
            workspace_x_.setZero();
        }
    }

    const AMGHierarchy* amg_;
    int cycles_ = 1;
    mutable Eigen::VectorXd workspace_b_;
    mutable Eigen::VectorXd workspace_x_;
};

} // namespace fvm
