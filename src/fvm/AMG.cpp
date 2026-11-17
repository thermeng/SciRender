// Hypre-only AMGHierarchy — amgcl removed
#include "fvm/AMG.h"
#include "fvm/HypreSolver.h"
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace fvm {

using cfd::AmgSettings;

// ============================================================================
// BoomerAMGBackend — stub (full implementation migrated from AMGHierarchy below)
// ============================================================================
class BoomerAMGBackend : public AMGBackend {
public:
    explicit BoomerAMGBackend(Config config, int blockDim)
        : config_(config), blockDim_(std::max(1, blockDim)) {}

    void setConfig(const Config& config) override { config_ = config; }
    const Config& config() const override { return config_; }

    void build(const SparseMatrix& A) override { /* TODO */ }
    void invalidate() override { /* TODO */ }
    bool isBuilt() const override { return false; }

    bool solve(const Eigen::VectorXd& b, Eigen::VectorXd& x,
               const std::atomic<bool>* cancel = nullptr) override { return false; }
    bool applyCycle(const Eigen::VectorXd& b, Eigen::VectorXd& x) const override { return false; }
    Eigen::VectorXd flexCycle(int level, const Eigen::VectorXd& b,
                              Eigen::VectorXd& x) const override { return {}; }

    int numLevels() const override { return 0; }
    int fineSize() const override { return 0; }
    const std::vector<int>& levelSizes() const override { return emptyLevels_; }
    std::string hierarchyReport() const override { return "BoomerAMG (stub)"; }
    int lastIterations() const override { return 0; }
    double lastResidual() const override { return 0.0; }
    const std::vector<double>& lastResidualHistory() const override { return emptyHistory_; }
    double buildTime() const override { return 0.0; }
    double solveTime() const override { return 0.0; }

    bool hasCachedAggregation() const override { return false; }
    bool hierarchyCurrent() const override { return false; }
    void refreshHierarchyValues(const SparseMatrix& A) override {}
    void updateFineValues(const SparseMatrix& A) override {}

private:
    Config config_;
    int blockDim_ = 2;
    std::vector<int> emptyLevels_;
    std::vector<double> emptyHistory_;
};

// ============================================================================
// AggregationBackend — stub (full implementation migrated from AMGHierarchy below)
// ============================================================================
class AggregationBackend : public AMGBackend {
public:
    explicit AggregationBackend(Config config, int blockDim)
        : config_(config), blockDim_(std::max(1, blockDim)) {}

    void setConfig(const Config& config) override { config_ = config; }
    const Config& config() const override { return config_; }

    void build(const SparseMatrix& A) override { /* TODO */ }
    void invalidate() override { /* TODO */ }
    bool isBuilt() const override { return false; }

    bool solve(const Eigen::VectorXd& b, Eigen::VectorXd& x,
               const std::atomic<bool>* cancel = nullptr) override { return false; }
    bool applyCycle(const Eigen::VectorXd& b, Eigen::VectorXd& x) const override { return false; }
    Eigen::VectorXd flexCycle(int level, const Eigen::VectorXd& b,
                              Eigen::VectorXd& x) const override { return {}; }

    int numLevels() const override { return 0; }
    int fineSize() const override { return 0; }
    const std::vector<int>& levelSizes() const override { return emptyLevels_; }
    std::string hierarchyReport() const override { return "Cached Aggregation (stub)"; }
    int lastIterations() const override { return 0; }
    double lastResidual() const override { return 0.0; }
    const std::vector<double>& lastResidualHistory() const override { return emptyHistory_; }
    double buildTime() const override { return 0.0; }
    double solveTime() const override { return 0.0; }

    bool hasCachedAggregation() const override { return true; }
    bool hierarchyCurrent() const override { return false; }
    void refreshHierarchyValues(const SparseMatrix& A) override {}
    void updateFineValues(const SparseMatrix& A) override {}

private:
    Config config_;
    int blockDim_ = 2;
    std::vector<int> emptyLevels_;
    std::vector<double> emptyHistory_;
};

AMGHierarchy::AMGHierarchy(Config config, int blockDim)
    : config_(config), blockDim_(std::max(1, blockDim)) {
    if (config.backend == Config::Backend::CachedAggregation) {
        backend_.reset(new AggregationBackend(config, blockDim));
    } else {
        backend_.reset(new BoomerAMGBackend(config, blockDim));
    }
}

AMGHierarchy::~AMGHierarchy() = default;

void AMGHierarchy::build(const SparseMatrix& A) {
    cgrid_.clear(); built_ = false; amgHandle_.reset();
    hypreMat_.reset(); hypreSolver_.reset();
    fineRows_.resize(0, 0); fineRowsReady_ = false; lazyBuild_ = false; hypreHierarchyReady_ = false;
    hierarchyInfoReady_ = false;
    n_ = 0; nlevels_ = 0; levelSizes_.clear(); report_.clear();
    systemMatrix_ = A;
    if (A.rows() == 0 || A.cols() == 0) { buildTimeSec_ = 0.0; return; }
    n_ = static_cast<int>(A.rows());
    auto buildStart = std::chrono::steady_clock::now();
    // Cached aggregation backend: no Hypre setup, no value-dependent
    // coarsening. The maps are built once per sparsity pattern; later
    // iterations refresh only the level operators (O(nnz)).
    if (useAggregation()) { buildAggregation_(A); return; }
#ifdef FVM_HAVE_HYPRE
    auto mat = std::make_shared<typename HypreSolver::HypreMat>();
    auto solv = std::make_shared<typename HypreSolver::HypreSolverImpl>();
    std::vector<double> sc;
    if (HypreSolver::buildSolver(A, config_, *mat, *solv, sc)) {
        hypreMat_ = mat; hypreSolver_ = solv; hypreScale_ = sc; useHypreActive_ = true;
        hierarchyInfoReady_ = false;
        // Only the Flexible cycle needs the C/F split to reconstruct its
        // per-level operators. Other cycles (V/W/F) run natively in Hypre and
        // must not pay for a metadata query inside the time-step loop.
        if (config_.cycleType == AmgSettings::CycleType::Flexible) {
            ensureHierarchyInfo_();
            buildLevelOperators_();
        }
        if (config_.verbosity >= AmgSettings::Verbosity::High) {
            ensureHierarchyInfo_();
            std::cerr << "  AMG hierarchy (" << n_ << " unknowns, Hypre)\n" << report_;
        }
        if (config_.verbosity >= AmgSettings::Verbosity::High) {
            std::cerr << "  AMG hierarchy (" << n_ << " unknowns, Hypre)\n" << report_;
        }
        if (config_.verbosity == AmgSettings::Verbosity::Diagnosis) {
            std::cerr << "  AMG diagnosis: " << n_ << " unknowns, Hypre\n"
                      << "    coarsening=smoothed aggregation, smoother="
                      << ((config_.smoother == AmgSettings::Smoother::Jacobi) ? "Jacobi" : "Gauss-Seidel")
                      << ", cycle=" << (config_.cycleType == AmgSettings::CycleType::WCycle ? "W" : config_.cycleType == AmgSettings::CycleType::FCycle ? "F" : config_.cycleType == AmgSettings::CycleType::Flexible ? "Flexible" : "V")
                      << ", coarse solve=direct (<= " << config_.coarseEnough << " equations)\n";
        }
        buildTimeSec_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - buildStart).count();
        built_ = true;
        lazyBuild_ = false;
        hypreHierarchyReady_ = true;
        return;
    }
    std::cerr << "  AMG: Hypre build failed\n";
#endif
    buildTimeSec_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - buildStart).count();
    built_ = false;
}

