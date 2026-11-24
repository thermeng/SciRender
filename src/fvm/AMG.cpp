// Cached-aggregation AMGHierarchy — no external linear solver dependency.
#include "fvm/AMG.h"
#include <Eigen/LU>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <ostream>
#include <queue>
#include <stdexcept>
#include <utility>
#include <vector>

namespace fvm {

using cfd::AmgSettings;

// ============================================================================
// AggregationBackend — Cached aggregation backend (no Hypre)
// ============================================================================
class AggregationBackend : public AMGBackend {
public:
    explicit AggregationBackend(Config config, int blockDim)
        : config_(config), blockDim_(std::max(1, blockDim)) {}

    void build(const SparseMatrix& A) override;
    void build(const SparseMatrix& A, std::size_t patternHash) override;
    void invalidate() override;
    bool isBuilt() const override { return built_; }

    bool solve(const Eigen::VectorXd& b, Eigen::VectorXd& x,
               const std::atomic<bool>* cancel = nullptr) override;
    bool applyCycle(const Eigen::VectorXd& b, Eigen::VectorXd& x) const override;
    Eigen::VectorXd flexCycle(int level, const Eigen::VectorXd& b,
                              Eigen::VectorXd& x) const override;

    int numLevels() const override { return nlevels_; }
    int fineSize() const override { return n_; }
    const std::vector<int>& levelSizes() const override { return levelSizes_; }
    int lastIterations() const override { return lastIterations_; }
    double lastResidual() const override { return lastResidual_; }
    const std::vector<double>& lastResidualHistory() const override { return lastResidualHistory_; }

    void updateFineValues(const SparseMatrix& A) override { build(A); }

private:
    using AggRowMatrix = Eigen::SparseMatrix<double, Eigen::RowMajor>;

    Config config_;
    int blockDim_ = 2;
    int n_ = 0;
    std::vector<std::vector<int>> aggMaps_;
    std::vector<int> aggSizes_;
    std::size_t aggPatternHash_ = 0;
    struct AggLevelCache {
        std::vector<int> scatter;
    };
    std::vector<AggLevelCache> aggCaches_;
    std::vector<AMGLevel> levels_;
    std::vector<int> levelSizes_;
    int nlevels_ = 0;
    bool built_ = false;
    mutable int lastIterations_ = 0;
    mutable double lastResidual_ = 0.0;
    mutable std::vector<double> lastResidualHistory_;
    // SparseMatrix (ColMajor) value index -> levels_[0].ARM (RowMajor) value
    // index. Built once per sparsity pattern so a value refresh is a flat
    // scatter instead of a structural conversion.
    std::vector<int> fineScatter_;
    bool fineScatterReady_ = false;
    // Per-level scratch for the Flexible-cycle coarse correction, so a cycle
    // does not allocate rCoarse/xCoarse on every level visit. The recursion is
    // depth-first, so each level owns its slot.
    mutable std::vector<Eigen::VectorXd> flexRCoarseScratch_;
    mutable std::vector<Eigen::VectorXd> flexXCoarseScratch_;
    std::vector<double> scale_;
    // Verbose-diagnostic timing (mutable: accumulated during const solves).
    mutable double buildSeconds_ = 0.0;
    mutable double solveSeconds_ = 0.0;
    mutable std::vector<double> levelSeconds_;

