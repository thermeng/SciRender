#include "fvm/AMG.h"
#include "fvm/AMG_F.h"
#include "fvm/HypreSolver.h"

#include <amgcl/amg.hpp>
#include <amgcl/coarsening/ruge_stuben.hpp>
#include <amgcl/coarsening/smoothed_aggregation.hpp>
#include <amgcl/relaxation/ilu0.hpp>
#include <amgcl/relaxation/spai0.hpp>
#include <amgcl/relaxation/gauss_seidel.hpp>
#include <amgcl/relaxation/damped_jacobi.hpp>
#include <amgcl/backend/builtin.hpp>
#include <amgcl/adapter/eigen.hpp>

#include <memory>
#include <cmath>
#include <limits>
#include <sstream>
#include <iomanip>
#include <vector>

AMGCL_USE_EIGEN_VECTORS_WITH_BUILTIN_BACKEND()

namespace fvm {

namespace {
#ifdef FVM_HAVE_HYPRE
// Shared Hypre holder: keeps the BoomerAMG solver and its ParCSR matrix
// alive for the lifetime of the hierarchy.
struct HypreHolder {
    std::shared_ptr<HypreSolver::HypreMat> mat;
    std::shared_ptr<HypreSolver::HypreSolverImpl> solver;
};
#endif
using Backend = amgcl::backend::builtin<double>;

// The coarsening/smoother pairs the configuration exposes.  uses
// aggregation coarsening with a Gauss-Seidel smoother for both the pressure and
// the momentum equation.
using AggGsAmg     = amgcl::amg<Backend, amgcl::coarsening::smoothed_aggregation,
                                amgcl::relaxation::gauss_seidel>;
using AggJacobiAmg = amgcl::amg<Backend, amgcl::coarsening::smoothed_aggregation,
                                amgcl::relaxation::damped_jacobi>;
using AggIluAmg    = amgcl::amg<Backend, amgcl::coarsening::smoothed_aggregation,
                                amgcl::relaxation::ilu0>;
using AggSpaiAmg   = amgcl::amg<Backend, amgcl::coarsening::smoothed_aggregation,
                                amgcl::relaxation::spai0>;
using RsGsAmg      = amgcl::amg<Backend, amgcl::coarsening::ruge_stuben,
                                amgcl::relaxation::gauss_seidel>;
using RsJacobiAmg  = amgcl::amg<Backend, amgcl::coarsening::ruge_stuben,
                                amgcl::relaxation::damped_jacobi>;
using RsIluAmg     = amgcl::amg<Backend, amgcl::coarsening::ruge_stuben,
                                amgcl::relaxation::ilu0>;
using RsSpaiAmg    = amgcl::amg<Backend, amgcl::coarsening::ruge_stuben,
                                amgcl::relaxation::spai0>;

enum class Kind { AggGs, AggJacobi, AggIlu, AggSpai, RsGs, RsJacobi, RsIlu, RsSpai };

struct Handle {
    Kind kind = Kind::AggGs;
    std::shared_ptr<void> ptr;
};

// Common amgcl parameters. `npre`/`npost` are global in amgcl (
// varies them per level in its flex cycle); the per-equation presets pick the
// values that matter at the fine level.
template <class AMG, class CoarseningParams, class RelaxParams>
std::shared_ptr<AMG> makeAmg(const Eigen::SparseMatrix<double, Eigen::RowMajor>& A,
                             const AMGHierarchy::Config& c,
                             CoarseningParams cp, RelaxParams rp) {
    typename AMG::params ap;
    ap.coarsening = cp;
    ap.relax = rp;
    ap.coarse_enough = static_cast<unsigned>(c.coarseEnough > 0 ? c.coarseEnough : 2);
    ap.max_levels = static_cast<unsigned>(c.maxLevels > 0 ? c.maxLevels : 30);
    ap.npre = static_cast<unsigned>(c.preSweeps >= 0 ? c.preSweeps : 1);
    ap.npost = static_cast<unsigned>(c.postSweeps >= 0 ? c.postSweeps : 1);
    // V = one inner loop, W = two. F is implemented in applyCycle
    // as two V-cycles (F = V then V on residual) — no vendor change needed.
    ap.ncycle = (c.cycleType == AMGHierarchy::Config::CycleType::WCycle) ? 2u : 1u;
    ap.pre_cycles = static_cast<unsigned>(c.preCycles > 0 ? c.preCycles : 1);
    ap.direct_coarse = c.directCoarse;
    ap.allow_rebuild = true;
    return std::make_shared<AMG>(A, ap);
}

// amgcl does not expose the level count, but its operator<< prints the whole
// hierarchy. Parse the sizes out once at build time so the solver can log the
// same "Level / Size" table  does.
void parseHierarchy(const std::string& report, int& nlevels, std::vector<int>& sizes) {
    nlevels = 0;
    sizes.clear();
    std::istringstream in(report);
    std::string line;
    bool table = false;
    while (std::getline(in, line)) {
        if (!table) {
            if (line.find("level") != std::string::npos &&
                line.find("unknowns") != std::string::npos) {
                table = true;
            }
            continue;
        }
        if (!line.empty() && line[0] == '-') continue;
        std::istringstream row(line);
        int level = -1;
        long long unknowns = 0;
        if (row >> level >> unknowns) {
            ++nlevels;
            sizes.push_back(static_cast<int>(unknowns));
        }
    }
    if (nlevels == 0) {
        // No table (amgcl failed to build): report the fine level only.
        nlevels = sizes.empty() ? 0 : 1;
    }
}
} // namespace

void AMGHierarchy::build(const SparseMatrix& A) {
    levels_.clear();
    built_ = false;
    amgHandle_.reset();
    famgHandle_.reset();
    hypreHandle_.reset();
    hypreScale_.clear();
    useHypreActive_ = false;
    n_ = 0;
    nlevels_ = 0;
    levelSizes_.clear();
    report_.clear();
    systemMatrix_ = A;
    if (A.rows() == 0 || A.cols() == 0) { buildTimeSec_ = 0.0; return; }

    n_ = static_cast<int>(A.rows());

    auto buildStart = std::chrono::steady_clock::now();

#ifdef FVM_HAVE_HYPRE
    // Hypre BoomerAMG backend (opt-in via Config::useHypre). On any Hypre
    // failure we fall through to the amgcl path below, never failing build.
    if (config_.useHypre) {
        try {
            auto impl = std::make_shared<HypreSolver::HypreSolverImpl>();
            auto mat = std::make_shared<HypreSolver::HypreMat>();
            std::vector<double> hscale;
            if (HypreSolver::buildSolver(A, config_, *mat, *impl, hscale)) {
                auto holder = std::make_shared<HypreHolder>();
                holder->mat = mat;
                holder->solver = impl;
                hypreHandle_ = holder;
                hypreScale_ = hscale;
                useHypreActive_ = true;
                nlevels_ = 1;
                levelSizes_ = {n_};
                report_ = "Hypre BoomerAMG hierarchy (" +
                          std::to_string(n_) + " unknowns, 1 level shown; " +
                          (config_.cycleType == Config::CycleType::WCycle ? "W"
                           : config_.cycleType == Config::CycleType::FCycle ? "F"
                           : config_.cycleType == Config::CycleType::Flexible ? "Flexible"
                                                                                  : "V") +
                          "-cycle, strength 0.25, maxLevels " +
                          std::to_string(config_.maxLevels > 0 ? config_.maxLevels : 50) +
                          ", coarse direct <= " +
                          std::to_string(config_.coarseEnough > 0 ? config_.coarseEnough : 2) +
                          ")\n";
                buildTimeSec_ = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - buildStart).count();
                built_ = true;
                return;
            }
        } catch (...) {
            hypreHandle_.reset();
            hypreScale_.clear();
            useHypreActive_ = false;
        }
    }
#else
    // Hypre not linked in: useHypre=true falls back to amgcl.
    (void)0;
#endif