bool AMGHierarchy::applyCycle(const Eigen::VectorXd& b, Eigen::VectorXd& x) const {
    if (!config_.flexStruct.empty() && config_.flexStruct[0] == static_cast<int>(AmgSettings::FlexMove::Terminate)) return true;
    if (!built_) return false;
    if (useAggregation()) {
        const bool flexFineOnly =
            config_.cycleType == AmgSettings::CycleType::Flexible &&
            config_.acceleration == AmgSettings::Acceleration::None;
        if (flexFineOnly) return applyFlexibleFineCycle_(b, x);
        return aggregationVcycle_(0, b, x);
    }
    if (!useHypreActive_) return false;
    // Flexible cycles are residual-gated: fine sweep first, coarse correction
    // only when the restriction tolerance asks for it.
    if (config_.cycleType == AmgSettings::CycleType::Flexible) return applyFlexibleCycle_(b, x);
    if (!hypreMat_ || !hypreSolver_) return false;
    auto mat = std::static_pointer_cast<HypreSolver::HypreMat>(hypreMat_);
    auto solv = std::static_pointer_cast<HypreSolver::HypreSolverImpl>(hypreSolver_);
    if (!mat || !solv) return false;
    return HypreSolver::applyCycle(*mat, *solv, b, x, hypreScale_);
}

// Query BoomerAMG's grid hierarchy on first use. Kept out of build() so a
// V/W/F-cycle run never calls a diagnostic getter in the time-step loop.
void AMGHierarchy::ensureHierarchyInfo_() const {
    if (hierarchyInfoReady_) return;
    hierarchyInfoReady_ = true; // one attempt per build; do not retry on failure
#ifdef FVM_HAVE_HYPRE
    auto solv = std::static_pointer_cast<HypreSolver::HypreSolverImpl>(hypreSolver_);
    if (solv && n_ > 0) {
        std::vector<int> cgrid;
        if (!HypreSolver::hierarchyLevelSizes(*solv, n_, levelSizes_, cgrid))
            levelSizes_.assign(1, n_);
        cgrid_ = std::move(cgrid);
    }
    else
#endif
    if (levelSizes_.empty() && n_ > 0) {
        levelSizes_.assign(1, n_);
    }
    nlevels_ = static_cast<int>(levelSizes_.size());
    report_ = "Hypre BoomerAMG (IJ)\n";
    for (int l = 0; l < nlevels_; ++l) {
        report_ += "  level " + std::to_string(l) + ": " +
                   std::to_string(levelSizes_[static_cast<std::size_t>(l)]) +
                   " unknowns\n";
    }
}

bool AMGHierarchy::ensureHypreBuilt_() const {
    if (hypreHierarchyReady_) return true;
#ifdef FVM_HAVE_HYPRE
    auto buildStart = std::chrono::steady_clock::now();
    auto mat = std::make_shared<typename HypreSolver::HypreMat>();
    auto solv = std::make_shared<typename HypreSolver::HypreSolverImpl>();
    std::vector<double> sc;
    if (HypreSolver::buildSolver(systemMatrix_, config_, *mat, *solv, sc)) {
        hypreMat_ = mat; hypreSolver_ = solv; hypreScale_ = sc;
        levelSizes_.clear();
        hierarchyInfoReady_ = false;
        ensureHierarchyInfo_();
        buildLevelOperators_();
        buildTimeSec_ += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - buildStart).count();
        hypreHierarchyReady_ = true;
        return true;
    }
    std::cerr << "  AMG: Hypre build failed (deferred hierarchy)\n";
#endif
    return false;
}