    void refreshAggregationOperators_(const SparseMatrix& A);
    void buildAggScatterCaches_();
    void buildFineScatter_(const SparseMatrix& A);
    void buildDiagCache_(AMGLevel& level);
    int aggregate_(const AggRowMatrix& A, int groupSize, std::vector<int>& agg) const;
    bool aggregationVcycle_(int level, const Eigen::VectorXd& b,
                              Eigen::VectorXd& x) const;
    bool applyFlexibleCycle_(const Eigen::VectorXd& b, Eigen::VectorXd& x,
                             double* outResNorm = nullptr) const;
    bool solveFlexible_(const Eigen::VectorXd& b, Eigen::VectorXd& x,
                        const std::atomic<bool>* cancel) const;
    bool solveAggregation_(const Eigen::VectorXd& b, Eigen::VectorXd& x,
                           const std::atomic<bool>* cancel) const;
    void printVerbose(const char* name, std::ostream& os) const override;
    bool applyFlexibleFineCycle_(const Eigen::VectorXd& b,
                                 Eigen::VectorXd& x) const;
    bool solveFlexibleFine_(const Eigen::VectorXd& b, Eigen::VectorXd& x,
                            const std::atomic<bool>* cancel) const;
    void fineGaussSeidel_(const Eigen::VectorXd& b, Eigen::VectorXd& x,
                          int sweeps) const;
    // One Flexible cycle on `level`, updating x in place. b is the level right
    // hand side. outResNorm (optional) receives ||b - A x|| on exit so the
    // caller does not recompute it with a full matvec.
    void flexCycleImpl_(int level, const Eigen::VectorXd& b, Eigen::VectorXd& x,
                        double* outResNorm) const;
    // Restrict r to the next level, run one Flexible cycle there, prolong the
    // correction back into x. Used by both Flexible-cycle exits.
    void coarsenAndProlong_(int level, const Eigen::VectorXd& r,
                            Eigen::VectorXd& x) const;
    void gaussSeidel(const AMGLevel& level,
                     const Eigen::VectorXd& b, Eigen::VectorXd& x,
                     int numSweeps, bool forward) const;
    // Gauss-Seidel sweep that also maintains r = b - A x incrementally, using
    // the cached per-level transpose. r must be current on entry.
    void gaussSeidelResidual_(const AMGLevel& level, const Eigen::VectorXd& b,
                              Eigen::VectorXd& x, Eigen::VectorXd& r,
                              int numSweeps, bool forward) const;
    void smoothPre_(const AMGLevel& level,
                    const Eigen::VectorXd& b, Eigen::VectorXd& x, int numSweeps) const;
    void smoothPost_(const AMGLevel& level,
                     const Eigen::VectorXd& b, Eigen::VectorXd& x, int numSweeps) const;
};

// ----------------------------------------------------------------------------
// Anonymous-namespace aggregation helpers (free functions, no class state)
// ----------------------------------------------------------------------------

namespace {

using AggRowMatrix = Eigen::SparseMatrix<double, Eigen::RowMajor>;

// P^T A P with piecewise-constant P: every fine coefficient is added to the
// (aggregate(row), aggregate(col)) entry.
AggRowMatrix coarsenByAggregation(const AggRowMatrix& A,
                                  const std::vector<int>& agg, int nCoarse) {
    std::vector<Eigen::Triplet<double>> triplets;
    triplets.reserve(static_cast<std::size_t>(A.nonZeros()));
    for (int row = 0; row < A.outerSize(); ++row) {
        const int ci = agg[static_cast<std::size_t>(row)];
        for (AggRowMatrix::InnerIterator it(A, row); it; ++it)
            triplets.emplace_back(ci, agg[static_cast<std::size_t>(it.col())], it.value());
    }
    Eigen::SparseMatrix<double, Eigen::RowMajor> out(static_cast<Eigen::Index>(nCoarse),
                                                    static_cast<Eigen::Index>(nCoarse));
    out.setFromTriplets(triplets.begin(), triplets.end());
    out.makeCompressed();

    // Sparsify: keep the diagonal plus up to maxPerRow largest-magnitude
    // off-diagonal entries per row. The full Galerkin operator is kept, so
    // this cap is generous -- it only guards against pathological density, it
    // does not truncate the inter-aggregate couplings the next coarsening needs.
    constexpr int maxPerRow = 1000000;
    std::vector<Eigen::Triplet<double>> kept;
    kept.reserve(static_cast<std::size_t>(out.nonZeros()));
    for (int row = 0; row < out.outerSize(); ++row) {
        double diag = 0.0;
        std::vector<std::tuple<double, double, int>> offdiag;
        for (Eigen::SparseMatrix<double, Eigen::RowMajor>::InnerIterator it(out, row); it; ++it) {
            if (it.col() == row) diag = it.value();
            else offdiag.emplace_back(std::abs(it.value()), it.value(), it.col());
        }
        std::sort(offdiag.begin(), offdiag.end(),
                  [](const auto& a, const auto& b) { return std::get<0>(a) > std::get<0>(b); });
        const int keep = std::min(maxPerRow, static_cast<int>(offdiag.size()));
        for (int k = 0; k < keep; ++k)
            kept.emplace_back(row, std::get<2>(offdiag[static_cast<std::size_t>(k)]),
                              std::get<1>(offdiag[static_cast<std::size_t>(k)]));
        if (diag != 0.0) kept.emplace_back(row, row, diag);
    }
    out.setFromTriplets(kept.begin(), kept.end());
    out.makeCompressed();
    return out;
}

// Conservative aggregation: each ungrouped cell opens a group
// and absorbs its strongest currently-ungrouped neighbour, then the
// neighbour's strongest ungrouped neighbour, and so on up to groupSize.
// "Strongest" is the largest |A_ij|; ungrouped neighbours are preferred so
// the coarsening does not collapse to singletons. The coarse operator is the
// Galerkin product R A P (coarsenByAggregation), which is conservative.
int aggregateGreedy(const AggRowMatrix& A, int groupSize, std::vector<int>& agg) {
    const int n = static_cast<int>(A.rows());
    agg.assign(static_cast<std::size_t>(n), -1);
    if (groupSize <= 1 || n <= 1) {
        for (int i = 0; i < n; ++i) agg[static_cast<std::size_t>(i)] = i;
        return n;
    }
    int nGroups = 0;
    std::vector<int> members;
    members.reserve(static_cast<std::size_t>(groupSize));
    for (int i = 0; i < n; ++i) {
        if (agg[static_cast<std::size_t>(i)] >= 0) continue;
        const int group = nGroups++;
        agg[static_cast<std::size_t>(i)] = group;
        members.clear();
        members.push_back(i);
        int count = 1;
        while (count < groupSize) {
            int bestJ = -1;
            double bestVal = -1.0;
            for (int m : members) {
                for (AggRowMatrix::InnerIterator it(A, m); it; ++it) {
                    const int j = it.col();
                    if (j == m || agg[static_cast<std::size_t>(j)] >= 0) continue;
                    const double v = std::abs(it.value());
                    if (v > bestVal) { bestVal = v; bestJ = j; }
                }
            }
            if (bestJ < 0) break;
            agg[static_cast<std::size_t>(bestJ)] = group;
            members.push_back(bestJ);
            ++count;
        }
    }
    return nGroups;
}

// Build the tentative prolongation T (piecewise constant): T(i, agg[i]) = 1.
SparseMatrix buildTentativeProlongation(const SparseMatrix& A,
                                         const std::vector<int>& agg,
                                         int nCoarse) {
    int n = static_cast<int>(A.rows());
    std::vector<Eigen::Triplet<double>> triplets;
    triplets.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        triplets.emplace_back(i, agg[static_cast<std::size_t>(i)], 1.0);
    SparseMatrix T(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(nCoarse));
    T.setFromTriplets(triplets.begin(), triplets.end());
    T.makeCompressed();
    return T;
}

// Jacobi-smooth the tentative prolongator:
//   P = (I - omega * D^-1 * A)^degree * T
SparseMatrix smoothProlongationJacobi(const SparseMatrix& A, const SparseMatrix& T,
                                       double omega, int degree) {
    int n = static_cast<int>(A.rows());
    std::vector<double> D(static_cast<std::size_t>(n), 1.0);
    for (int i = 0; i < n; ++i) {
        double diag = A.coeff(i, i);
        if (std::abs(diag) > 1e-14)
            D[static_cast<std::size_t>(i)] = diag;
    }
    SparseMatrix P = T;
    for (int d = 0; d < degree; ++d) {
        SparseMatrix AP = A * P;
        for (int j = 0; j < AP.outerSize(); ++j) {
            for (SparseMatrix::InnerIterator it(AP, j); it; ++it) {
                const int row = it.row();
                it.valueRef() *= (omega / D[static_cast<std::size_t>(row)]);
            }
        }
        P = P - AP;
        P.makeCompressed();
        P.prune(0.0);
    }
    return P;
}

// Galerkin coarse operator with smoothed prolongation: A_c = P^T * A * P
SparseMatrix galerkinProductPAP(const SparseMatrix& P, const SparseMatrix& A) {
    SparseMatrix AP = A * P;
    SparseMatrix Ac = P.transpose() * AP;
    Ac.makeCompressed();
    Ac.prune(0.0);
    return Ac;
}

} // namespace
// ============================================================================
// AggregationBackend implementation
// ============================================================================

void AggregationBackend::invalidate() {
    built_ = false;
    n_ = 0;
    nlevels_ = 0;
    levelSizes_.clear();
    aggMaps_.clear();
    aggSizes_.clear();
    aggPatternHash_ = 0;
    aggCaches_.clear();
    levels_.clear();
    lastIterations_ = 0;
    lastResidual_ = 0.0;
    lastResidualHistory_.clear();
    fineScatter_.clear();
    fineScatterReady_ = false;
    scale_.clear();
}

void AggregationBackend::build(const SparseMatrix& A) {
    build(A, detail::sparsityHash(A));
}

void AggregationBackend::build(const SparseMatrix& A, std::size_t hash) {
    const auto tBuild0 = std::chrono::steady_clock::now();
    n_ = static_cast<int>(A.rows());
    const bool mapsValid = !aggMaps_.empty() && hash == aggPatternHash_;

    if (!mapsValid) {
        aggMaps_.clear();
        aggSizes_.clear();
        aggCaches_.clear();
        levels_.clear();
        levelSizes_.clear();
        fineScatter_.clear();
        fineScatterReady_ = false;
        aggPatternHash_ = hash;

        AggRowMatrix level = A;
        level.makeCompressed();
        // Conservative aggregation groups each
        // cell with its strongest ungrouped neighbour, up to "Coarsen by".
        // The scalar default is 2 for Gauss-Seidel; 4 is the coupled/2D default
        // and coarsens fast enough to avoid the deep, stalling hierarchy that
        // groupSize=2 produces on an irregular 3D tet mesh.
        const int groupSize = 4;
        const int coarseTarget = 128;
        for (int depth = 0; depth < config_.maxLevels; ++depth) {
            const int n = static_cast<int>(level.rows());
            std::vector<int> agg;
            const int nCoarse = aggregate_(level, groupSize, agg);
            if (nCoarse >= n || nCoarse <= 0) break;
            aggMaps_.push_back(std::move(agg));
            aggSizes_.push_back(nCoarse);
            level = coarsenByAggregation(level, aggMaps_.back(), nCoarse);
            if (nCoarse <= coarseTarget) break;
        }
    }

    refreshAggregationOperators_(A);
    if (!config_.smoothedProlongation && aggCaches_.size() != aggMaps_.size())
        buildAggScatterCaches_();

    nlevels_ = static_cast<int>(levels_.size());
    if (config_.verbosity >= cfd::AmgSettings::Verbosity::High ||
        std::getenv("SCIRENDER_AGG_TIMING")) {
        for (int l = 0; l < nlevels_; ++l)
            std::cerr << "  level " << l << ": " << levelSizes_[l]
                      << " unknowns, " << levels_[l].ARM.nonZeros() << " nnz\n";
    }
    built_ = !levels_.empty();
    buildSeconds_ += std::chrono::duration<double>(
        std::chrono::steady_clock::now() - tBuild0).count();
    levelSeconds_.assign(levels_.size(), 0.0);
}

void AggregationBackend::buildFineScatter_(const SparseMatrix& A) {
    // The fine operator is levels_[0].ARM (RowMajor). A is ColMajor. For every
    // A value index k, record the RowMajor value index holding the same (i,j)
    // entry. A row scan of the RowMajor matrix plus a binary search in the
    // ColMajor column gives the mapping in O(nnz log deg).
    auto& fine = levels_[0].ARM;
    const int nnz = A.nonZeros();
    fineScatter_.assign(static_cast<std::size_t>(nnz), -1);
    const int* aOuter = A.outerIndexPtr();
    const int* aInner = A.innerIndexPtr();
    int p = 0;
    for (int row = 0; row < fine.outerSize(); ++row) {
        for (AggRowMatrix::InnerIterator it(fine, row); it; ++it, ++p) {
            const int col = it.col();
            const int* b = aInner + aOuter[col];
            const int* e = aInner + aOuter[col + 1];
            const int* lb = std::lower_bound(b, e, row);
            if (lb != e && *lb == row)
                fineScatter_[static_cast<std::size_t>(lb - aInner)] = p;
        }
    }
    fineScatterReady_ = true;
}

void AggregationBackend::buildDiagCache_(AMGLevel& level) {
    const AggRowMatrix& A = level.ARM;
    const int n = static_cast<int>(A.rows());
    const int* outer = A.outerIndexPtr();
    const int* inner = A.innerIndexPtr();
    const double* vals = A.valuePtr();
    level.invDiag.assign(static_cast<std::size_t>(n), 0.0);
    level.diagPos.assign(static_cast<std::size_t>(n), -1);
    for (int i = 0; i < n; ++i) {
        const int ib = outer[i];
        const int ie = outer[i + 1];
        int pos = -1;
        for (int k = ib; k < ie; ++k) {
            if (inner[k] == i) { pos = k - ib; break; }
        }
        level.diagPos[static_cast<std::size_t>(i)] = pos;
        if (pos >= 0) {
            const double d = vals[ib + pos];
            if (std::isfinite(d) && std::abs(d) >= 1e-14)
                level.invDiag[static_cast<std::size_t>(i)] = 1.0 / d;
        }
    }
    // Row i of AT is column i of ARM: the residual update after a
    // Gauss-Seidel row change is then an O(nnz) sweep. Only the Flexible
    // cycle maintains that residual, so the V-cycle smoother never needs AT
    // and must not pay for the transpose.
    if (config_.cycleType == AmgSettings::CycleType::Flexible) {
        level.AT = A.transpose();
        level.AT.makeCompressed();
    } else if (level.AT.nonZeros() != 0) {
        level.AT.resize(0, 0);
    }
}

void AggregationBackend::buildAggScatterCaches_() {
    aggCaches_.clear();
    aggCaches_.resize(aggMaps_.size());
    for (std::size_t l = 0; l < aggMaps_.size() && l + 1 < levels_.size(); ++l) {
        const auto& fine = levels_[l].ARM;
        const auto& coarse = levels_[l + 1].ARM;
        const auto& agg = aggMaps_[l];
        auto& scatter = aggCaches_[l].scatter;
        scatter.resize(static_cast<std::size_t>(fine.nonZeros()));
        const int* outer = coarse.outerIndexPtr();
        const int* inner = coarse.innerIndexPtr();
        int k = 0;
        for (int row = 0; row < fine.outerSize(); ++row) {
            const int ci = agg[static_cast<std::size_t>(row)];
            for (AggRowMatrix::InnerIterator it(fine, row); it; ++it, ++k) {
                const int cj = agg[static_cast<std::size_t>(it.col())];
                const int* lb = std::lower_bound(
                    inner + outer[ci], inner + outer[ci + 1], cj);
                scatter[static_cast<std::size_t>(k)] =
                    (lb < inner + outer[ci + 1] && *lb == cj)
                        ? static_cast<int>(lb - inner) : -1;
            }
        }
    }
}

void AggregationBackend::refreshAggregationOperators_(const SparseMatrix& A) {
    const bool havePattern =
        !config_.smoothedProlongation &&
        levels_.size() == aggMaps_.size() + 1 && !levels_.empty() &&
        levels_[0].ARM.rows() == A.rows() &&
        aggCaches_.size() == aggMaps_.size();
    if (havePattern) {
        // O(nnz) value refresh: copy A's values into the cached RowMajor fine
        // operator through the scatter built once per pattern.
        auto& fine = levels_[0].ARM;
        if (fineScatterReady_ &&
            fineScatter_.size() == static_cast<std::size_t>(A.nonZeros()) &&
            fine.nonZeros() == A.nonZeros()) {
            double* fv = fine.valuePtr();
            const double* av = A.valuePtr();
            const int nnz = A.nonZeros();
            for (int k = 0; k < nnz; ++k)
                fv[static_cast<std::size_t>(fineScatter_[static_cast<std::size_t>(k)])] = av[k];
        } else {
            fine = A;   // structural fallback (first refresh after a rebuild)
            fine.makeCompressed();
            buildFineScatter_(A);
        }
        for (std::size_t l = 0; l < aggMaps_.size(); ++l) {
            auto& coarse = levels_[l + 1].ARM;
            auto* coarseVals = coarse.valuePtr();
            std::fill(coarseVals, coarseVals + coarse.nonZeros(), 0.0);
            const auto& fineVals = levels_[l].ARM.valuePtr();
            const auto& scatter = aggCaches_[l].scatter;
            const int count = std::min(
                static_cast<int>(scatter.size()),
                static_cast<int>(levels_[l].ARM.nonZeros()));
            for (int k = 0; k < count; ++k) {
                const int pos = scatter[static_cast<std::size_t>(k)];
                if (pos >= 0) coarseVals[pos] += fineVals[k];
            }
        }
        for (std::size_t l = 0; l < levels_.size(); ++l)
            buildDiagCache_(levels_[l]);
        return;
    }
    levels_.assign(aggMaps_.size() + 1, AMGLevel{});
    levelSizes_.assign(aggMaps_.size() + 1, 0);
    AggRowMatrix level = A;
    level.makeCompressed();
    for (std::size_t l = 0; l < levels_.size(); ++l) {
        levels_[l].ARM = level;
        levels_[l].ARM.makeCompressed();
        levelSizes_[l] = static_cast<int>(level.rows());
        if (l >= aggMaps_.size()) break;
        if (config_.smoothedProlongation) {
            SparseMatrix T = buildTentativeProlongation(level, aggMaps_[l], aggSizes_[l]);
            SparseMatrix P = smoothProlongationJacobi(level, T,
                                                      config_.saOmega, config_.saDegree);
            SparseMatrix R = P.transpose();
            R.makeCompressed();
            levels_[l].P = std::move(P);
            levels_[l].R = std::move(R);
            level = galerkinProductPAP(levels_[l].P, level);
        } else {
            level = coarsenByAggregation(level, aggMaps_[l], aggSizes_[l]);
        }
    }
    for (std::size_t l = 0; l < levels_.size(); ++l)
        buildDiagCache_(levels_[l]);
}

bool AggregationBackend::aggregationVcycle_(int level, const Eigen::VectorXd& b,
                                             Eigen::VectorXd& x) const {
    if (level < 0 || static_cast<std::size_t>(level) >= levels_.size())
        return false;
    // Per-level timing for the verbose diagnostic (accumulated on every return
    // path via this scope guard; levelSeconds_ is mutable).
    const auto tLvl0 = std::chrono::steady_clock::now();
    struct LevelTimer {
        const AggregationBackend* self;
        int level;
        std::chrono::steady_clock::time_point t0;
        ~LevelTimer() {
            if (level >= 0 && level < static_cast<int>(self->levelSeconds_.size()))
                self->levelSeconds_[level] += std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - t0).count();
        }
    } levelTimer{this, level, tLvl0};
    const auto& lev = levels_[static_cast<std::size_t>(level)];
    const auto& A = lev.ARM;
    const int n = static_cast<int>(A.rows());
    if (n == 0 || b.size() != n || x.size() != n) return false;

