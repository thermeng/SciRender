#pragma once
// Optional Hypre BoomerAMG backend for fvm::AMGHierarchy.
//
// Hypre (MIT/Apache dual, commercial OK) is vendored at vendor/hypre and
// built with HYPRE_ENABLE_MPI=OFF by default (single-rank serial build; MPI
// can be enabled with -DSCIRENDER_HYPRE_MPI=ON). This header is only
// compiled with FVM_HAVE_HYPRE defined (see the root CMakeLists.txt).
//
// STAR-CCM+ parity target (lid 129x129): BoomerAMG with strength 0.25,
// max 50 levels, coarse solve direct at <= 2 unknowns, Gauss-Seidel
// smoother, V/W/F cycles and a Flexible cycle driven by a -2/-1/0/1 walk
// struct (Terminate/Coarser/Stay/Finer) with prolongation alpha = 0.5 and
// restriction beta = 0.9, 1 sweep.
//
// Matrix/vector exchange uses the Hypre master IJ interface (HYPRE_IJMatrix
// / HYPRE_IJVector) instead of the removed-in-master ParCSR direct
// construction (SetNumNonzeros / SetOffDiagValues / ParVectorGetData).

#include "fvm/Types.h"

#include <memory>
#include <vector>

namespace fvm {

#ifdef FVM_HAVE_HYPRE
class HypreAMGHierarchy;
#endif

// SparseMatrix -> HYPRE_IJMatrix (+ BoomerAMG) converter + solver wrapper.
// Serial use: the matrix/vector live on a single rank (MPI_COMM_WORLD when
// MPI is on, a dummy communicator when Hypre is built with MPI OFF).
// Used by AMGHierarchy when Config::useHypre == true and Hypre is linked in;
// AMGHierarchy falls back to amgcl otherwise, so the API stays identical.
class HypreSolver {
public:
    // Export an Eigen SparseMatrix to a HYPRE_IJMatrix (owned by the
    // caller via HypreMat). Returns false on failure.
    struct HypreMat;
    struct HypreSolverImpl;

    // RAII holder for a HYPRE_IJMatrix (the underlying ParCSR object is
    // obtained via HYPRE_IJMatrixGetObject for BoomerAMG setup/solve).
    struct HypreMat {
#ifdef FVM_HAVE_HYPRE
        void* mat = nullptr; // HYPRE_IJMatrix
#else
        void* mat = nullptr;
#endif
        ~HypreMat();
        HypreMat() = default;
        HypreMat(const HypreMat&) = delete;
        HypreMat& operator=(const HypreMat&) = delete;
        bool ok() const { return mat != nullptr; }
    };

    // RAII holder for a HYPRE_Solver (BoomerAMG).
    struct HypreSolverImpl {
#ifdef FVM_HAVE_HYPRE
        void* solver = nullptr; // HYPRE_Solver
#else
        void* solver = nullptr;
#endif
        ~HypreSolverImpl();
        HypreSolverImpl() = default;
        HypreSolverImpl(const HypreSolverImpl&) = delete;
        HypreSolverImpl& operator=(const HypreSolverImpl&) = delete;
        bool ok() const { return solver != nullptr; }
    };

    // Build a BoomerAMG hierarchy for `A` (already optionally scaled by the
    // caller). `cycleType`, `flexStruct`, `flexAlpha`, `flexBeta` and
    // `flexMaxSweeps` select V/W/F/Flexible exactly as AMGHierarchy::Config
    // describes. Returns true on success.
#ifdef FVM_HAVE_HYPRE
    static bool buildSolver(const SparseMatrix& A,
                            const class AMGHierarchy::Config& cfg,
                            HypreMat& mat, HypreSolverImpl& solver,
                            std::vector<double>& scale);
    // Apply one BoomerAMG cycle (MaxIter is forced to 1 in buildSolver) to
    // the caller's current guess `x`, never discarding it. Returns false on
    // failure or non-finite output.
    static bool applyCycle(HypreMat& mat, HypreSolverImpl& solver,
                           const Eigen::VectorXd& b, Eigen::VectorXd& x,
                           const std::vector<double>& scale);
    static bool hypreAvailable() { return true; }
#else
    static bool hypreAvailable() { return false; }
#endif
};

} // namespace fvm

