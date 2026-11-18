#pragma once

#include "cfd/cfd_types.h"
#include "fvm/AMGBackend.h"
#include "fvm/Types.h"
#include <Eigen/Sparse>
#include <memory>
#include <vector>
#include <cstddef>

namespace fvm {

struct AMGLevel {
    SparseMatrix P;
    SparseMatrix R;
    Eigen::SparseMatrix<double, Eigen::RowMajor> ARM;
    // Cached inverse diagonal (1/diag, 0 where the row has no usable
    // diagonal) and the row-local position of the diagonal entry. A
    // Gauss-Seidel sweep then sums the off-diagonals without searching each
    // row for its diagonal.
    std::vector<double> invDiag;
    std::vector<int> diagPos;
    // Row i of AT is column i of ARM. Relaxing row i changes x[i] by delta,
    // which changes r by -delta * A[:, i]; AT makes that update an O(nnz)
    // sweep instead of a full matvec.
    Eigen::SparseMatrix<double, Eigen::RowMajor> AT;
    std::vector<int> cfSplit;
    int nCoarse = 0;
};

class AMGBackend;
class BoomerAMGBackend;
class AggregationBackend;

class AMGHierarchy {
public:
    using Config = cfd::AmgSettings;

    AMGHierarchy() = default;
    explicit AMGHierarchy(Config config, int blockDim = 2);
    ~AMGHierarchy();
    AMGHierarchy(const AMGHierarchy&) = delete;
    AMGHierarchy& operator=(const AMGHierarchy&) = delete;
    AMGHierarchy(AMGHierarchy&&) = default;
    AMGHierarchy& operator=(AMGHierarchy&&) = default;

    void build(const SparseMatrix& A) { backend_->build(A); }
    // Build with a sparsity hash the caller already computed (avoids the
    // O(nnz) hash pass inside the backend on every cached refresh).
    void build(const SparseMatrix& A, std::size_t patternHash) {
        backend_->build(A, patternHash);
    }

    bool solve(const Eigen::VectorXd& b, Eigen::VectorXd& x,
               const std::atomic<bool>* cancel = nullptr) const {
        return backend_->solve(b, x, cancel);
    }

    // One multigrid cycle applied to the caller's current guess `x`. Pass a
    // zeroed x for an independent cycle; calling it repeatedly on the growing
    // x applies multiple cycles. Never discards the incoming x.
    bool applyCycle(const Eigen::VectorXd& b, Eigen::VectorXd& x) const {
        return backend_->applyCycle(b, x);
    }

    bool isBuilt() const { return backend_->isBuilt(); }
    // True when the active backend caches its aggregation maps (operators can
    // be refreshed in O(nnz) instead of re-coarsened).
    bool hasCachedAggregation() const { return backend_->hasCachedAggregation(); }
    void updateFineValues(const SparseMatrix& A) { backend_->updateFineValues(A); }

    int numLevels() const { return backend_->numLevels(); }
    int fineSize() const { return backend_->fineSize(); }
    const std::vector<int>& levelSizes() const { return backend_->levelSizes(); }
    int lastIterations() const { return backend_->lastIterations(); }
    double lastResidual() const { return backend_->lastResidual(); }
    const std::vector<double>& lastResidualHistory() const { return backend_->lastResidualHistory(); }

private:
    std::unique_ptr<AMGBackend> backend_;
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