    // Respect the configured sweeps (scalar default: pre=0, post=1).
    const int pre = std::max(0, config_.preSweeps);
    const int post = std::max(0, config_.postSweeps);

    for (int s = 0; s < pre; ++s) gaussSeidel(lev, b, x, 1, true);

    const bool hasCoarse = static_cast<std::size_t>(level) < aggMaps_.size();
    if (hasCoarse) {
        const auto& P = levels_[static_cast<std::size_t>(level)].P;
        const auto& R = levels_[static_cast<std::size_t>(level)].R;
        const int nCoarse = aggSizes_[static_cast<std::size_t>(level)];
        Eigen::VectorXd residual = b - A * x;
        Eigen::VectorXd coarseRhs;
        if (P.cols() > 0 && R.rows() > 0 && P.cols() == nCoarse) {
            coarseRhs = R * residual;
        } else {
            const auto& agg = aggMaps_[static_cast<std::size_t>(level)];
            coarseRhs = Eigen::VectorXd::Zero(nCoarse);
            for (int i = 0; i < n; ++i)
                coarseRhs[agg[static_cast<std::size_t>(i)]] += residual[i];
        }
        Eigen::VectorXd coarseCorr = Eigen::VectorXd::Zero(nCoarse);
        if (!aggregationVcycle_(level + 1, coarseRhs, coarseCorr)) return false;
        if (P.cols() > 0 && P.cols() == nCoarse) {
            x += P * coarseCorr;
        } else {
            const auto& agg = aggMaps_[static_cast<std::size_t>(level)];
            for (int i = 0; i < n; ++i)
                x[i] += coarseCorr[agg[static_cast<std::size_t>(i)]];
        }
    } else {
        Eigen::MatrixXd dense(n, n);
        for (int row = 0; row < n; ++row) {
            dense.row(row).setZero();
            for (AggRowMatrix::InnerIterator it(A, row); it; ++it)
                dense(row, it.col()) = it.value();
        }
        const Eigen::FullPivLU<Eigen::MatrixXd> factor(dense);
        if (!factor.isInvertible()) return false;
        x = factor.solve(b);
        if (!x.allFinite()) return false;
        return true;
    }

