#pragma once

#include "fvm/Types.h"
#include <Eigen/Sparse>
#include <Eigen/SparseLU>
#include <vector>
#include <cstddef>
#include <iostream>

namespace fvm {

struct AMGLevel {
    SparseMatrix P;
    SparseMatrix R;
    Eigen::SparseMatrix<double, Eigen::RowMajor> ARM;
    std::vector<int> cfSplit;
    int nCoarse = 0;
};

class AMGHierarchy {
public:
    enum class CoarseningType { RS, PMIS };
    enum class InterpolationType { Direct, Extended };
    enum class CycleType { V, F };
    // Smoother choice. The momentum (convection-diffusion) matrix is
    // nonsymmetric even for an SPD-looking operator, and forward+backward
    // Gauss-Seidel is only guaranteed convergent for SPD systems. For
    // nonsymmetric operators use forward-only GS, or forward SOR with
    // omega < 1 which is more robust (AGMG/Notay pick omega heuristically).
    enum class SmootherType { SymmetricGaussSeidel, ForwardGaussSeidel, ForwardSOR };

    struct Config {
        CoarseningType coarseningType = CoarseningType::RS;
        InterpolationType interpolationType = InterpolationType::Extended;
        CycleType cycleType = CycleType::F;
        SmootherType smootherType = SmootherType::SymmetricGaussSeidel;
        // Relaxation for ForwardSOR. 1.0 degenerates to plain Gauss-Seidel.
        // Values above 1 can make the standalone smoother divergent on
        // nonsymmetric operators, so the default stays conservative.
        double smootherOmega = 1.0;
        double strengthThreshold = 0.25;
        double truncThreshold = 0.2;
        int maxLevels = 30;
        int minCoarseSize = 10;
        int preSweeps = 2;
        int postSweeps = 2;
        int maxIterations = 100;
        double tolerance = 1e-8;
        // Hardening options (address silent-failure observations)
        int rngSeed = 42; // 0 = nondeterministic (random_device)
        double stagnationFactor = 1.05;
        int stagnationMinIter = 10;
        double stagnationLooseFactor = 5.0;
        double galerkinDropTol = 1e-12;
    };

    AMGHierarchy() = default;
    explicit AMGHierarchy(Config config) : config_(config) {}

    void build(const SparseMatrix& A);

    bool solve(const Eigen::VectorXd& b, Eigen::VectorXd& x) const;

    // One multigrid cycle applied to the caller's current guess `x`. Pass a
    // zeroed x for an independent cycle; calling it repeatedly on the growing
    // x applies multiple cycles. Never discards the incoming x.
    void applyCycle(const Eigen::VectorXd& b, Eigen::VectorXd& x) const;

    bool isBuilt() const { return !levels_.empty(); }
    void invalidate() { levels_.clear(); }
    void setConfig(Config config) { config_ = config; }
    void updateFineValues(const SparseMatrix& A);

    int numLevels() const { return static_cast<int>(levels_.size()); }
    int fineSize() const { return levels_.empty() ? 0 : static_cast<int>(levels_[0].ARM.rows()); }

private:
    Config config_;
    std::vector<AMGLevel> levels_;

    void strengthOfConnection(const SparseMatrix& A, std::vector<std::vector<int>>& strongNeighbors) const;

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
        }
        return x_vec;
    }

    template<typename Rhs>
    inline void solveInPlace(Rhs& x) const {
        if (amg_ && amg_->isBuilt()) {
            Rhs b = x;
            x.setZero();
            for (int i = 0; i < cycles_; ++i) amg_->applyCycle(b, x);
        }
    }

    inline int rows() const { return amg_ ? amg_->fineSize() : 0; }
    inline int cols() const { return amg_ ? amg_->fineSize() : 0; }

private:
    const AMGHierarchy* amg_;
    int cycles_ = 1;
};

} // namespace fvm
