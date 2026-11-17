#pragma once

#include "cfd/cfd_types.h"
#include "fvm/Types.h"

#include <Eigen/Sparse>
#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

namespace fvm {

// AMGBackend is the abstract interface for AMG backends. Two concrete
// implementations exist:
//   BoomerAMGBackend  - Hypre BoomerAMG (value-dependent coarsening every build)
//   AggregationBackend - Cached aggregation (maps built once per pattern, O(nnz) refresh)
//
// AMGHierarchy holds a unique_ptr<AMGBackend> and delegates all operations to it.
// Callers never branch on backend type - that decision is made once at
// construction time and is invisible thereafter. This is the real seam: two
// adapters, one interface.
class AMGBackend {
public:
    using Config = cfd::AmgSettings;

    virtual ~AMGBackend() = default;

    // --- Build / invalidate ---------------------------------------------
    virtual void build(const SparseMatrix& A) = 0;
    virtual void invalidate() = 0;
    virtual bool isBuilt() const = 0;

    // --- Solve -----------------------------------------------------------
    virtual bool solve(const Eigen::VectorXd& b, Eigen::VectorXd& x,
                       const std::atomic<bool>* cancel = nullptr) = 0;
    virtual bool applyCycle(const Eigen::VectorXd& b, Eigen::VectorXd& x) const = 0;
    virtual Eigen::VectorXd flexCycle(int level, const Eigen::VectorXd& b,
                                      Eigen::VectorXd& x) const = 0;

    // --- Hierarchy info --------------------------------------------------
    virtual int numLevels() const = 0;
    virtual int fineSize() const = 0;
    virtual const std::vector<int>& levelSizes() const = 0;

    // --- Stats -----------------------------------------------------------
    virtual int lastIterations() const = 0;
    virtual double lastResidual() const = 0;
    virtual const std::vector<double>& lastResidualHistory() const = 0;

    // --- Backend-specific ------------------------------------------------
    virtual bool hasCachedAggregation() const = 0;
    virtual void updateFineValues(const SparseMatrix& A) = 0;
};

} // namespace fvm