    for (int s = 0; s < post; ++s) gaussSeidel(lev, b, x, 1, false);   // backward (symmetric GS)
    return x.allFinite();
}

bool AggregationBackend::solveAggregation_(const Eigen::VectorXd& b, Eigen::VectorXd& x,
                                            const std::atomic<bool>* cancel) const {
    if (!built_ || levels_.empty()) return false;
    if (static_cast<int>(x.size()) != n_) { x.resize(n_); x.setZero(); }
    if (!b.allFinite()) {
        std::cerr << "  [solveAgg] b NON-FINITE (n=" << n_ << ") bnorm=" << b.norm()
                  << " max|b|=" << b.cwiseAbs().maxCoeff()
                  << " max|x|=" << (x.size() ? x.cwiseAbs().maxCoeff() : 0.0) << "\n";
        return false;
    }
    const double bnorm = b.norm();
    if (!std::isfinite(bnorm)) {
        std::cerr << "  [solveAgg] bnorm NON-FINITE\n";
        return false;
    }
    if (bnorm < 1e-30) { x.setZero(); return true; }

    const int maxIt = std::max(1, config_.maxIterations);
    const double tol = std::max(1e-16, config_.tolerance);

    lastResidualHistory_.clear();
    lastResidualHistory_.push_back((b - levels_[0].ARM * x).norm() / bnorm);
    lastIterations_ = 0;
    lastResidual_ = (b - levels_[0].ARM * x).norm() / bnorm;

    if (config_.cycleType == AmgSettings::CycleType::Flexible &&
        config_.acceleration == AmgSettings::Acceleration::None) {
        return solveFlexible_(b, x, cancel);
    }

    Eigen::VectorXd r = b - levels_[0].ARM * x;
    Eigen::VectorXd z(n_);
    for (int iter = 0; iter < maxIt; ++iter) {
        if (cancel && cancel->load()) return false;
        z.setZero();
        if (!aggregationVcycle_(0, r, z)) return false;
        if (!z.allFinite()) return false;
        x += z;
        r = b - levels_[0].ARM * x;
        if (!r.allFinite()) return false;
        const double rel = r.norm() / bnorm;
        lastResidualHistory_.push_back(rel);
        lastIterations_ = iter + 1;
        lastResidual_ = rel;
        if (rel < tol) return true;
        if (iter >= config_.stagnationMinIter &&
            rel > lastResidualHistory_[static_cast<std::size_t>(iter)] *
                      config_.stagnationFactor) {
            return rel < tol * config_.stagnationLooseFactor;
        }
    }
    return lastResidual_ < tol * config_.stagnationLooseFactor;
}