    Eigen::SparseMatrix<double, Eigen::RowMajor> AR = A;
    AR.makeCompressed();

    // Bandwidth reorder TODO: needs matching b/x permutation at applyCycle.
    // Disabled for now to avoid scale mismatch.

    // Auto scaling: symmetric diagonal equilibration.
    scale_.clear();
    if (config_.scaling == AMGHierarchy::Config::ProportionalScaling::Automatic) {
        const int n = static_cast<int>(A.rows());
        scale_.assign(n, 1.0);
        for (int i = 0; i < n; ++i) {
            double diag = A.coeff(i, i);
            if (std::abs(diag) > 0.0) scale_[i] = 1.0 / std::sqrt(std::abs(diag));
        }
        // Apply D A D to AR.
        const int* outer = AR.outerIndexPtr();
        const int* inner = AR.innerIndexPtr();
        double* vals = AR.valuePtr();
        for (int col = 0; col < n; ++col) {
            for (int k = outer[col]; k < outer[col + 1]; ++k) {
                int row = inner[k];
                vals[k] *= scale_[row] * scale_[col];
            }
        }
    } else {
        scale_.clear();
    }

    auto h = std::make_shared<Handle>();

    try {
        using Coarsening = Config::Coarsening;
        using Smoother = Config::Smoother;

        // `nonsymmetric` is the legacy UI hint. Aggregation coarsening is the
        // robust choice for a nonsymmetric operator, so it overrides the
        // coarsening choice while leaving the smoother as configured.
        Coarsening coarsening = config_.coarsening;
        if (config_.nonsymmetric) coarsening = Coarsening::SmoothedAggregation;
        const bool agg = (coarsening == Coarsening::SmoothedAggregation);

        Kind kind;
        switch (config_.smoother) {
            case Smoother::Jacobi:      kind = agg ? Kind::AggJacobi : Kind::RsJacobi; break;
            case Smoother::GaussSeidel: kind = agg ? Kind::AggGs     : Kind::RsGs;     break;
            case Smoother::ILU0:        kind = agg ? Kind::AggIlu    : Kind::RsIlu;    break;
            case Smoother::SPAI0:
            default:                    kind = agg ? Kind::AggSpai   : Kind::RsSpai;   break;
        }
        h->kind = kind;

        auto aggParams = [&]() {
            amgcl::coarsening::smoothed_aggregation<Backend>::params cp;
            // amgcl marks a coefficient strong when
            //     a_ij^2 > eps^2 * a_ii * a_jj,
            // i.e. eps is measured against sqrt(a_ii a_jj).  measures
            // it against the diagonal and defaults to 0.25. For a uniform
            // 5-point stencil the two scales coincide exactly at 0.25, which
            // sits on amgcl's strict ">" boundary: no connection is strong, no
            // aggregate is formed, and the hierarchy collapses to a single
            // level (the momentum operator degenerates that way on a uniform
            // mesh). Halving keeps the  value usable with margin.
            cp.aggr.eps_strong =
                static_cast<float>(0.5 * config_.strengthThreshold);
            // Group size: amgcl requires n % block == 0. Auto=2 needs padding -> stay 1 for scalar.
            if (config_.groupSizeControl == Config::GroupSizeControl::Manual &&
                config_.blockSize > 1 && (n_ % config_.blockSize) == 0) {
                cp.aggr.block_size = static_cast<unsigned>(config_.blockSize);
            }
            return cp;
        };
        auto rsParams = [&]() {
            amgcl::coarsening::ruge_stuben<Backend>::params cp;
            cp.eps_strong = static_cast<float>(config_.strengthThreshold);
            cp.do_trunc = (config_.truncThreshold > 0.0 && config_.truncThreshold < 1.0);
            cp.eps_trunc = static_cast<float>(config_.truncThreshold);
            return cp;
        };

        if (agg) {
            auto cp = aggParams();
            switch (config_.smoother) {
                case Smoother::Jacobi: {
                    amgcl::relaxation::damped_jacobi<Backend>::params rp;
                    h->ptr = makeAmg<AggJacobiAmg>(AR, config_, cp, rp);
                    break;
                }
                case Smoother::GaussSeidel: {
                    amgcl::relaxation::gauss_seidel<Backend>::params rp;
                    // amgcl defaults `serial` to false and then switches to its
                    // level-scheduled parallel_sweep whenever OpenMP reports >= 4
                    // threads. That sweep inserts an OpenMP barrier per
                    // dependency level (hundreds per V-cycle on a structured
                    // grid), which on MinGW/Windows costs ~50 ms per cycle at 4
                    // threads and ~100 ms at 8 - roughly 300x the serial sweep.
                    // Gauss-Seidel is inherently sequential here, so keep the
                    // fast, deterministic serial sweep on every level.
                    rp.serial = true;
                    h->ptr = makeAmg<AggGsAmg>(AR, config_, cp, rp);
                    break;
                }
                case Smoother::ILU0: {
                    amgcl::relaxation::ilu0<Backend>::params rp;
                    rp.damping = 1.0;
                    h->ptr = makeAmg<AggIluAmg>(AR, config_, cp, rp);
                    break;
                }
                case Smoother::SPAI0:
                default: {
                    amgcl::relaxation::spai0<Backend>::params rp;
                    h->ptr = makeAmg<AggSpaiAmg>(AR, config_, cp, rp);
                    break;
                }
            }
        } else {
            auto cp = rsParams();
            switch (config_.smoother) {
                case Smoother::Jacobi: {
                    amgcl::relaxation::damped_jacobi<Backend>::params rp;
                    h->ptr = makeAmg<RsJacobiAmg>(AR, config_, cp, rp);
                    break;
                }
                case Smoother::GaussSeidel: {
                    amgcl::relaxation::gauss_seidel<Backend>::params rp;
                    // See the aggregation branch: force the serial sweep so the
                    // OpenMP level-scheduled sweep (>=4 threads) is never used.
                    rp.serial = true;
                    h->ptr = makeAmg<RsGsAmg>(AR, config_, cp, rp);
                    break;
                }
                case Smoother::ILU0: {
                    amgcl::relaxation::ilu0<Backend>::params rp;
                    rp.damping = 1.0;
                    h->ptr = makeAmg<RsIluAmg>(AR, config_, cp, rp);
                    break;
                }
                case Smoother::SPAI0:
                default: {
                    amgcl::relaxation::spai0<Backend>::params rp;
                    h->ptr = makeAmg<RsSpaiAmg>(AR, config_, cp, rp);
                    break;
                }
            }
        }

        if (h->ptr) {
            std::ostringstream os;
            switch (kind) {
                case Kind::AggGs:     os << *std::static_pointer_cast<AggGsAmg>(h->ptr); break;
                case Kind::AggJacobi: os << *std::static_pointer_cast<AggJacobiAmg>(h->ptr); break;
                case Kind::AggIlu:    os << *std::static_pointer_cast<AggIluAmg>(h->ptr); break;
                case Kind::AggSpai:   os << *std::static_pointer_cast<AggSpaiAmg>(h->ptr); break;
                case Kind::RsGs:      os << *std::static_pointer_cast<RsGsAmg>(h->ptr); break;
                case Kind::RsJacobi:  os << *std::static_pointer_cast<RsJacobiAmg>(h->ptr); break;
                case Kind::RsIlu:     os << *std::static_pointer_cast<RsIluAmg>(h->ptr); break;
                case Kind::RsSpai:    os << *std::static_pointer_cast<RsSpaiAmg>(h->ptr); break;
            }
            report_ = os.str();
            parseHierarchy(report_, nlevels_, levelSizes_);
            //  "High" verbosity prints the multigrid series and the
            // equation count of each level after the hierarchy is built.
            if (static_cast<int>(config_.verbosity) >=
                static_cast<int>(Config::Verbosity::High)) {
                std::cerr << "  AMG hierarchy (" << n_ << " unknowns, "
                          << nlevels_ << " levels)\n" << report_;
            }
            // "Diagnosis" only for V/W — F/Flexible use own hierarchy (AMG_F.h),
            // amgcl levels are dummy there and would mislead.
            if (config_.verbosity == Config::Verbosity::Diagnosis && config_.cycleType != Config::CycleType::FCycle && config_.cycleType != Config::CycleType::Flexible) {
                std::cerr << "  AMG diagnosis: " << n_ << " unknowns, "
                          << nlevels_ << " levels, level sizes";
                for (int s : levelSizes_) std::cerr << ' ' << s;
                std::cerr << "\n    coarsening="
                          << (agg ? "smoothed aggregation" : "Ruge-Stuben")
                          << ", smoother="
                          << (config_.smoother == Config::Smoother::Jacobi ? "Jacobi"
                              : config_.smoother == Config::Smoother::ILU0 ? "ILU"
                              : config_.smoother == Config::Smoother::SPAI0 ? "SPAI0"
                                                                           : "Gauss-Seidel")
                          << ", cycle="
                           << (config_.cycleType == Config::CycleType::WCycle ? "W"
                               : config_.cycleType == Config::CycleType::FCycle ? "F"
                               : config_.cycleType == Config::CycleType::Flexible ? "Flexible"
                                                                                 : "V")
                          << ", coarse solve="
                          << (config_.directCoarse ? "direct" : "iterative")
                          << " (<= " << config_.coarseEnough << " equations)\n";
            }
        }
    } catch (...) {
        h->ptr.reset();
    }
    buildTimeSec_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - buildStart).count();
    amgHandle_ = h;
    built_ = (h->ptr != nullptr && n_ > 0);
}