void AMGHierarchy::buildLevelOperators_() const {
    levels_.clear();
    levels_.resize(static_cast<std::size_t>(nlevels_));

    // Guard: cgrid_ must be populated by the Hypre hierarchy query before we
    // index into it. If it is empty there is nothing to build.
    if (static_cast<int>(cgrid_.size()) != n_) return;

    // Level 0 operator: row-major copy for efficient Gauss-Seidel row access.
    {
        Eigen::SparseMatrix<double, Eigen::RowMajor> arm0 = systemMatrix_;
        levels_[0].ARM = arm0;
        levels_[0].ARM.makeCompressed();
        levels_[0].nCoarse = 0; // updated below if there is a next level
    }

    // Build per-level P (prolongation, coarse->fine), R (restriction, fine->coarse),
    // ARM (coarse operator), cfSplit, nCoarse from cgrid_.
    for (int L = 0; L < nlevels_ - 1; ++L) {
        const int nFine = levelSizes_[static_cast<std::size_t>(L)];
        const int nCoarse = levelSizes_[static_cast<std::size_t>(L + 1)];

        // fineNodes: local level-L index -> original id.
        // coarseLocal: original id -> local level-(L+1) index.
        std::vector<int> fineNodes;
        std::vector<int> coarseLocal(n_, -1);
        int coarseSeen = 0;
        for (int i = 0; i < n_; ++i) {
            if (cgrid_[static_cast<std::size_t>(i)] >= L) {
                fineNodes.push_back(i);
            }
            if (cgrid_[static_cast<std::size_t>(i)] >= L + 1) {
                coarseLocal[static_cast<std::size_t>(i)] = coarseSeen++;
            }
        }

        // cfSplit: 1 = C-node (survives to L+1), 0 = F-node (eliminated).
        auto& lev = levels_[static_cast<std::size_t>(L)];
        lev.cfSplit.assign(static_cast<std::size_t>(nFine), 0);
        lev.nCoarse = nCoarse;
        for (int i = 0; i < nFine; ++i) {
            if (cgrid_[static_cast<std::size_t>(fineNodes[static_cast<std::size_t>(i)])] >= L + 1) {
                lev.cfSplit[static_cast<std::size_t>(i)] = 1;
            }
        }

        // Strength of connection on level L matrix. strongNeighbors is keyed
        // by local level-L index (0..nFine-1), NOT by original node ID.
        const Eigen::SparseMatrix<double, Eigen::RowMajor>& A_L = lev.ARM;
        std::vector<std::vector<int>> strongNeighbors;
        strengthOfConnection(A_L, strongNeighbors);

        // Build P (prolongation, coarse->fine): nFine x nCoarse.
        // Fine node i that survives: P(i, i) = 1 (identity).
        // Fine node i eliminated: P(i, parent(i)) = 1 where parent is strongest coarse neighbor.
        std::vector<Eigen::Triplet<double>> triplets;
        triplets.reserve(static_cast<std::size_t>(nFine));
        for (int i = 0; i < nFine; ++i) {
            const int origId = fineNodes[static_cast<std::size_t>(i)];
            if (cgrid_[static_cast<std::size_t>(origId)] >= L + 1) {
                triplets.emplace_back(i, coarseLocal[static_cast<std::size_t>(origId)], 1.0);
            } else {
                int bestParent = -1;
                double bestVal = 0.0;
                for (int nbLocal : strongNeighbors[static_cast<std::size_t>(i)]) {
                    // nbLocal is a local level-L index; map back to original node ID.
                    const int nbOrig = fineNodes[static_cast<std::size_t>(nbLocal)];
                    if (cgrid_[static_cast<std::size_t>(nbOrig)] >= L + 1) {
                        const double val = std::abs(A_L.coeff(i, nbLocal));
                        if (val > bestVal) { bestVal = val; bestParent = nbOrig; }
                    }
                }
                if (bestParent >= 0) {
                    triplets.emplace_back(i, coarseLocal[static_cast<std::size_t>(bestParent)], 1.0);
                } else {
                    // Fallback: no strong coarse neighbor; map to coarse node 0.
                    triplets.emplace_back(i, nCoarse > 0 ? 0 : i, 1.0);
                }
            }
        }

        lev.P.resize(static_cast<Eigen::Index>(nFine), static_cast<Eigen::Index>(nCoarse));
        lev.P.setFromTriplets(triplets.begin(), triplets.end());
        lev.P.makeCompressed();

        // R = P^T: nCoarse x nFine (restriction, fine->coarse).
        lev.R = lev.P.transpose();
        lev.R.makeCompressed();

        // ARM[L+1] = R * A_L * P = P^T * A_L * P (Galerkin product).
        Eigen::SparseMatrix<double> APC = A_L * lev.P;
        levels_[static_cast<std::size_t>(L + 1)].ARM = lev.P.transpose() * APC;
        levels_[static_cast<std::size_t>(L + 1)].ARM.makeCompressed();
    }

    // Coarsest level has no next level.
    if (nlevels_ > 0) {
        levels_[static_cast<std::size_t>(nlevels_ - 1)].nCoarse = 0;
    }
}

bool AMGHierarchy::applyFlexibleCycle_(const Eigen::VectorXd& b, Eigen::VectorXd& x) const {
    if (!built_ || !useHypreActive_) return false;
    if (config_.cycleType != AmgSettings::CycleType::Flexible) return false;
    // Ensure the full hierarchy is available (covers deferred builds).
    if (!hypreHierarchyReady_ && !ensureHypreBuilt_()) return false;
    // Recursive flexible cycle: alpha/beta gates at every level.
    flexCycle(0, b, x);
    return x.allFinite();
}

Eigen::VectorXd AMGHierarchy::flexCycle(int level, const Eigen::VectorXd& b,
                                         Eigen::VectorXd& x) const {
    // Defensive: per-level operators must be built before accessing them.
    if (static_cast<std::size_t>(level) >= levels_.size())
        return Eigen::VectorXd();
    const auto& lev = levels_[static_cast<std::size_t>(level)];
    if (lev.ARM.rows() == 0 || lev.ARM.cols() == 0)
        return Eigen::VectorXd();

    const int n = static_cast<int>(levelSizes_[static_cast<std::size_t>(level)]);
    if (n == 0 || x.size() != static_cast<Eigen::Index>(n)) return Eigen::VectorXd();

    const Eigen::SparseMatrix<double, Eigen::RowMajor>& A =
        levels_[static_cast<std::size_t>(level)].ARM;
    const Eigen::VectorXd xStart = x;

    const double r0 = (b - A * x).norm();
    if (!std::isfinite(r0) || r0 == 0.0) return Eigen::VectorXd::Zero(n);

    const double alpha = config_.flexAlpha;
    const double beta = config_.flexBeta;
    // fine levels use flexMaxFineRelaxations, coarse levels use
    // flexMaxCoarseRelaxations; fall back to flexMaxSweeps if either is 0.
    const int maxSweeps =
        (level == 0 && config_.flexMaxFineRelaxations > 0)
            ? config_.flexMaxFineRelaxations
        : (level > 0 && config_.flexMaxCoarseRelaxations > 0)
            ? config_.flexMaxCoarseRelaxations
            : (config_.flexMaxSweeps > 0 ? config_.flexMaxSweeps : 1);

    double rPrev = r0;
    for (int sweep = 0; sweep < maxSweeps; ++sweep) {
        gaussSeidel(A, b, x, 1, true);
        const double res = (b - A * x).norm();
        if (!std::isfinite(res)) { x.setZero(); return Eigen::VectorXd::Zero(n); }

        // Alpha: residual reduced to alpha * r0 -> converged at this level.
        if (res < alpha * r0) return x - xStart;

        // Beta: residual not dropping -> go coarser.
        if (level < nlevels_ - 1) {
            const auto& nextLev = levels_[static_cast<std::size_t>(level + 1)];
            if (nextLev.ARM.rows() == 0 || lev.R.rows() == 0 || lev.P.cols() == 0)
                return x - xStart; // cannot coarsen: return partial correction

            const Eigen::VectorXd r = b - A * x;
            const Eigen::VectorXd rCoarse = lev.R * r;

            Eigen::VectorXd xCoarse =
                Eigen::VectorXd::Zero(levelSizes_[static_cast<std::size_t>(level + 1)]);
            const Eigen::VectorXd correctionCoarse =
                flexCycle(level + 1, rCoarse, xCoarse);

            x += lev.P * correctionCoarse;

            return x - xStart;
        }

        rPrev = res;
    }

    // Max sweeps reached without alpha/beta trigger: return partial correction.
    return x - xStart;
}