void AggregationBackend::printVerbose(const char* name, std::ostream& os) const {
    const int cycles = std::max(1, config_.flexMaxSweeps);
    const int pre = std::max(0, config_.preSweeps);
    const int post = std::max(0, config_.postSweeps);

    os << "\nSolving " << name << "...\n";
    os << "\nIteration Relative Residuals\n";
    os << "--------- --------------------\n";
    const std::size_t nh = lastResidualHistory_.size();
    for (std::size_t i = 0; i < nh; ++i) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%9zu % .6e\n", i, lastResidualHistory_[i]);
        os << buf;
    }
    os << "\nTotal number AMG iterations : " << lastIterations_ << "\n";

    os << "\nLevel       Size nCycles nPreSmooths nPostSmooths "
          "Elapsed Time (s) Elapsed Time pec (ms)\n";
    os << "----- ---------- ------- ----------- ------------ "
          "---------------- ---------------------\n";
    double totalSolve = 0.0;
    std::size_t maxMem = 0;
    for (int l = 0; l < nlevels_; ++l) {
        const int sz = (l < static_cast<int>(levelSizes_.size())) ? levelSizes_[l] : 0;
        const double el = (l < static_cast<int>(levelSeconds_.size())) ? levelSeconds_[l] : 0.0;
        totalSolve += el;
        const double pec = (sz > 0 && cycles > 0) ? (el * 1000.0 / (cycles * sz)) : 0.0;
        char buf[192];
        std::snprintf(buf, sizeof(buf), "%5d %10d %7d %11d %12d %17.6f %21.6f\n",
                      l, sz, cycles, pre, post, el, pec);
        os << buf;
        if (l < static_cast<int>(levels_.size())) {
            const auto& lv = levels_[l];
            std::size_t m = static_cast<std::size_t>(lv.ARM.nonZeros()) * (sizeof(double) + sizeof(int));
            m += static_cast<std::size_t>(lv.P.nonZeros()) * (sizeof(double) + sizeof(int));
            m += static_cast<std::size_t>(lv.R.nonZeros()) * (sizeof(double) + sizeof(int));
            m += static_cast<std::size_t>(lv.AT.nonZeros()) * (sizeof(double) + sizeof(int));
            m += lv.invDiag.size() * sizeof(double) + lv.diagPos.size() * sizeof(int);
            maxMem += m;
        }
    }
    os << " - pec = \"Per Equation Cycle\"\n";

    os << "\nTotal component times (incl comm):\n";
    char tbuf[256];
    std::snprintf(tbuf, sizeof(tbuf),
                  "  Building levels          : %14.6f(s)\n"
                  "  Solving levels           : %14.6f(s)\n"
                  "  Krylov solve time        : %14.6f(s)\n"
                  "  Total solver time        : %14.6f(s)\n",
                  buildSeconds_, solveSeconds_, solveSeconds_,
                  buildSeconds_ + solveSeconds_);
    os << tbuf;

    std::snprintf(tbuf, sizeof(tbuf),
                  "\nMaximum AMG Memory (all processes): %zu bytes\n", maxMem);
    os << tbuf;
}