bool AMGHierarchy::applyCycle(const Eigen::VectorXd& b, Eigen::VectorXd& x) const {
#ifdef FVM_HAVE_HYPRE
    if (useHypreActive_ && hypreHandle_) {
        auto holder = std::static_pointer_cast<HypreHolder>(hypreHandle_);
        if (holder && holder->mat && holder->solver) {
            return HypreSolver::applyCycle(*holder->mat, *holder->solver, b, x, hypreScale_);
        }
        return false;
    }
#endif
    if (!built_ || !amgHandle_) { x = b; return false; }
    auto h = std::static_pointer_cast<Handle>(amgHandle_);
    if (!h || !h->ptr) { x = b; return false; }

    const bool hasScale = (static_cast<int>(scale_.size()) == b.size());
    if (bn_work_.size() != static_cast<std::size_t>(b.size()) ||
        xn_work_.size() != static_cast<std::size_t>(x.size())) {
        bn_work_.resize(b.size());
        xn_work_.resize(x.size());
    }
    amgcl::backend::numa_vector<double>& bn = bn_work_;
    amgcl::backend::numa_vector<double>& xn = xn_work_;
    for (int i = 0; i < b.size(); ++i) {
        bn[i] = hasScale ? b(i) * scale_[i] : b(i);
        xn[i] = hasScale ? x(i) / scale_[i] : x(i);
    }

    // F-cycle via own hierarchy in src/fvm/AMG_F.h (vendor untouched).
    // Per-instance (not thread_local) → no cross-solver clobbering, rebuilt
    // in build()/updateFineValues() so values never go stale.
    if (config_.cycleType == Config::CycleType::FCycle || config_.cycleType == Config::CycleType::Flexible) {
        if (!famgHandle_) {
            auto famg = std::make_shared<FAMGHierarchy>(config_);
            if (!famg->build(systemMatrix_)) {
                // No UB cast: use kind switch like the V/W path.
                switch (h->kind) {
                    case Kind::AggGs:     std::static_pointer_cast<AggGsAmg>(h->ptr)->cycle(bn, xn); break;
                    case Kind::AggJacobi: std::static_pointer_cast<AggJacobiAmg>(h->ptr)->cycle(bn, xn); break;
                    case Kind::AggIlu:    std::static_pointer_cast<AggIluAmg>(h->ptr)->cycle(bn, xn); break;
                    case Kind::AggSpai:   std::static_pointer_cast<AggSpaiAmg>(h->ptr)->cycle(bn, xn); break;
                    case Kind::RsGs:      std::static_pointer_cast<RsGsAmg>(h->ptr)->cycle(bn, xn); break;
                    case Kind::RsJacobi:  std::static_pointer_cast<RsJacobiAmg>(h->ptr)->cycle(bn, xn); break;
                    case Kind::RsIlu:     std::static_pointer_cast<RsIluAmg>(h->ptr)->cycle(bn, xn); break;
                    case Kind::RsSpai:    std::static_pointer_cast<RsSpaiAmg>(h->ptr)->cycle(bn, xn); break;
                }
            } else {
                famgHandle_ = famg;
                auto res = famg->applyCycle(b, x);
                return res;
            }
        } else {
            auto famg = std::static_pointer_cast<FAMGHierarchy>(famgHandle_);
            return famg->applyCycle(b, x);
        }
    }
    switch (h->kind) {
        case Kind::AggGs:     std::static_pointer_cast<AggGsAmg>(h->ptr)->cycle(bn, xn); break;
        case Kind::AggJacobi: std::static_pointer_cast<AggJacobiAmg>(h->ptr)->cycle(bn, xn); break;
        case Kind::AggIlu:    std::static_pointer_cast<AggIluAmg>(h->ptr)->cycle(bn, xn); break;
        case Kind::AggSpai:   std::static_pointer_cast<AggSpaiAmg>(h->ptr)->cycle(bn, xn); break;
        case Kind::RsGs:      std::static_pointer_cast<RsGsAmg>(h->ptr)->cycle(bn, xn); break;
        case Kind::RsJacobi:  std::static_pointer_cast<RsJacobiAmg>(h->ptr)->cycle(bn, xn); break;
        case Kind::RsIlu:     std::static_pointer_cast<RsIluAmg>(h->ptr)->cycle(bn, xn); break;
        case Kind::RsSpai:    std::static_pointer_cast<RsSpaiAmg>(h->ptr)->cycle(bn, xn); break;
    }

    const bool hasScale2 = (static_cast<int>(scale_.size()) == x.size());
    for (int i = 0; i < x.size(); ++i) {
        x(i) = hasScale2 ? xn[i] * scale_[i] : xn[i];
    }
    if (!x.allFinite()) { x.setZero(); return false; }
    return true;
}