void AMGHierarchy::fineGaussSeidel_(const Eigen::VectorXd& b, Eigen::VectorXd& x,
                                    int sweeps) const {
    if (!fineRowsReady_) { fineRows_ = systemMatrix_; fineRowsReady_ = true; }
    gaussSeidel(fineRows_, b, x, sweeps, true);
}

bool AMGHierarchy::solveFlexible_(const Eigen::VectorXd& b, Eigen::VectorXd& x,
                                  const std::atomic<bool>* cancel) const {
    auto solveStart = std::chrono::steady_clock::now();
    const int n = n_;
    if ((int)x.size() != n) { x.resize(n); x.setZero(); }
    // A non-finite right-hand side is a broken system, not a converged one.
    // Returning a zero correction would hide the NaN and let the caller report
    // a bogus success. Only a genuinely zero RHS has the exact answer 0.
    if (!b.allFinite()) return false;
    const double bnorm = b.norm();
    if (!std::isfinite(bnorm)) return false; // norm overflowed
    if (bnorm < 1e-30) { x.setZero(); return true; }
    const int maxIt = std::max(1, config_.maxIterations);
    const double tol = std::max(1e-16, config_.tolerance);
    const double r0 = (b - systemMatrix_ * x).norm();
    double prev = std::numeric_limits<double>::max();
    lastResidualHistory_.clear();
    // STAR prints an "Iteration Residuals" table starting at cycle 0 (the
    // residual of the incoming guess); mirror that baseline.
    lastResidualHistory_.push_back(r0 / bnorm);
    lastIterations_ = 0; lastResidual_ = r0 / bnorm;
    for (int iter = 0; iter < maxIt; ++iter) {
        if (cancel && cancel->load()) return false;
        if (!applyFlexibleCycle_(b, x)) return false;
        Eigen::VectorXd r = b - systemMatrix_ * x;
        double res = r.norm();
        double rel = res / bnorm;
        lastResidualHistory_.push_back(rel);
        lastIterations_ = iter + 1; lastResidual_ = rel;
        if (rel < tol || (config_.epsilon > 0 && res < config_.epsilon)) {
            solveTimeSec_ = std::chrono::duration<double>(std::chrono::steady_clock::now()-solveStart).count();
            return true;
        }
        if (res > prev * config_.stagnationFactor && iter >= config_.stagnationMinIter) {
            bool ok = rel < tol * config_.stagnationLooseFactor;
            solveTimeSec_ = std::chrono::duration<double>(std::chrono::steady_clock::now()-solveStart).count();
            return ok;
        }
        prev = res;
    }
    Eigen::VectorXd r = b - systemMatrix_ * x;
    lastResidual_ = r.norm() / bnorm;
    solveTimeSec_ = std::chrono::duration<double>(std::chrono::steady_clock::now()-solveStart).count();
    return lastResidual_ < tol * config_.stagnationLooseFactor;
}

bool AMGHierarchy::solve(const Eigen::VectorXd& b, Eigen::VectorXd& x, const std::atomic<bool>* cancel) const {
    if (!built_) return false;
    if (useAggregation()) return solveAggregation_(b, x, cancel);
    if (!useHypreActive_) return false;
    if (config_.cycleType == AmgSettings::CycleType::Flexible) return solveFlexible_(b, x, cancel);
    auto solveStart = std::chrono::steady_clock::now();
    const int n = n_;
    if ((int)x.size() != n) { x.resize(n); x.setZero(); }
    // A non-finite right-hand side is a broken system, not a converged one.
    // Returning a zero correction would hide the NaN and let the caller report
    // a bogus success. Only a genuinely zero RHS has the exact answer 0.
    if (!b.allFinite()) return false;
    const double bnorm = b.norm();
    if (!std::isfinite(bnorm)) return false; // norm overflowed
    if (bnorm < 1e-30) { x.setZero(); return true; }
    const int maxIt = std::max(1, config_.maxIterations);
    const double tol = std::max(1e-16, config_.tolerance);
    double prev = std::numeric_limits<double>::max();
    lastResidualHistory_.clear();
    // Cycle-0 baseline (see solveFlexible_).
    lastResidualHistory_.push_back((b - systemMatrix_ * x).norm() / bnorm);
    for (int iter = 0; iter < maxIt; ++iter) {
        if (cancel && cancel->load()) return false;
        if (!applyCycle(b, x)) return false;
        Eigen::VectorXd r = b - systemMatrix_ * x;
        double res = r.norm();
        double rel = res / bnorm;
        lastResidualHistory_.push_back(rel);
        lastIterations_ = iter + 1; lastResidual_ = rel;
        if (rel < tol || (config_.epsilon > 0 && res < config_.epsilon)) {
            solveTimeSec_ = std::chrono::duration<double>(std::chrono::steady_clock::now()-solveStart).count();
            return true;
        }
        if (res > prev * config_.stagnationFactor && iter >= config_.stagnationMinIter) {
            bool ok = rel < tol * config_.stagnationLooseFactor;
            solveTimeSec_ = std::chrono::duration<double>(std::chrono::steady_clock::now()-solveStart).count();
            return ok;
        }
        prev = res;
    }
    Eigen::VectorXd r = b - systemMatrix_ * x;
    lastResidual_ = r.norm() / bnorm;
    solveTimeSec_ = std::chrono::duration<double>(std::chrono::steady_clock::now()-solveStart).count();
    return lastResidual_ < tol * config_.stagnationLooseFactor;
}