bool AggregationBackend::applyFlexibleFineCycle_(const Eigen::VectorXd& b,
                                                  Eigen::VectorXd& x) const {
    const int n = n_;
    if ((int)x.size() != n) { x.resize(n); x.setZero(); }
    if (!b.allFinite()) return false;
    const double bnorm = b.norm();
    if (!std::isfinite(bnorm) || bnorm < 1e-30) return true;

    const int sweeps = std::max(1, config_.flexMaxSweeps);
    const double r0 = (b - levels_[0].ARM * x).norm();
    if (!std::isfinite(r0) || r0 == 0.0) return true;
    const double gate = config_.flexAlpha * r0;
    for (int s = 0; s < sweeps; ++s) {
        fineGaussSeidel_(b, x, 1);
        const double res = (b - levels_[0].ARM * x).norm();
        if (!std::isfinite(res)) { x.setZero(); return false; }
        if (res < gate) break;
    }
    return x.allFinite();
}

bool AggregationBackend::solveFlexibleFine_(const Eigen::VectorXd& b, Eigen::VectorXd& x,
                                             const std::atomic<bool>* cancel) const {
    if (static_cast<int>(x.size()) != n_) { x.resize(n_); x.setZero(); }
    if (!b.allFinite()) return false;
    const double bnorm = b.norm();
    if (!std::isfinite(bnorm)) return false;
    if (bnorm < 1e-30) { x.setZero(); return true; }

    const int maxIt = std::max(1, config_.maxIterations);
    const double tol = std::max(1e-16, config_.tolerance);
    const double r0 = (b - levels_[0].ARM * x).norm();
    double prev = std::numeric_limits<double>::max();
    lastResidualHistory_.clear();
    lastResidualHistory_.push_back(r0 / bnorm);
    lastIterations_ = 0;
    lastResidual_ = r0 / bnorm;
    for (int iter = 0; iter < maxIt; ++iter) {
        if (cancel && cancel->load()) return false;
        if (!applyFlexibleFineCycle_(b, x)) return false;
        const double rel = (b - levels_[0].ARM * x).norm() / bnorm;
        lastResidualHistory_.push_back(rel);
        lastIterations_ = iter + 1;
        lastResidual_ = rel;
        if (rel < tol) return true;
        if (rel > prev * config_.stagnationFactor && iter >= config_.stagnationMinIter)
            return rel < tol * config_.stagnationLooseFactor;
        prev = rel;
    }
    lastResidual_ = (b - levels_[0].ARM * x).norm() / bnorm;
    return lastResidual_ < tol * config_.stagnationLooseFactor;
}

bool AggregationBackend::solve(const Eigen::VectorXd& b, Eigen::VectorXd& x,
                               const std::atomic<bool>* cancel) {
    if (!built_) return false;
    const auto tSolve0 = std::chrono::steady_clock::now();
    const bool ok = solveAggregation_(b, x, cancel);
    solveSeconds_ += std::chrono::duration<double>(
        std::chrono::steady_clock::now() - tSolve0).count();
    return ok;
}

bool AggregationBackend::applyCycle(const Eigen::VectorXd& b, Eigen::VectorXd& x) const {
    if (!built_) return false;
    if (config_.cycleType == AmgSettings::CycleType::Flexible)
        return applyFlexibleCycle_(b, x);
    return aggregationVcycle_(0, b, x);
}

bool AggregationBackend::applyFlexibleCycle_(const Eigen::VectorXd& b, Eigen::VectorXd& x,
                                             double* outResNorm) const {
    if (!built_ || levels_.empty()) return false;
    if (static_cast<int>(x.size()) != n_) { x.resize(n_); x.setZero(); }
    double res = 0.0;
    flexCycleImpl_(0, b, x, &res);
    if (outResNorm) *outResNorm = res;
    return x.allFinite();
}

Eigen::VectorXd AggregationBackend::flexCycle(int level, const Eigen::VectorXd& b,
                                               Eigen::VectorXd& x) const {
    // Interface entry point: the cycle updates x in place. Every caller that
    // uses the return value (the recursion) passes a zero initial guess, so the
    // correction is x itself and no x - xStart copy is needed.
    flexCycleImpl_(level, b, x, nullptr);
    return x;
}

void AggregationBackend::flexCycleImpl_(int level, const Eigen::VectorXd& b,
                                        Eigen::VectorXd& x, double* outResNorm) const {
    if (level < 0 || static_cast<std::size_t>(level) >= levels_.size()) {
        if (outResNorm) *outResNorm = std::numeric_limits<double>::infinity();
        return;
    }
    const AMGLevel& lev = levels_[static_cast<std::size_t>(level)];
    const AggRowMatrix& A = lev.ARM;
    const int n = static_cast<int>(A.rows());
    if (n == 0 || b.size() != n || x.size() != n) {
        if (outResNorm) *outResNorm = std::numeric_limits<double>::infinity();
        return;
    }

    // One matvec per level entry, not per sweep: r is maintained incrementally
    // from here on by gaussSeidelResidual_.
    Eigen::VectorXd r = b - A * x;
    double res = r.norm();
    const double r0 = res;
    if (!std::isfinite(res) || res == 0.0) {
        if (outResNorm) *outResNorm = res;
        return;
    }

    //   Restriction Tolerance (flexAlpha, default 0.7) = β: if R_i > β*R_{i-1},
    //     residual not dropping fast enough → go coarser.
    //   Prolongation Tolerance (flexBeta, default 0.1) = α: if R_i < α*R_0,
    //     level is sufficiently resolved → accept and return to finer grid.
    const double restrictionTol = config_.flexAlpha;   // β
    const double prolongationTol = config_.flexBeta;   // α

    // i_max = sweeps for this level (fine vs coarse). The loop below mirrors
    // the reference variable-cycle algorithm: relax, i++, terminate on α or
    // i > i_max, and when the restriction gate fires apply the coarse
    // correction and KEEP sweeping (not return).
    const int maxSweeps = std::max(
        0, (level == 0) ? config_.flexMaxFineRelaxations
                        : config_.flexMaxCoarseRelaxations);

    const bool hasTranspose = lev.AT.rows() == n && lev.AT.nonZeros() > 0;
    const bool canCoarsen =
        level < nlevels_ - 1 &&
        levelSizes_[static_cast<std::size_t>(level + 1)] > 0;

    double rPrev = r0;
    int i = 0;
    while (true) {
        if (hasTranspose)
            gaussSeidelResidual_(lev, b, x, r, 1, true);
        else {
            gaussSeidel(lev, b, x, 1, true);
            r = b - A * x;
        }
        ++i;
        res = r.norm();
        if (!std::isfinite(res)) {
            // DBG: overflow diagnostic -- which level blew up and how large.
            std::cerr << "  [AMG overflow] level=" << level << " n=" << n
                      << " sweep=" << i << " max|x|="
                      << (x.size() ? x.cwiseAbs().maxCoeff() : 0.0)
                      << " max|r|=" << (r.size() ? r.cwiseAbs().maxCoeff() : 0.0)
                      << " max|b|=" << (b.size() ? b.cwiseAbs().maxCoeff() : 0.0) << "\n";
            x.setZero();
            if (outResNorm) *outResNorm = res;
            return;
        }

        // Prolongation / termination: level resolved (R_i < alpha*R_0), or the
        // sweep budget is spent. R_i < alpha*R_0 with alpha = 0.1.
        if (res < prolongationTol * r0 || i >= maxSweeps) {
            if (outResNorm) *outResNorm = res;
            return;
        }

        // Restriction: R_i > beta*R_{i-1} -> visit the coarser level.
        // rPrev must be R_{i-1} = the residual after the PREVIOUS relaxation,
        // i.e. the pre-correction residual of this sweep (NOT the post-coarse-
        // correction residual, which would double-count the coarse effect).
        const double resThisSweep = res;   // R_i
        if (res > restrictionTol * rPrev && canCoarsen) {
            coarsenAndProlong_(level, r, x);
            // x changed: the incrementally maintained residual is now stale.
            r = b - A * x;
            res = r.norm();
            if (!std::isfinite(res)) {
                x.setZero();
                if (outResNorm) *outResNorm = res;
                return;
            }
        }

        rPrev = resThisSweep;   // R_i for the next restriction gate
    }
}