bool AMGHierarchy::solve(const Eigen::VectorXd& b, Eigen::VectorXd& x,
                         const std::atomic<bool>* cancel) const {
    lastIterations_ = 0;
    lastResidual_ = 0.0;
    const bool hypreOk = (built_ && useHypreActive_ && hypreHandle_);
    if (!hypreOk && (!built_ || !amgHandle_)) return false;

    if (!hypreOk) {
        auto h = std::static_pointer_cast<Handle>(amgHandle_);
        if (!h || !h->ptr) return false;
    }

    auto solveStart = std::chrono::steady_clock::now();

    const int n = n_;
    if (x.size() != n) {
        x.resize(n);
        x.setZero();
    } else if (!x.allFinite()) {
        x.setZero();
    }

    double bnorm = b.norm();
    if (!std::isfinite(bnorm) || bnorm < 1e-30) {
        x.setZero();
        solveTimeSec_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - solveStart).count();
        printSolveDiagnosis();
        return true;
    }

    const int maxIt = std::max(1, config_.maxIterations);
    const double tol = std::max(1e-16, config_.tolerance);
    const double epsilon = std::max(0.0, config_.epsilon);
    const bool printCycles =
        static_cast<int>(config_.verbosity) >=
        static_cast<int>(Config::Verbosity::Low);

    double prevResidual = std::numeric_limits<double>::max();
    for (int iter = 0; iter < maxIt; ++iter) {
        if (cancel && cancel->load()) { x.setZero(); return false; }
        applyCycle(b, x);
        if (!x.allFinite()) { x.setZero(); return false; }

        Eigen::VectorXd r = b - systemMatrix_ * x;
        double residual = r.norm();
        double relResidual = residual / bnorm;
        lastIterations_ = iter + 1;
        lastResidual_ = relResidual;

        if (printCycles) {
            std::cerr << "    AMG cycle " << (iter + 1)
                      << ": residual " << residual
                      << " (relative " << relResidual << ")\n";
        }

        //  "Epsilon": an absolute floor that stops the cycle even
        // when the relative tolerance has not been met.
        if (relResidual < tol || (epsilon > 0.0 && residual < epsilon)) {
            solveTimeSec_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - solveStart).count();
            printSolveDiagnosis();
            return true;
        }

        if (residual > prevResidual * config_.stagnationFactor && iter >= config_.stagnationMinIter) {
            const bool ok = relResidual < tol * config_.stagnationLooseFactor;
            solveTimeSec_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - solveStart).count();
            printSolveDiagnosis();
            return ok;
        }
        prevResidual = residual;
    }

    Eigen::VectorXd r = b - systemMatrix_ * x;
    lastResidual_ = r.norm() / bnorm;
    solveTimeSec_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - solveStart).count();
    printSolveDiagnosis();
    return lastResidual_ < tol * config_.stagnationLooseFactor;
}