#ifdef FVM_HAVE_HYPRE
// ---- Hypre implementation -------------------------------------------------
// Included in the header so AMGHierarchy (AMG.cpp) only needs to call into
// it; no new translation unit is required. HYPRE_* types are only visible
// when FVM_HAVE_HYPRE is set, so amgcl builds never touch Hypre headers.

#include "fvm/AMG.h"

#include <HYPRE_utilities.h>
#include <HYPRE.h>
#include <HYPRE_parcsr_ls.h>
#include <HYPRE_IJ_mv.h>
#include <HYPRE_parcsr_mv.h>

// MPI is optional: when Hypre is built with HYPRE_ENABLE_MPI=OFF it defines
// HYPRE_SEQUENTIAL and HYPRE_utilities.h provides MPI_Comm (as an int)
// without including <mpi.h> and without MPI_COMM_WORLD. Provide a dummy
// communicator for the single-rank serial case.
#ifdef HYPRE_SEQUENTIAL
#ifndef MPI_COMM_WORLD
#define MPI_COMM_WORLD ((MPI_Comm)0)
#endif
#elif defined(HYPRE_HAVE_MPI)
#include <mpi.h>
#endif

#include <cmath>
#include <cstdlib>
#include <mutex>

namespace fvm {

namespace hypre_detail {

// Single communicator for the serial/single-rank build. When MPI is on this
// is MPI_COMM_WORLD; when off it is the Hypre dummy communicator (0).
inline MPI_Comm hypreComm() { return MPI_COMM_WORLD; }

// HYPRE_Initialize() is idempotent in Hypre; call it exactly once so that
// every buildSolver() is safe without the caller remembering to init.
inline void ensureHypreInitialized() {
    static std::once_flag flag;
    std::call_once(flag, []() { HYPRE_Initialize(); });
}

} // namespace hypre_detail

inline HypreSolver::HypreMat::~HypreMat() {
    if (mat) {
        HYPRE_IJMatrixDestroy(static_cast<HYPRE_IJMatrix>(mat));
        mat = nullptr;
    }
}

inline HypreSolver::HypreSolverImpl::~HypreSolverImpl() {
    if (solver) {
        HYPRE_BoomerAMGDestroy(static_cast<HYPRE_Solver>(solver));
        solver = nullptr;
    }
}

inline bool HypreSolver::buildSolver(const SparseMatrix& A,
                                     const AMGHierarchy::Config& cfg,
                                     HypreMat& mat, HypreSolverImpl& solver,
                                     std::vector<double>& scale) {
    const int n = static_cast<int>(A.rows());
    if (n <= 0 || static_cast<int>(A.cols()) != n) return false;

    hypre_detail::ensureHypreInitialized();

    // Symmetric diagonal equilibration (same Automatic scaling rule as the
    // amgcl path). The caller must apply/invert `scale` around b/x.
    scale.clear();
    if (cfg.scaling == AMGHierarchy::Config::ProportionalScaling::Automatic) {
        scale.assign(n, 1.0);
        for (int i = 0; i < n; ++i) {
            const double diag = A.coeff(i, i);
            if (std::abs(diag) > 0.0)
                scale[i] = 1.0 / std::sqrt(std::abs(diag));
        }
    }

    // Copy into RowMajor CSR so we can hand Hypre row-by-row arrays.
    Eigen::SparseMatrix<double, Eigen::RowMajor> AR = A;
    AR.makeCompressed();
    if (!scale.empty()) {
        const int* outer = AR.outerIndexPtr();
        const int* inner = AR.innerIndexPtr();
        double* vals = AR.valuePtr();
        for (int col = 0; col < n; ++col) {
            for (int k = outer[col]; k < outer[col + 1]; ++k) {
                const int row = inner[k];
                vals[k] *= scale[row] * scale[col];
            }
        }
    }

    // --- Export to HYPRE_IJMatrix -----------------------------------------
    const HYPRE_BigInt ilower = 0;
    const HYPRE_BigInt iupper = static_cast<HYPRE_BigInt>(n - 1);
    const HYPRE_BigInt jlower = 0;
    const HYPRE_BigInt jupper = static_cast<HYPRE_BigInt>(n - 1);

    // Flatten the CSR rows into IJ arrays. rows[] is one entry per row,
    // ncols[] is the count per row, cols[]/values[] are concatenated in
    // row order — exactly what HYPRE_IJMatrixSetValues expects.
    std::vector<HYPRE_Int> rowSizes(static_cast<std::size_t>(n), 0);
    std::vector<HYPRE_Int> ncols(static_cast<std::size_t>(n), 0);
    std::vector<HYPRE_BigInt> rows(static_cast<std::size_t>(n), 0);
    std::vector<HYPRE_BigInt> cols;
    std::vector<HYPRE_Complex> values;
    {
        const int* outer = AR.outerIndexPtr();
        const int* inner = AR.innerIndexPtr();
        const double* vals = AR.valuePtr();
        cols.reserve(static_cast<std::size_t>(AR.nonZeros()));
        values.reserve(static_cast<std::size_t>(AR.nonZeros()));
        for (int i = 0; i < n; ++i) {
            const int begin = outer[i];
            const int end = outer[i + 1];
            const int cnt = end - begin;
            rowSizes[static_cast<std::size_t>(i)] = static_cast<HYPRE_Int>(cnt);
            ncols[static_cast<std::size_t>(i)] = static_cast<HYPRE_Int>(cnt);
            rows[static_cast<std::size_t>(i)] = static_cast<HYPRE_BigInt>(i);
            for (int k = begin; k < end; ++k) {
                cols.push_back(static_cast<HYPRE_BigInt>(inner[k]));
                values.push_back(static_cast<HYPRE_Complex>(vals[k]));
            }
        }
    }

    HYPRE_IJMatrix ij = nullptr;
    if (HYPRE_IJMatrixCreate(hypre_detail::hypreComm(), ilower, iupper, jlower, jupper, &ij) != 0) {
        return false;
    }
    if (HYPRE_IJMatrixSetObjectType(ij, HYPRE_PARCSR) != 0 ||
        HYPRE_IJMatrixInitialize(ij) != 0) {
        HYPRE_IJMatrixDestroy(ij);
        return false;
    }
    // SetRowSizes is optional but avoids reallocation during SetValues.
    if (HYPRE_IJMatrixSetRowSizes(ij, rowSizes.data()) != 0) {
        HYPRE_IJMatrixDestroy(ij);
        return false;
    }
    if (HYPRE_IJMatrixSetValues(ij, static_cast<HYPRE_Int>(n), ncols.data(),
                                rows.data(), cols.data(), values.data()) != 0) {
        HYPRE_IJMatrixDestroy(ij);
        return false;
    }
    if (HYPRE_IJMatrixAssemble(ij) != 0) {
        HYPRE_IJMatrixDestroy(ij);
        return false;
    }
    mat.mat = static_cast<void*>(ij);

    HYPRE_ParCSRMatrix parA = nullptr;
    if (HYPRE_IJMatrixGetObject(ij, reinterpret_cast<void**>(&parA)) != 0 || !parA) {
        HYPRE_IJMatrixDestroy(ij);
        mat.mat = nullptr;
        return false;
    }

    // --- BoomerAMG configuration -----------------------------------------
    HYPRE_Solver amg = nullptr;
    if (HYPRE_BoomerAMGCreate(&amg) != 0) return false;
    solver.solver = static_cast<void*>(amg);

    // V-cycle (1) / W-cycle (2).
    const HYPRE_Int cycleType =
        (cfg.cycleType == AMGHierarchy::Config::CycleType::WCycle) ? 2 : 1;
    HYPRE_BoomerAMGSetCycleType(amg, cycleType);
    // F-cycle (Full multigrid).
    if (cfg.cycleType == AMGHierarchy::Config::CycleType::FCycle)
        HYPRE_BoomerAMGSetFCycle(amg, 1);

    // Flexible cycle: -2 Terminate / -1 Coarser / 0 Stay / 1 Finer walk.
    // Alpha (prolongation) and beta (restriction) are the coarse-grid
    // correction scaling factors applied on every Finer step; 1 sweep each.
    // NOTE: HYPRE_BoomerAMGSetFlexibleCycleStruct / SetFlexibleCGCScalingFactors
    // store the array pointer without copying and free it with hypre_TFree on
    // BoomerAMGDestroy. The arrays MUST therefore be malloc-allocated (hypre's
    // host allocator is malloc) and ownership transferred to Hypre; passing a
    // std::vector buffer here leaves a dangling pointer after buildSolver
    // returns (heap corruption on the next solve).
    if (cfg.cycleType == AMGHierarchy::Config::CycleType::Flexible) {
        std::vector<HYPRE_Int> flexTmp;
        if (!cfg.flexStruct.empty()) {
            for (auto m : cfg.flexStruct)
                flexTmp.push_back(static_cast<HYPRE_Int>(m));
        } else {
            // Default walk: stay, go coarser, go finer, stay, terminate.
            // Depth 1 (not the older depth-3 walk): the flex walk depth must
            // not exceed the actual hierarchy depth. On small problems (e.g.
            // the 20x20/32x32 test Laplacians, 3-4 levels) a depth-3 walk
            // overshoots the coarsest level and the cycle then stalls
            // (~0.9 residual factor per cycle, never reaching tolerance in
            // 30 cycles). Depth 1 converges on both the small test
            // problems and the 129x129 lid target (~0.44-0.48 per cycle
            // with alpha=0.5, i.e. within maxIterations=30). Trailing -2
            // is required by Hypre. Explicit Config::flexStruct walks are
            // honoured verbatim.
            flexTmp = {0, -1, 1, 0, -2};
        }
        std::vector<HYPRE_Real> cgcTmp;
        cgcTmp.reserve(flexTmp.size());
        for (HYPRE_Int step : flexTmp)
            cgcTmp.push_back(step == 1 ? static_cast<HYPRE_Real>(cfg.flexAlpha) : 1.0);
        const std::size_t nFlex = flexTmp.size();
        HYPRE_Int* flex = static_cast<HYPRE_Int*>(
            std::malloc(nFlex * sizeof(HYPRE_Int)));
        HYPRE_Real* cgc = static_cast<HYPRE_Real*>(
            std::malloc(nFlex * sizeof(HYPRE_Real)));
        if (!flex || !cgc) {
            if (flex) std::free(flex);
            if (cgc) std::free(cgc);
            // Fall back to a non-flexible V-cycle rather than failing.
            HYPRE_BoomerAMGSetNumSweeps(amg, 1);
        } else {
            for (std::size_t i = 0; i < nFlex; ++i) {
                flex[i] = flexTmp[i];
                cgc[i] = cgcTmp[i];
            }
            // Ownership of flex/cgc passes to Hypre (freed on Destroy).
            HYPRE_BoomerAMGSetFlexibleCycleStruct(amg, flex);
            // HYPRE exposes per-step coarse-grid correction scaling
            // (prolongation). Beta (restriction) has no direct Hypre knob; it is
            // kept in the config and honoured by the own FAMGHierarchy flexible
            // walk, so here it is recorded but not passed to Hypre.
            (void)cfg.flexBeta;
            HYPRE_BoomerAMGSetFlexibleCGCScalingFactors(amg, cgc);
            const HYPRE_Int sweeps =
                cfg.flexMaxSweeps > 0 ? static_cast<HYPRE_Int>(cfg.flexMaxSweeps) : 1;
            HYPRE_BoomerAMGSetNumSweeps(amg, sweeps);
        }
    }

    // Gauss-Seidel smoother: hybrid GS forward on the down cycle, backward
    // on the up cycle, Gaussian elimination on the coarsest level.
    HYPRE_BoomerAMGSetRelaxType(amg, 3);
    HYPRE_BoomerAMGSetCycleRelaxType(amg, 4, 2);
    HYPRE_BoomerAMGSetCycleRelaxType(amg, 9, 3);

    // Strength 0.25 (2D Laplace optimum; also Hypre's own default).
    HYPRE_BoomerAMGSetStrongThreshold(amg, 0.25);
    // Max levels 50.
    HYPRE_BoomerAMGSetMaxLevels(amg,
        cfg.maxLevels > 0 ? static_cast<HYPRE_Int>(cfg.maxLevels) : 50);
    // Coarse solve direct at <= 2 unknowns (Gaussian elimination above).
    HYPRE_BoomerAMGSetMaxCoarseSize(amg,
        cfg.coarseEnough > 0 ? static_cast<HYPRE_Int>(cfg.coarseEnough) : 2);
    // Automatic proportional scaling: the equilibration above (scale_) is
    // the Automatic rule; Disabled keeps the raw operator. Hypre has no
    // separate proportional-scaling flag for BoomerAMG, so nothing to set.
    (void)cfg.scaling;

    // One cycle per applyCycle(): solver tolerance off, max 1 iteration.
    HYPRE_BoomerAMGSetTol(amg, 0.0);
    HYPRE_BoomerAMGSetMaxIter(amg, 1);

    // --- Setup with zero IJ vectors --------------------------------------
    HYPRE_IJVector bvec = nullptr, xvec = nullptr;
    HYPRE_ParVector parB = nullptr, parX = nullptr;
    std::vector<HYPRE_BigInt> indices(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        indices[static_cast<std::size_t>(i)] = static_cast<HYPRE_BigInt>(i);
    std::vector<HYPRE_Real> zeros(static_cast<std::size_t>(n), 0.0);

    if (HYPRE_IJVectorCreate(hypre_detail::hypreComm(), ilower, iupper, &bvec) != 0 ||
        HYPRE_IJVectorCreate(hypre_detail::hypreComm(), ilower, iupper, &xvec) != 0) {
        if (bvec) HYPRE_IJVectorDestroy(bvec);
        if (xvec) HYPRE_IJVectorDestroy(xvec);
        return false;
    }
    HYPRE_IJVectorSetObjectType(bvec, HYPRE_PARCSR);
    HYPRE_IJVectorSetObjectType(xvec, HYPRE_PARCSR);
    HYPRE_IJVectorInitialize(bvec);
    HYPRE_IJVectorInitialize(xvec);
    HYPRE_IJVectorSetValues(bvec, static_cast<HYPRE_Int>(n), indices.data(), zeros.data());
    HYPRE_IJVectorSetValues(xvec, static_cast<HYPRE_Int>(n), indices.data(), zeros.data());
    HYPRE_IJVectorAssemble(bvec);
    HYPRE_IJVectorAssemble(xvec);
    HYPRE_IJVectorGetObject(bvec, reinterpret_cast<void**>(&parB));
    HYPRE_IJVectorGetObject(xvec, reinterpret_cast<void**>(&parX));
    const bool setupOk = (parB && parX && HYPRE_BoomerAMGSetup(amg, parA, parB, parX) == 0);
    if (bvec) HYPRE_IJVectorDestroy(bvec);
    if (xvec) HYPRE_IJVectorDestroy(xvec);
    return setupOk;
}

inline bool HypreSolver::applyCycle(HypreMat& mat, HypreSolverImpl& solver,
                                    const Eigen::VectorXd& b,
                                    Eigen::VectorXd& x,
                                    const std::vector<double>& scale) {
    if (!mat.ok() || !solver.ok()) return false;
    const int n = static_cast<int>(b.size());
    if (n <= 0 || static_cast<int>(x.size()) != n) return false;

    hypre_detail::ensureHypreInitialized();

    HYPRE_IJMatrix ij = static_cast<HYPRE_IJMatrix>(mat.mat);
    HYPRE_ParCSRMatrix parA = nullptr;
    if (HYPRE_IJMatrixGetObject(ij, reinterpret_cast<void**>(&parA)) != 0 || !parA) return false;

    const bool hasScale = (static_cast<int>(scale.size()) == n);
    std::vector<HYPRE_Real> bdata(static_cast<std::size_t>(n)), xdata(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        bdata[static_cast<std::size_t>(i)] = static_cast<HYPRE_Real>(hasScale ? b(i) * scale[i] : b(i));
        xdata[static_cast<std::size_t>(i)] = static_cast<HYPRE_Real>(hasScale ? x(i) / scale[i] : x(i));
    }

    const HYPRE_BigInt ilower = 0;
    const HYPRE_BigInt iupper = static_cast<HYPRE_BigInt>(n - 1);
    std::vector<HYPRE_BigInt> indices(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        indices[static_cast<std::size_t>(i)] = static_cast<HYPRE_BigInt>(i);

    HYPRE_IJVector bvec = nullptr, xvec = nullptr;
    HYPRE_ParVector parB = nullptr, parX = nullptr;
    if (HYPRE_IJVectorCreate(hypre_detail::hypreComm(), ilower, iupper, &bvec) != 0 ||
        HYPRE_IJVectorCreate(hypre_detail::hypreComm(), ilower, iupper, &xvec) != 0) {
        if (bvec) HYPRE_IJVectorDestroy(bvec);
        if (xvec) HYPRE_IJVectorDestroy(xvec);
        return false;
    }
    HYPRE_IJVectorSetObjectType(bvec, HYPRE_PARCSR);
    HYPRE_IJVectorSetObjectType(xvec, HYPRE_PARCSR);
    HYPRE_IJVectorInitialize(bvec);
    HYPRE_IJVectorInitialize(xvec);
    HYPRE_IJVectorSetValues(bvec, static_cast<HYPRE_Int>(n), indices.data(), bdata.data());
    HYPRE_IJVectorSetValues(xvec, static_cast<HYPRE_Int>(n), indices.data(), xdata.data());
    HYPRE_IJVectorAssemble(bvec);
    HYPRE_IJVectorAssemble(xvec);
    HYPRE_IJVectorGetObject(bvec, reinterpret_cast<void**>(&parB));
    HYPRE_IJVectorGetObject(xvec, reinterpret_cast<void**>(&parX));
    if (!parB || !parX) {
        HYPRE_IJVectorDestroy(bvec);
        HYPRE_IJVectorDestroy(xvec);
        return false;
    }
    const HYPRE_Int rc =
        HYPRE_BoomerAMGSolve(static_cast<HYPRE_Solver>(solver.solver), parA, parB, parX);
    // IJ GetValues replaces the removed HYPRE_ParVectorGetData.
    std::vector<HYPRE_Real> xout(static_cast<std::size_t>(n), 0.0);
    if (HYPRE_IJVectorGetValues(xvec, static_cast<HYPRE_Int>(n), indices.data(), xout.data()) != 0) {
        HYPRE_IJVectorDestroy(bvec);
        HYPRE_IJVectorDestroy(xvec);
        return false;
    }
    for (int i = 0; i < n; ++i)
        x(i) = static_cast<double>(xout[static_cast<std::size_t>(i)]) * (hasScale ? scale[i] : 1.0);
    HYPRE_IJVectorDestroy(bvec);
    HYPRE_IJVectorDestroy(xvec);
    if (!x.allFinite()) { x.setZero(); return false; }
    return rc == 0;
}

} // namespace fvm
#endif // FVM_HAVE_HYPRE