void AggregationBackend::coarsenAndProlong_(int level, const Eigen::VectorXd& r,
                                            Eigen::VectorXd& x) const {
    const auto& lev = levels_[static_cast<std::size_t>(level)];
    const AggRowMatrix& A = lev.ARM;
    const int n = static_cast<int>(A.rows());
    const int nCoarse = levelSizes_[static_cast<std::size_t>(level + 1)];
    if (nCoarse <= 0) return;

    const auto& P = lev.P;
    const auto& R = lev.R;
    const bool matrixPath = P.cols() > 0 && R.rows() > 0 && P.cols() == nCoarse;

    if (static_cast<std::size_t>(level) >= flexRCoarseScratch_.size()) {
        flexRCoarseScratch_.resize(static_cast<std::size_t>(level) + 1);
        flexXCoarseScratch_.resize(static_cast<std::size_t>(level) + 1);
    }
    Eigen::VectorXd& rCoarse = flexRCoarseScratch_[static_cast<std::size_t>(level)];
    Eigen::VectorXd& xCoarse = flexXCoarseScratch_[static_cast<std::size_t>(level)];

    if (matrixPath) {
        rCoarse = R * r;
    } else {
        rCoarse.setZero(nCoarse);
        const auto& agg = aggMaps_[static_cast<std::size_t>(level)];
        for (int i = 0; i < n; ++i)
            rCoarse[agg[static_cast<std::size_t>(i)]] += r[i];
    }

    // The coarse solve starts from zero, so its result IS the correction.
    xCoarse.setZero(nCoarse);
    flexCycleImpl_(level + 1, rCoarse, xCoarse, nullptr);

    if (matrixPath) {
        x += P * xCoarse;
    } else {
        const auto& agg = aggMaps_[static_cast<std::size_t>(level)];
        for (int i = 0; i < n; ++i)
            x[i] += xCoarse[agg[static_cast<std::size_t>(i)]];
    }
}

bool AggregationBackend::solveFlexible_(const Eigen::VectorXd& b, Eigen::VectorXd& x,
                                        const std::atomic<bool>* cancel) const {
    const int n = n_;
    if ((int)x.size() != n) { x.resize(n); x.setZero(); }
    if (!b.allFinite()) {
        std::cerr << "  [flex solve] b NON-FINITE (bnorm=" << b.norm()
                  << " max|b|=" << b.cwiseAbs().maxCoeff() << ")\n";
        return false;
    }
    const double bnorm = b.norm();
    if (!std::isfinite(bnorm)) {
        std::cerr << "  [flex solve] bnorm NON-FINITE\n";
        return false;
    }
    if (bnorm < 1e-30) { x.setZero(); return true; }
    const int maxIt = std::max(1, config_.maxIterations);
    const double tol = std::max(1e-16, config_.tolerance);
    double res = (b - levels_[0].ARM * x).norm();
    double prev = std::numeric_limits<double>::max();
    lastResidualHistory_.clear();
    lastResidualHistory_.push_back(res / bnorm);
    lastIterations_ = 0; lastResidual_ = res / bnorm;
    for (int iter = 0; iter < maxIt; ++iter) {
        if (cancel && cancel->load()) return false;
        // The cycle reports the residual it achieved, so no extra matvec here.
        if (!applyFlexibleCycle_(b, x, &res)) {
            std::cerr << "  [flex solve] applyFlexibleCycle_ FAILED at iter=" << iter
                      << " res=" << res << " max|x|=" << x.cwiseAbs().maxCoeff() << "\n";
            return false;
        }
        const double rel = res / bnorm;
        lastResidualHistory_.push_back(rel);
        lastIterations_ = iter + 1; lastResidual_ = rel;
        if (rel < tol) {
            return true;
        }
        if (res > prev * config_.stagnationFactor && iter >= config_.stagnationMinIter) {
            bool ok = rel < tol * config_.stagnationLooseFactor;
            std::cerr << "  [flex solve] stagnation at iter=" << iter << " rel=" << rel
                      << " tol*loose=" << tol * config_.stagnationLooseFactor << " ok=" << ok << "\n";
            return ok;
        }
        prev = res;
    }
    res = (b - levels_[0].ARM * x).norm();
    lastResidual_ = res / bnorm;
    return lastResidual_ < tol * config_.stagnationLooseFactor;
}

void AggregationBackend::fineGaussSeidel_(const Eigen::VectorXd& b, Eigen::VectorXd& x,
                                          int sweeps) const {
    if (levels_.empty()) return;
    gaussSeidel(levels_[0], b, x, sweeps, true);
}