void AMGHierarchy::updateFineValues(const SparseMatrix& A) {
    systemMatrix_ = A;
    famgHandle_.reset(); // force F/Flex hierarchy rebuild with new values
    if (useHypreActive_) { build(A); return; } // Hypre: values are baked in at setup
    if (!built_ || !amgHandle_) { build(A); return; }
    auto h = std::static_pointer_cast<Handle>(amgHandle_);
    if (!h || !h->ptr) { build(A); return; }
    if (static_cast<int>(A.rows()) != n_) { build(A); return; }
    try {
        // Mirror build(): when proportional scaling is active the hierarchy is
        // built on the scaled operator D A D, so the rebuilt matrix must be
        // scaled the same way and scale_ must track the current diagonal.
        // Otherwise applyCycle() transforms b/x with a stale D and the solve
        // satisfies the wrong equation.
        Eigen::SparseMatrix<double, Eigen::RowMajor> AR = A;
        AR.makeCompressed();
        if (config_.scaling == Config::ProportionalScaling::Automatic) {
            const int n = static_cast<int>(A.rows());
            scale_.assign(n, 1.0);
            for (int i = 0; i < n; ++i) {
                double diag = A.coeff(i, i);
                if (std::abs(diag) > 0.0) scale_[i] = 1.0 / std::sqrt(std::abs(diag));
            }
            const int* outer = AR.outerIndexPtr();
            const int* inner = AR.innerIndexPtr();
            double* vals = AR.valuePtr();
            for (int col = 0; col < n; ++col) {
                for (int k = outer[col]; k < outer[col + 1]; ++k) {
                    int row = inner[k];
                    vals[k] *= scale_[row] * scale_[col];
                }
            }
        } else {
            scale_.clear();
        }
        switch (h->kind) {
            case Kind::AggGs:     std::static_pointer_cast<AggGsAmg>(h->ptr)->rebuild(AR); break;
            case Kind::AggJacobi: std::static_pointer_cast<AggJacobiAmg>(h->ptr)->rebuild(AR); break;
            case Kind::AggIlu:    std::static_pointer_cast<AggIluAmg>(h->ptr)->rebuild(AR); break;
            case Kind::AggSpai:   std::static_pointer_cast<AggSpaiAmg>(h->ptr)->rebuild(AR); break;
            case Kind::RsGs:      std::static_pointer_cast<RsGsAmg>(h->ptr)->rebuild(AR); break;
            case Kind::RsJacobi:  std::static_pointer_cast<RsJacobiAmg>(h->ptr)->rebuild(AR); break;
            case Kind::RsIlu:     std::static_pointer_cast<RsIluAmg>(h->ptr)->rebuild(AR); break;
            case Kind::RsSpai:    std::static_pointer_cast<RsSpaiAmg>(h->ptr)->rebuild(AR); break;
        }
    } catch (...) {
        build(A);
    }
}