void AMGHierarchy::updateFineValues(const SparseMatrix& A) {
    // Rebuild the hierarchy from scratch. For cached aggregation this reuses
    // the cached maps and refreshes the level operators in O(nnz); for
    // BoomerAMG this does a clean full Hypre rebuild (new IJ matrix, new
    // BoomerAMG solver, fresh BoomerAMGSetup). Using Hypre's
    // updateSolverValues() here would call HYPRE_IJMatrixSetValues on an
    // already-assembled ParCSR matrix, which accumulates values instead of
    // replacing them, and BoomerAMGSetup repeatedly on the same solver object,
    // both of which corrupt the hierarchy over repeated calls.
    build(A);
}
// Preconditioner-role refresh for the BoomerAMG backend: overwrite the values
// of the existing assembled IJ matrix and re-run BoomerAMGSetup on the same
// solver object. The caller must only use this when a Krylov method wraps the
// hierarchy (Acceleration != None) so the stale coarse operators are corrected
// by the outer iteration.
void AMGHierarchy::refreshHierarchyValues(const SparseMatrix& A) {
#ifdef FVM_HAVE_HYPRE
    if (!built_ || !useHypreActive_ || !hypreMat_ || !hypreSolver_) return;
    if (A.rows() == 0 || static_cast<int>(A.cols()) != static_cast<int>(A.rows())) return;
    if (!HypreSolver::updateSolverValues(
            *std::static_pointer_cast<HypreSolver::HypreMat>(hypreMat_),
            *std::static_pointer_cast<HypreSolver::HypreSolverImpl>(hypreSolver_),
            A, hypreScale_))
        return;
    systemMatrix_ = A;
    fineRows_.resize(0, 0);
    fineRowsReady_ = false;
#else
    (void)A;
#endif
}

// --- Cached aggregation AMG ------------------------------------------------
//
// Classical AMG (BoomerAMG) re-derives its coarsening from the matrix values
// on every setup. On a SIMPLE outer iteration the sparsity pattern never
// changes, so this backend keeps the aggregation instead: the maps are built
// once per pattern, and later iterations only refresh the level operators by
// accumulating values through the cached scatter (O(nnz), no sort). This is
// the OpenFOAM GAMG structure and what makes the per-iteration setup cost
// competitive with   /  .