void AggregationBackend::gaussSeidel(const AMGLevel& level,
                                     const Eigen::VectorXd& b, Eigen::VectorXd& x,
                                     int numSweeps, bool forward) const {
    const AggRowMatrix& A = level.ARM;
    const int n = static_cast<int>(A.rows());
    if (static_cast<int>(level.invDiag.size()) != n) return;
    const int* outer = A.outerIndexPtr();
    const int* inner = A.innerIndexPtr();
    const double* vals = A.valuePtr();
    const double* invDiag = level.invDiag.data();
    const int* diagPos = level.diagPos.data();
    const double* bp = b.data();
    double* xp = x.data();

    for (int s = 0; s < numSweeps; ++s) {
        if (forward) {
            for (int i = 0; i < n; ++i) {
                const double inv = invDiag[i];
                const int dp = diagPos[i];
                if (inv == 0.0 || dp < 0) continue;
                if (!std::isfinite(bp[i])) continue;
                const int ib = outer[i];
                const int len = outer[i + 1] - ib;
                const int* idx = inner + ib;
                const double* v = vals + ib;
                double sum = 0.0;
                // The cached diagonal index splits the row: the off-diagonal
                // sum keeps the exact accumulation order of the old
                // col()!=i loop, with no per-entry branch.
                for (int k = 0; k < dp; ++k) sum += v[k] * xp[idx[k]];
                for (int k = dp + 1; k < len; ++k) sum += v[k] * xp[idx[k]];
                if (!std::isfinite(sum)) continue;
                xp[i] = (bp[i] - sum) * inv;
            }
        } else {
            for (int i = n - 1; i >= 0; --i) {
                const double inv = invDiag[i];
                const int dp = diagPos[i];
                if (inv == 0.0 || dp < 0) continue;
                if (!std::isfinite(bp[i])) continue;
                const int ib = outer[i];
                const int len = outer[i + 1] - ib;
                const int* idx = inner + ib;
                const double* v = vals + ib;
                double sum = 0.0;
                for (int k = 0; k < dp; ++k) sum += v[k] * xp[idx[k]];
                for (int k = dp + 1; k < len; ++k) sum += v[k] * xp[idx[k]];
                if (!std::isfinite(sum)) continue;
                xp[i] = (bp[i] - sum) * inv;
            }
        }
    }
}

void AggregationBackend::gaussSeidelResidual_(const AMGLevel& level,
                                              const Eigen::VectorXd& b,
                                              Eigen::VectorXd& x, Eigen::VectorXd& r,
                                              int numSweeps, bool forward) const {
    const AggRowMatrix& A = level.ARM;
    const int n = static_cast<int>(A.rows());
    if (static_cast<int>(level.invDiag.size()) != n) return;
    const int* outer = A.outerIndexPtr();
    const int* inner = A.innerIndexPtr();
    const double* vals = A.valuePtr();
    const double* invDiag = level.invDiag.data();
    const int* diagPos = level.diagPos.data();
    // Row i of AT is column i of A: relaxing x[i] by delta updates
    // r[j] -= A[j,i] * delta for every j, in one O(nnz) sweep.
    const int* tOuter = level.AT.outerIndexPtr();
    const int* tInner = level.AT.innerIndexPtr();
    const double* tVals = level.AT.valuePtr();
    const double* bp = b.data();
    double* xp = x.data();
    double* rp = r.data();

    for (int s = 0; s < numSweeps; ++s) {
        if (forward) {
            for (int i = 0; i < n; ++i) {
                const double inv = invDiag[i];
                const int dp = diagPos[i];
                if (inv == 0.0 || dp < 0) continue;
                if (!std::isfinite(bp[i])) continue;
                const int ib = outer[i];
                const int len = outer[i + 1] - ib;
                const int* idx = inner + ib;
                const double* v = vals + ib;
                double sum = 0.0;
                for (int k = 0; k < dp; ++k) sum += v[k] * xp[idx[k]];
                for (int k = dp + 1; k < len; ++k) sum += v[k] * xp[idx[k]];
                if (!std::isfinite(sum)) continue;
                const double xNew = (bp[i] - sum) * inv;
                const double delta = xNew - xp[i];
                xp[i] = xNew;
                for (int k = tOuter[i]; k < tOuter[i + 1]; ++k)
                    rp[tInner[k]] -= tVals[k] * delta;
            }
        } else {
            for (int i = n - 1; i >= 0; --i) {
                const double inv = invDiag[i];
                const int dp = diagPos[i];
                if (inv == 0.0 || dp < 0) continue;
                if (!std::isfinite(bp[i])) continue;
                const int ib = outer[i];
                const int len = outer[i + 1] - ib;
                const int* idx = inner + ib;
                const double* v = vals + ib;
                double sum = 0.0;
                for (int k = 0; k < dp; ++k) sum += v[k] * xp[idx[k]];
                for (int k = dp + 1; k < len; ++k) sum += v[k] * xp[idx[k]];
                if (!std::isfinite(sum)) continue;
                const double xNew = (bp[i] - sum) * inv;
                const double delta = xNew - xp[i];
                xp[i] = xNew;
                for (int k = tOuter[i]; k < tOuter[i + 1]; ++k)
                    rp[tInner[k]] -= tVals[k] * delta;
            }
        }
    }
}

void AggregationBackend::smoothPre_(const AMGLevel& level,
                                    const Eigen::VectorXd& b, Eigen::VectorXd& x, int numSweeps) const {
    gaussSeidel(level, b, x, numSweeps, true);
}

void AggregationBackend::smoothPost_(const AMGLevel& level,
                                     const Eigen::VectorXd& b, Eigen::VectorXd& x, int numSweeps) const {
    gaussSeidel(level, b, x, numSweeps, false);   // backward (symmetric GS)
}

int AggregationBackend::aggregate_(const AggRowMatrix& A, int groupSize,
                                    std::vector<int>& agg) const {
    return aggregateGreedy(A, groupSize, agg);
}

// ============================================================================
// AMGHierarchy implementation
// ============================================================================

AMGHierarchy::AMGHierarchy(Config config, int blockDim)
    : backend_(new AggregationBackend(config, blockDim)) {}

AMGHierarchy::~AMGHierarchy() = default;

} // namespace fvm