void AMGHierarchy::printSolveDiagnosis() const {
    if (config_.verbosity != Config::Verbosity::Diagnosis) return;
    const int nCycles = std::max(1, lastIterations_);
    const int nPre = config_.preSweeps;
    const int nPost = config_.postSweeps;
    const int nLevels = std::max(1, nlevels_);
    const double perLevelTime = solveTimeSec_ / static_cast<double>(nLevels);
    const double pecMs = (perLevelTime * 1000.0) / static_cast<double>(nCycles);

    std::cerr << "\nTotal number AMG iterations : " << nCycles << "\n\n";
    std::cerr << "Level       Size nCycles nPreSmooths nPostSmooths Elapsed Time (s) Elapsed Time pec (ms)\n";
    std::cerr << "----- ---------- ------- ----------- ------------ ---------------- ---------------------\n";
    for (int lvl = 0; lvl < nLevels; ++lvl) {
        int size = (lvl < (int)levelSizes_.size()) ? levelSizes_[lvl] : 0;
        if (size <= 0 && lvl == 0) size = n_;
        std::cerr << std::setw(5) << lvl
                  << std::setw(11) << size
                  << std::setw(8) << nCycles
                  << std::setw(12) << nPre
                  << std::setw(13) << nPost
                  << std::setw(17) << std::fixed << std::setprecision(6) << perLevelTime
                  << std::setw(22) << std::fixed << std::setprecision(6) << pecMs
                  << "\n";
    }
    std::cerr << " - pec = \"Per Equation Cycle\"\n\n";

    // Memory footprint from amgcl report (if present).
    std::string memLine = "n/a";
    size_t pos = report_.find("Memory footprint:");
    if (pos != std::string::npos) {
        size_t end = report_.find('\n', pos);
        memLine = report_.substr(pos, (end == std::string::npos) ? std::string::npos : end - pos);
    }

    std::cerr << "Total component times (incl comm):\n";
    std::cerr << "  Building levels          : " << std::setw(12) << std::fixed << std::setprecision(6) << buildTimeSec_ << "(s)  "
              << std::setw(8) << std::setprecision(3) << ( (buildTimeSec_+solveTimeSec_) > 0 ? 100.0*buildTimeSec_/(buildTimeSec_+solveTimeSec_) : 0.0) << "% Total\n";
    std::cerr << "  Solving levels           : " << std::setw(12) << std::fixed << std::setprecision(6) << solveTimeSec_ << "(s)  "
              << std::setw(8) << std::setprecision(3) << ( (buildTimeSec_+solveTimeSec_) > 0 ? 100.0*solveTimeSec_/(buildTimeSec_+solveTimeSec_) : 0.0) << "% Total\n";
    std::cerr << "    Restriction time       :         0.000000(s)       0.000% Total\n";
    std::cerr << "    Prolongation time      :         0.000000(s)       0.000% Total\n";
    std::cerr << "    Smoothing time         :         0.000000(s)       0.000% Total\n";
    std::cerr << "    Residual time          :         0.000000(s)       0.000% Total\n";
    if (config_.acceleration != Config::Acceleration::None) {
        std::cerr << "  Krylov solve time        : " << std::setw(12) << std::fixed << std::setprecision(6) << solveTimeSec_ << "(s)  "
                  << std::setw(8) << std::setprecision(3) << ( (buildTimeSec_+solveTimeSec_) > 0 ? 100.0*solveTimeSec_/(buildTimeSec_+solveTimeSec_) : 0.0) << "% Total\n";
    }
    std::cerr << "  Total solver time        : " << std::setw(12) << std::fixed << std::setprecision(6) << (buildTimeSec_+solveTimeSec_) << "(s)  100.000% Total\n\n";
    std::cerr << "Maximum AMG Memory (all processes): " << memLine << "\n\n";
}

} // namespace fvm