namespace {

using AggRowMatrix = Eigen::SparseMatrix<double, Eigen::RowMajor>;

// P^T A P with piecewise-constant P: every fine coefficient is added to the
// (aggregate(row), aggregate(col)) entry.
AggRowMatrix coarsenByAggregation(const AggRowMatrix& A,
                                  const std::vector<int>& agg, int nCoarse) {
    // P^T A P with piecewise-constant P, then a per-row sparsification: keep
    // the diagonal and the largest entries, drop the rest. The unsmoothed
    // Galerkin product over a group of `g` unknowns produces a stencil up to
    // g^2 times wider than the fine one (a 5-point fine stencil becomes 9-
    // point at level 1, 25-point at level 2, ...), which is what makes the
    // coarse levels dense and the V-cycle expensive.   and OpenFOAM
    // both truncate the coarse operator to a compact stencil; this keeps the
    // per-level nnz ~constant instead of growing.
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
    // off-diagonal entries per row. maxPerRow = 8 keeps a 9-point stencil
    // (the 2D 5-point + diagonals); it does not truncate at these levels,
    constexpr int maxPerRow = 8;
    std::vector<Eigen::Triplet<double>> kept;
    kept.reserve(static_cast<std::size_t>(out.nonZeros()));
    for (int row = 0; row < out.outerSize(); ++row) {
        double diag = 0.0;
        // (magnitude, signed value, column) - sort by magnitude, keep value.
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
    AggRowMatrix result = out;
    result.makeCompressed();
    return result;
}

// Greedy maximal-matching aggregation: every unassigned unknown opens a group
// and absorbs its strongest unassigned neighbours (relative to the row's
    // largest off-diagonal) up to groupSize members. O(nnz).
int aggregateGreedy(const AggRowMatrix& A, int groupSize, double strengthThreshold,
                    std::vector<int>& agg) {
    const int n = static_cast<int>(A.rows());
    agg.assign(static_cast<std::size_t>(n), -1);
    if (groupSize <= 1 || n <= 1) {
        for (int i = 0; i < n; ++i) agg[static_cast<std::size_t>(i)] = i;
        return n;
    }
    int nGroups = 0;
    for (int i = 0; i < n; ++i) {
        if (agg[static_cast<std::size_t>(i)] >= 0) continue;
        const int group = nGroups++;
        agg[static_cast<std::size_t>(i)] = group;
        int members = 1;
        if (members < groupSize) {
            double maxOff = 0.0;
            for (AggRowMatrix::InnerIterator it(A, i); it; ++it)
                if (it.col() != i) maxOff = std::max(maxOff, std::abs(it.value()));
            const double limit = strengthThreshold * maxOff;
            if (limit > 0.0) {
                for (AggRowMatrix::InnerIterator it(A, i); it; ++it) {
                    if (members >= groupSize) break;
                    const int j = it.col();
                    if (j == i || agg[static_cast<std::size_t>(j)] >= 0) continue;
                    if (std::abs(it.value()) < limit) continue;
                    agg[static_cast<std::size_t>(j)] = group;
                    ++members;
                }
            }
        }
    }
    return nGroups;
}

// --- Smoothed Aggregation prolongation helpers --------------------------

// Build the tentative prolongation T (piecewise constant): T(i, agg[i]) = 1.
// Dimensions: n_fine x n_coarse. Each fine node belongs to exactly one
// aggregate, so T has exactly one nonzero per row.
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
// where D = diag(A). Each pass spreads each coarse basis function to the
// neighbours of its aggregate, letting the coarse grid represent smooth
// error modes that piecewise-constant injection cannot. The near-nullspace
// constraint P*B = T*B is preserved (constant mode interpolated exactly).
SparseMatrix smoothProlongationJacobi(const SparseMatrix& A, const SparseMatrix& T,
                                       double omega, int degree) {
    int n = static_cast<int>(A.rows());
    // Extract diagonal of A.
    std::vector<double> D(static_cast<std::size_t>(n), 1.0);
    for (int i = 0; i < n; ++i) {
        double diag = A.coeff(i, i);
        if (std::abs(diag) > 1e-14)
            D[static_cast<std::size_t>(i)] = diag;
    }
    SparseMatrix P = T;
    for (int d = 0; d < degree; ++d) {
        SparseMatrix AP = A * P;  // n_fine x n_coarse (ColMajor)
        // Scale ROWS of AP by omega / D[row].  AP is ColMajor, so iterate over
        // columns and scale each entry by its row's diagonal weight.
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

// Galerkin coarse operator with smoothed prolongation:
//   A_c = P^T * A * P
SparseMatrix galerkinProductPAP(const SparseMatrix& P, const SparseMatrix& A) {
    SparseMatrix AP = A * P;            // n_fine x n_coarse
    SparseMatrix Ac = P.transpose() * AP;  // n_coarse x n_coarse
    Ac.makeCompressed();
    Ac.prune(0.0);
    return Ac;
}

} // namespace

bool AMGHierarchy::hasCachedAggregation() const {
    return useAggregation() && !aggMaps_.empty();
}

int AMGHierarchy::aggregate_(const AggRowMatrix& A, int groupSize,
                             std::vector<int>& agg) const {
    return aggregateGreedy(A, groupSize, config_.strengthThreshold, agg);
}

AggRowMatrix AMGHierarchy::coarsenByAggregation_(const AggRowMatrix& A,
                                                 const std::vector<int>& agg,
                                                 int nCoarse) const {
    return coarsenByAggregation(A, agg, nCoarse);
}

void AMGHierarchy::buildAggregation_(const SparseMatrix& A) {
    const std::size_t hash = detail::sparsityHash(A);
    const bool mapsValid = !aggMaps_.empty() && hash == aggPatternHash_;
    const auto buildStart = std::chrono::steady_clock::now();

    if (!mapsValid) {
        // First build for this pattern: derive the aggregate maps while the
        // operators from the previous level are available.
        aggMaps_.clear();
        aggSizes_.clear();
        aggCaches_.clear();
        // The level operators were built from the previous maps, so they are
        // stale now; force a full operator rebuild in refreshAggregationOperators_().
        levels_.clear();
        levelSizes_.clear();
        aggPatternHash_ = hash;

        AggRowMatrix level = A;
        level.makeCompressed();
        const int groupSize = std::max(2, config_.effectiveBlockSize(blockDim_));
        // Stop coarsening at a few hundred unknowns: each level below that
        // costs an operator rebuild per iteration but removes almost no error.
        const int coarseTarget = 128;
        for (int depth = 0; depth < config_.maxLevels; ++depth) {
            const int n = static_cast<int>(level.rows());
            std::vector<int> agg;
            const int nCoarse = aggregate_(level, groupSize, agg);
            if (nCoarse >= n || nCoarse <= 0) break;
            aggMaps_.push_back(std::move(agg));
            aggSizes_.push_back(nCoarse);
            level = coarsenByAggregation_(level, aggMaps_.back(), nCoarse);
            if (nCoarse <= coarseTarget) break;
        }
    }

    // Build (or refresh) the level operators, then the scatter caches when the
    // maps were just created. Scatter caches are only used by the plain
    // aggregation fast path; Smoothed Aggregation always rebuilds P and coarse
    // operators from current values, so skip cache build for SA.
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
    useHypreActive_ = false;
    hypreHierarchyReady_ = false;
    buildTimeSec_ = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - buildStart).count();
}

void AMGHierarchy::buildAggScatterCaches_() {
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

void AMGHierarchy::refreshAggregationOperators_(const SparseMatrix& A) {
    // Fast path: the level patterns already exist (built once from the cached
    // maps). Refresh only the values: level 0 from the fine matrix, then each
    // coarse level by accumulating the level above through the cached scatter.
    const bool havePattern =
        !config_.smoothedProlongation &&   // SA: P depends on A values, always rebuild
        levels_.size() == aggMaps_.size() + 1 && !levels_.empty() &&
        levels_[0].ARM.rows() == A.rows() &&
        aggCaches_.size() == aggMaps_.size();
    if (havePattern) {
        levels_[0].ARM = A;
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
        return;
    }
    // Slow path: first build or pattern change. Build the level patterns with
    // the sort-based path; the scatter caches are rebuilt by the caller.
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
            // Smoothed Aggregation: build tentative prolongation T from the
            // aggregation map, smooth it with Jacobi, then form the Galerkin
            // coarse operator A_c = P^T * A * P. This replaces plain
            // piecewise-constant injection, giving the coarse grid the ability
            // to represent smooth error modes.
            SparseMatrix T = buildTentativeProlongation(level, aggMaps_[l], aggSizes_[l]);
            SparseMatrix P = smoothProlongationJacobi(level, T,
                                                      config_.saOmega, config_.saDegree);
            SparseMatrix R = P.transpose();
            R.makeCompressed();
            levels_[l].P = std::move(P);
            levels_[l].R = std::move(R);
            level = galerkinProductPAP(levels_[l].P, level);
        } else {
            level = coarsenByAggregation_(level, aggMaps_[l], aggSizes_[l]);
        }
    }
}

// One V-cycle over the cached aggregation hierarchy.
bool AMGHierarchy::aggregationVcycle_(int level, const Eigen::VectorXd& b,
                                      Eigen::VectorXd& x) const {
    if (level < 0 || static_cast<std::size_t>(level) >= levels_.size())
        return false;
    const auto& A = levels_[static_cast<std::size_t>(level)].ARM;
    const int n = static_cast<int>(A.rows());
    if (n == 0 || b.size() != n || x.size() != n) return false;

    // STAR's pressure preconditioner is a fixed V-cycle. The exact
    // per-level sweep counts in its log (level 0: 1/3, levels >= 1: 5/5)
    // make each coarse level expensive with our Galerkin operators, so the
    // measured optimum here is 2/2 everywhere; the strength comes from the
    // number of cycles per application (setCycles).
    const int pre = std::max(2, config_.preSweeps);
    const int post = std::max(2, config_.postSweeps);

    for (int s = 0; s < pre; ++s) gaussSeidel(A, b, x, 1, true);

    const bool hasCoarse = static_cast<std::size_t>(level) < aggMaps_.size();
    if (hasCoarse) {
        const auto& P = levels_[static_cast<std::size_t>(level)].P;
        const auto& R = levels_[static_cast<std::size_t>(level)].R;
        const int nCoarse = aggSizes_[static_cast<std::size_t>(level)];
        Eigen::VectorXd residual = b - A * x;
        Eigen::VectorXd coarseRhs;
        if (P.cols() > 0 && R.rows() > 0 && P.cols() == nCoarse) {
            // Smoothed Aggregation: restrict via P^T (prolongation transpose)
            coarseRhs = R * residual;
        } else {
            // Plain aggregation: sum residual into aggregates
            const auto& agg = aggMaps_[static_cast<std::size_t>(level)];
            coarseRhs = Eigen::VectorXd::Zero(nCoarse);
            for (int i = 0; i < n; ++i)
                coarseRhs[agg[static_cast<std::size_t>(i)]] += residual[i];
        }
        Eigen::VectorXd coarseCorr = Eigen::VectorXd::Zero(nCoarse);
        if (!aggregationVcycle_(level + 1, coarseRhs, coarseCorr)) return false;
        if (P.cols() > 0 && P.cols() == nCoarse) {
            // Smoothed Aggregation: prolongate via P
            x += P * coarseCorr;
        } else {
            // Plain aggregation: gather correction from aggregate
            const auto& agg = aggMaps_[static_cast<std::size_t>(level)];
            for (int i = 0; i < n; ++i)
                x[i] += coarseCorr[agg[static_cast<std::size_t>(i)]];
        }
    } else {
        // Coarsest level: dense direct solve (small by construction).
        Eigen::MatrixXd dense(n, n);
        for (int row = 0; row < n; ++row) {
            dense.row(row).setZero();
            for (AggRowMatrix::InnerIterator it(A, row); it; ++it)
                dense(row, it.col()) = it.value();
        }
        const Eigen::LDLT<Eigen::MatrixXd> factor(dense);
        if (factor.info() != Eigen::Success) return false;
        x = factor.solve(b);
        if (!x.allFinite()) return false;
        return true;
    }

    for (int s = 0; s < post; ++s) gaussSeidel(A, b, x, 1, true);
    return x.allFinite();
}

// Standalone solve. When Acceleration = None and CycleType = Flexible
// (momentum/turbulence default) this delegates to the flexible fine-only
// path: residual-gated Gauss-Seidel, no hierarchy, no V-cycle, no
// acceleration wrapper. Otherwise the V-cycle is iterated as a
// preconditioned Richardson (fixed-point) iteration to tolerance. The
// SIMPLE outer loop owns the final accuracy.
bool AMGHierarchy::solveAggregation_(const Eigen::VectorXd& b, Eigen::VectorXd& x,
                                     const std::atomic<bool>* cancel) const {
    if (!built_ || levels_.empty()) return false;
    if (static_cast<int>(x.size()) != n_) { x.resize(n_); x.setZero(); }
    if (!b.allFinite()) return false;
    const double bnorm = b.norm();
    if (!std::isfinite(bnorm)) return false;
    if (bnorm < 1e-30) { x.setZero(); return true; }

    const int maxIt = std::max(1, config_.maxIterations);
    const double tol = std::max(1e-16, config_.tolerance);

    lastResidualHistory_.clear();
    lastResidualHistory_.push_back((b - systemMatrix_ * x).norm() / bnorm);
    lastIterations_ = 0;
    lastResidual_ = (b - systemMatrix_ * x).norm() / bnorm;

    // STAR Flex-Cycle fine-only path (momentum/turbulence default):
    // Acceleration = None means no Krylov wrapper; Flexible cycle means
    // residual-gated fine-level Gauss-Seidel only — no hierarchy, no V-cycle,
    // no acceleration. This matches   exactly: one cycle = one fine
    // GS sweep, repeated while the residual falls by flexAlpha each sweep.
    if (config_.cycleType == AmgSettings::CycleType::Flexible &&
        config_.acceleration == AmgSettings::Acceleration::None) {
        return solveFlexibleFine_(b, x, cancel);
    }

    // Fixed-point (Richardson) iteration: apply one V-cycle to the current
    // residual and add the correction, repeated to tolerance. Unlike the
    // Conjugate-Gradient algebra used previously this makes no symmetry or
    // positive-definiteness assumption, so it is valid for nonsymmetric
    // systems (e.g. pressure on the legacy nonsymmetric operator). The
    // V-cycle uses Gauss-Seidel smoothing only — no acceleration wrapper.
    Eigen::VectorXd r = b - systemMatrix_ * x;
    Eigen::VectorXd z(n_);
    for (int iter = 0; iter < maxIt; ++iter) {
        if (cancel && cancel->load()) return false;
        z.setZero();
        if (!aggregationVcycle_(0, r, z)) return false;
        if (!z.allFinite()) return false;
        x += z;
        r = b - systemMatrix_ * x;
        if (!r.allFinite()) return false;
        const double rel = r.norm() / bnorm;
        lastResidualHistory_.push_back(rel);
        lastIterations_ = iter + 1;
        lastResidual_ = rel;
        if (rel < tol) return true;
        // Stagnation: the residual stopped falling. Accept a loose pass so the
        // SIMPLE outer loop can still make progress instead of triggering an
        // ILU fallback.
        if (iter >= config_.stagnationMinIter &&
            rel > lastResidualHistory_[static_cast<std::size_t>(iter)] *
                      config_.stagnationFactor) {
            return rel < tol * config_.stagnationLooseFactor;
        }
    }
    return lastResidual_ < tol * config_.stagnationLooseFactor;
}

void AMGHierarchy::strengthOfConnection(const Eigen::SparseMatrix<double, Eigen::RowMajor>& A,
                                         std::vector<std::vector<int>>& strongNeighbors) const {
    strongNeighbors.assign(static_cast<std::size_t>(A.rows()), {});
    const double thresh = config_.strengthThreshold;
    for (Eigen::Index i = 0; i < A.outerSize(); ++i) {
        double maxOff = 0.0;
        for (Eigen::SparseMatrix<double, Eigen::RowMajor>::InnerIterator it(A, i); it; ++it) {
            if (it.col() != i) maxOff = std::max(maxOff, std::abs(it.value()));
        }
        if (maxOff == 0.0) continue;
        const double limit = thresh * maxOff;
        for (Eigen::SparseMatrix<double, Eigen::RowMajor>::InnerIterator it(A, i); it; ++it) {
            if (it.col() != i && std::abs(it.value()) >= limit) {
                strongNeighbors[static_cast<std::size_t>(i)].push_back(static_cast<int>(it.col()));
            }
        }
    }
}

void AMGHierarchy::rsCoarsening(const std::vector<std::vector<int>>&, std::vector<int>&, int&) const {
    throw std::runtime_error("AMGHierarchy::rsCoarsening not implemented");
}
void AMGHierarchy::pmisCoarsening(const std::vector<std::vector<int>>&, std::vector<int>&, int&) const {
    throw std::runtime_error("AMGHierarchy::pmisCoarsening not implemented");
}
void AMGHierarchy::directInterpolation(const SparseMatrix&, const std::vector<std::vector<int>>&, const std::vector<int>&, int, SparseMatrix&) const {
    throw std::runtime_error("AMGHierarchy::directInterpolation not implemented");
}
void AMGHierarchy::extendedInterpolation(const SparseMatrix&, const std::vector<std::vector<int>>&, const std::vector<int>&, int, SparseMatrix&) const {
    throw std::runtime_error("AMGHierarchy::extendedInterpolation not implemented");
}
void AMGHierarchy::truncateInterpolation(SparseMatrix&) const {
    throw std::runtime_error("AMGHierarchy::truncateInterpolation not implemented");
}

SparseMatrix AMGHierarchy::galerkinProduct(const SparseMatrix& A, const SparseMatrix& P) const {
    // Compute R * A * P where R = P^T. A is ColMajor, P is ColMajor (n_fine x n_coarse).
    Eigen::SparseMatrix<double> APC = A * P;
    SparseMatrix result = P.transpose() * APC;
    result.prune(0.0);
    return result;
}

void AMGHierarchy::vCycle(int, const Eigen::VectorXd&, Eigen::VectorXd&) const {
    throw std::runtime_error("AMGHierarchy::vCycle not implemented");
}
void AMGHierarchy::fCycle(int, const Eigen::VectorXd&, Eigen::VectorXd&) const {
    throw std::runtime_error("AMGHierarchy::fCycle not implemented");
}
void AMGHierarchy::cycle(int, const Eigen::VectorXd&, Eigen::VectorXd&) const {
    throw std::runtime_error("AMGHierarchy::cycle not implemented");
}

void AMGHierarchy::gaussSeidel(const Eigen::SparseMatrix<double, Eigen::RowMajor>& ARM,
                               const Eigen::VectorXd& b, Eigen::VectorXd& x,
                               int numSweeps, bool forward) const {
    const int n = static_cast<int>(ARM.rows());
    for (int s = 0; s < numSweeps; ++s) {
        if (forward) {
            for (int i = 0; i < n; ++i) {
                double diag = 0.0, sum = 0.0;
                for (Eigen::SparseMatrix<double, Eigen::RowMajor>::InnerIterator it(ARM, i); it; ++it) {
                    if (it.col() == i) diag = it.value();
                    else sum += it.value() * x[it.col()];
                }
                if (std::abs(diag) < 1e-14 || !std::isfinite(diag)) continue;
                if (!std::isfinite(b[i]) || !std::isfinite(sum)) continue;
                x[i] = (b[i] - sum) / diag;
            }
        } else {
            for (int i = n - 1; i >= 0; --i) {
                double diag = 0.0, sum = 0.0;
                for (Eigen::SparseMatrix<double, Eigen::RowMajor>::InnerIterator it(ARM, i); it; ++it) {
                    if (it.col() == i) diag = it.value();
                    else sum += it.value() * x[it.col()];
                }
                if (std::abs(diag) < 1e-14 || !std::isfinite(diag)) continue;
                if (!std::isfinite(b[i]) || !std::isfinite(sum)) continue;
                x[i] = (b[i] - sum) / diag;
            }
        }
    }
}

void AMGHierarchy::smoothPre_(const Eigen::SparseMatrix<double, Eigen::RowMajor>& ARM,
                              const Eigen::VectorXd& b, Eigen::VectorXd& x, int numSweeps) const {
    gaussSeidel(ARM, b, x, numSweeps, true);
}

void AMGHierarchy::smoothPost_(const Eigen::SparseMatrix<double, Eigen::RowMajor>& ARM,
                               const Eigen::VectorXd& b, Eigen::VectorXd& x, int numSweeps) const {
    // For nonsymmetric operators, post-sweep is also forward so the composed
    // preconditioner remains a fixed linear operator.
    gaussSeidel(ARM, b, x, numSweeps, true);
}


// --- STAR Flex-Cycle fine-only path (Acceleration = None) ------------------
//
//   default momentum AMG: "Flex Cycle", Restriction Tolerance = 0.9,
// Prolongation Tolerance = 0.5, Sweeps = 1, Acceleration = None. When the fine
// level alone keeps the residual falling by the restriction tolerance each
// sweep, STAR never builds a coarse level (its log shows a single "level 0"
// row and Restriction/Prolongation time = 0). This is the residual-gated
// fine iteration: O(nnz) per cycle, no hierarchy, no setup.

bool AMGHierarchy::applyFlexibleFineCycle_(const Eigen::VectorXd& b,
                                          Eigen::VectorXd& x) const {
    const int n = n_;
    if ((int)x.size() != n) { x.resize(n); x.setZero(); }
    if (!b.allFinite()) return false;
    const double bnorm = b.norm();
    if (!std::isfinite(bnorm) || bnorm < 1e-30) return true;

    // One fine sweep; the restriction tolerance (flexAlpha) decides whether
    // the level is resolved. flexMaxSweeps caps the sweeps per cycle (STAR
    // Sweeps = 1 by default).
    const int sweeps = std::max(1, config_.flexMaxSweeps);
    const double r0 = (b - systemMatrix_ * x).norm();
    if (!std::isfinite(r0) || r0 == 0.0) return true;
    const double gate = config_.flexAlpha * r0;
    for (int s = 0; s < sweeps; ++s) {
        fineGaussSeidel_(b, x, 1);
        const double res = (b - systemMatrix_ * x).norm();
        if (!std::isfinite(res)) { x.setZero(); return false; }
        if (res < gate) break; // restriction tolerance met: stop refining
    }
    return x.allFinite();
}

bool AMGHierarchy::solveFlexibleFine_(const Eigen::VectorXd& b, Eigen::VectorXd& x,
                                      const std::atomic<bool>* cancel) const {
    if (static_cast<int>(x.size()) != n_) { x.resize(n_); x.setZero(); }
    if (!b.allFinite()) return false;
    const double bnorm = b.norm();
    if (!std::isfinite(bnorm)) return false;
    if (bnorm < 1e-30) { x.setZero(); return true; }

    const int maxIt = std::max(1, config_.maxIterations);
    const double tol = std::max(1e-16, config_.tolerance);
    const double r0 = (b - systemMatrix_ * x).norm();
    double prev = std::numeric_limits<double>::max();
    lastResidualHistory_.clear();
    // Cycle-0 baseline, mirroring the fixed-cycle path.
    lastResidualHistory_.push_back(r0 / bnorm);
    lastIterations_ = 0;
    lastResidual_ = r0 / bnorm;
    for (int iter = 0; iter < maxIt; ++iter) {
        if (cancel && cancel->load()) return false;
        if (!applyFlexibleFineCycle_(b, x)) return false;
        const double rel = (b - systemMatrix_ * x).norm() / bnorm;
        lastResidualHistory_.push_back(rel);
        lastIterations_ = iter + 1;
        lastResidual_ = rel;
        if (rel < tol) return true;
        if (rel > prev * config_.stagnationFactor && iter >= config_.stagnationMinIter)
            return rel < tol * config_.stagnationLooseFactor;
        prev = rel;
    }
    lastResidual_ = (b - systemMatrix_ * x).norm() / bnorm;
    return lastResidual_ < tol * config_.stagnationLooseFactor;
}

} // namespace fvm