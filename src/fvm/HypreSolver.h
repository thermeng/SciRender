#pragma once
// Optional Hypre BoomerAMG backend for fvm::AMGHierarchy.
//
// Hypre (MIT/Apache dual, commercial OK) is vendored at vendor/hypre and
// built with HYPRE_ENABLE_MPI=OFF by default (single-rank serial build; MPI
// can be enabled with -DSCIRENDER_HYPRE_MPI=ON). This header is only
// compiled with FVM_HAVE_HYPRE defined (see the root CMakeLists.txt).
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
                            const cfd::AmgSettings& cfg,
                            HypreMat& mat, HypreSolverImpl& solver,
                            std::vector<double>& scale);
    // Apply one BoomerAMG cycle (MaxIter is forced to 1 in buildSolver) to
    // the caller's current guess `x`, never discarding it. Returns false on
    // failure or non-finite output.
    static bool applyCycle(HypreMat& mat, HypreSolverImpl& solver,
                           const Eigen::VectorXd& b, Eigen::VectorXd& x,
                           const std::vector<double>& scale);
    // Real level sizes of a built BoomerAMG hierarchy, derived from
    // HYPRE_BoomerAMGGetGridHierarchy(). levelSizes[0] is the finest grid.
    // cgrid[i] is the deepest level still containing row i (0 = fine only).
    // Returns false when the query fails (caller falls back to one level).
    static bool hierarchyLevelSizes(HypreSolverImpl& solver, int n,
                                    std::vector<int>& levelSizes,
                                    std::vector<int>& cgrid);
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
#include <iostream>
#include <mutex>

namespace fvm {

namespace hypre_detail {

// Single communicator for the serial/single-rank build. When MPI is on this
// is MPI_COMM_WORLD; when off it is the Hypre dummy communicator (0).
inline MPI_Comm hypreComm() { return MPI_COMM_WORLD; }

// HYPRE_Initialize() is idempotent in Hypre; call it exactly once so that
// every buildSolver() is safe without the caller remembering to init.
// Serial (HYPRE_SEQUENTIAL) builds need no MPI_Init. MPI builds still
// require the application to have called MPI_Init before any Hypre call
// (a library must not initialise MPI itself); we log a clear diagnostic
// instead of crashing inside Hypre if that was forgotten.
inline void ensureHypreInitialized() {
    static std::once_flag flag;
    std::call_once(flag, []() {
#if defined(HYPRE_HAVE_MPI) && !defined(HYPRE_SEQUENTIAL)
        int initialized = 0;
        MPI_Initialized(&initialized);
        if (!initialized) {
            std::cerr << "  Hypre: MPI not initialised by application; "
                         "call MPI_Init before using the Hypre AMG backend.\n";
        }
#endif
        HYPRE_Initialize();
    });
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
                                     const cfd::AmgSettings& cfg,
                                     HypreMat& mat, HypreSolverImpl& solver,
                                     std::vector<double>& scale) {
    // BoomerAMG has no block-size option (scalar coarsening only), so the
    // STAR "Group Size = Auto" (2 in 2D) padding has no purpose here. Padding
    // must not be done with SparseMatrix::resize(): resize() deallocates every
    // nonzero, which silently handed Hypre an empty matrix for any odd n
    // (16641 unknowns on the 129x129 cavity) and made AMG setup fail.
    int n = static_cast<int>(A.rows());
    SparseMatrix Aeff = A;
    if (n <= 0 || static_cast<int>(A.cols()) != n) return false;

    hypre_detail::ensureHypreInitialized();
    // Hypre's error flag is sticky: a failed setup leaves it set, and every
    // later Hypre call then returns the same stale code (e.g. all subsequent
    // IJMatrixCreate calls "failing" with the first setup's error). Clear it
    // before each build so a single failure does not cascade.
    HYPRE_ClearAllErrors();

    // Symmetric diagonal equilibration (same Automatic scaling rule as the
    // amgcl path). The caller must apply/invert `scale` around b/x.
    scale.clear();
    if (cfg.scaling == cfd::AmgSettings::ProportionalScaling::Automatic) {
        scale.assign(n, 1.0);
        for (int i = 0; i < n; ++i) {
            const double diag = Aeff.coeff(i, i);
            if (std::abs(diag) > 0.0)
                scale[i] = 1.0 / std::sqrt(std::abs(diag));
        }
    }

    // Bandwidth reorder is internal to Hypre; keep AR as Original ordering,
    // but sort each row by column (Hypre expects sorted IJ rows).
    Eigen::SparseMatrix<double, Eigen::RowMajor> AR = Aeff;
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

    auto fail = [](const char* what, HYPRE_Int rc) {
        std::cerr << "  Hypre: " << what << " failed (rc=" << rc << ")\n";
        return false;
    };

    HYPRE_IJMatrix ij = nullptr;
    if (HYPRE_Int rc = HYPRE_IJMatrixCreate(hypre_detail::hypreComm(), ilower, iupper, jlower, jupper, &ij); rc != 0) {
        return fail("IJMatrixCreate", rc);
    }
    if (HYPRE_Int rc = HYPRE_IJMatrixSetObjectType(ij, HYPRE_PARCSR); rc != 0) {
        HYPRE_IJMatrixDestroy(ij);
        return fail("IJMatrixSetObjectType", rc);
    }
    if (HYPRE_Int rc = HYPRE_IJMatrixInitialize(ij); rc != 0) {
        HYPRE_IJMatrixDestroy(ij);
        return fail("IJMatrixInitialize", rc);
    }
    // SetRowSizes is optional but avoids reallocation during SetValues.
    if (HYPRE_Int rc = HYPRE_IJMatrixSetRowSizes(ij, rowSizes.data()); rc != 0) {
        HYPRE_IJMatrixDestroy(ij);
        return fail("IJMatrixSetRowSizes", rc);
    }
    if (HYPRE_Int rc = HYPRE_IJMatrixSetValues(ij, static_cast<HYPRE_Int>(n), ncols.data(),
                                rows.data(), cols.data(), values.data()); rc != 0) {
        HYPRE_IJMatrixDestroy(ij);
        return fail("IJMatrixSetValues", rc);
    }
    if (HYPRE_Int rc = HYPRE_IJMatrixAssemble(ij); rc != 0) {
        HYPRE_IJMatrixDestroy(ij);
        return fail("IJMatrixAssemble", rc);
    }
    mat.mat = static_cast<void*>(ij);

    HYPRE_ParCSRMatrix parA = nullptr;
    if (HYPRE_Int rc = HYPRE_IJMatrixGetObject(ij, reinterpret_cast<void**>(&parA)); rc != 0 || !parA) {
        HYPRE_IJMatrixDestroy(ij);
        mat.mat = nullptr;
        return fail("IJMatrixGetObject (matrix)", rc);
    }

    // --- BoomerAMG configuration -----------------------------------------
    HYPRE_Solver amg = nullptr;
    if (HYPRE_Int rc = HYPRE_BoomerAMGCreate(&amg); rc != 0) return fail("BoomerAMGCreate", rc);
    solver.solver = static_cast<void*>(amg);

    // V-cycle (1) / W-cycle (2).
    const HYPRE_Int cycleType =
        (cfg.cycleType == cfd::AmgSettings::CycleType::WCycle) ? 2 : 1;
    HYPRE_BoomerAMGSetCycleType(amg, cycleType);
    // F-cycle (Full multigrid).
    if (cfg.cycleType == cfd::AmgSettings::CycleType::FCycle)
        HYPRE_BoomerAMGSetFCycle(amg, 1);

    // Flexible cycle: we no longer use Hypre's static FlexibleCycleStruct
    // (depth-capped at 1). The recursive residual-driven traversal lives in
    // AMGHierarchy::flexCycle, which evaluates alpha/beta gates at every
    // level. BoomerAMG is only used here to build the hierarchy (cgrid +
    // level sizes); the actual multigrid cycling is done by AMGHierarchy.
    if (cfg.cycleType == cfd::AmgSettings::CycleType::Flexible) {
        const HYPRE_Int sweeps =
            cfg.flexMaxSweeps > 0 ? static_cast<HYPRE_Int>(cfg.flexMaxSweeps) : 1;
        HYPRE_BoomerAMGSetNumSweeps(amg, sweeps);
    }

    // Smoother: map Config::Smoother where Hypre has an equivalent relax
    // type (Jacobi = 0, hybrid Gauss-Seidel = 3). ILU0/SPAI0 have no
    // BoomerAMG relax type, so they fall back to Gauss-Seidel; log that
    // instead of silently ignoring the selection.
    {
        const HYPRE_Int relaxType =
            (cfg.smoother == cfd::AmgSettings::Smoother::Jacobi) ? 0 : 3;
        if (cfg.smoother == cfd::AmgSettings::Smoother::ILU) {
            std::cerr << "  Hypre: smoother ILU0/SPAI0 has no BoomerAMG "
                         "equivalent; using Gauss-Seidel.\n";
        }
        HYPRE_BoomerAMGSetRelaxType(amg, relaxType);
    }
    HYPRE_BoomerAMGSetCycleRelaxType(amg, 4, 2);
    HYPRE_BoomerAMGSetCycleRelaxType(amg, 9, 3);

    // Pre/post smoothing sweeps. STAR exposes 1/1 for pressure and a single
    // sweep count for momentum. A Flexible cycle already applied its own
    // flexMaxSweeps above, so only the fixed cycles map pre/post separately.
    if (cfg.cycleType != cfd::AmgSettings::CycleType::Flexible) {
        const HYPRE_Int pre =
            static_cast<HYPRE_Int>(cfg.preSweeps > 0 ? cfg.preSweeps : 1);
        const HYPRE_Int post =
            static_cast<HYPRE_Int>(cfg.postSweeps > 0 ? cfg.postSweeps : 1);
        HYPRE_BoomerAMGSetCycleNumSweeps(amg, pre, 1);
        HYPRE_BoomerAMGSetCycleNumSweeps(amg, post, 2);
    }

    // Research: Hypre default 0.25 sits on boundary -> 1 level. Need 0.5+.
    const double strength =
        (cfg.strengthThreshold > 0.0) ? cfg.strengthThreshold : 0.5;
    HYPRE_BoomerAMGSetStrongThreshold(amg, strength);
    // Coarsening algorithm selected in the UI. BoomerAMG has no smoothed
    // aggregation variant, so the two UI choices map to the closest classical
    // coarsening Hypre provides:
    //   Ruge-Stuben        -> 3  (classical RS on each rank + boundary pass)
    //   SmoothedAggregation -> 10 (HMIS, Hypre's robust default; SA itself is
    //                              not available in BoomerAMG)
    const HYPRE_Int coarsenType =
        (cfg.coarsening == cfd::AmgSettings::Coarsening::RugeStuben) ? 3 : 10;
    HYPRE_BoomerAMGSetCoarsenType(amg, coarsenType);
    HYPRE_BoomerAMGSetInterpType(amg, 6); // extended+i (Hypre default)
    HYPRE_BoomerAMGSetMaxRowSum(amg, 1.0); // disable diag dom check
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

    // Hypre's native BoomerAMG printing is disabled (print_level=0) so it
    // never writes to the terminal or any GUI output stream.
    HYPRE_BoomerAMGSetPrintLevel(amg, 0);

    // --- Setup with zero IJ vectors --------------------------------------
    HYPRE_IJVector bvec = nullptr, xvec = nullptr;
    HYPRE_ParVector parB = nullptr, parX = nullptr;
    std::vector<HYPRE_BigInt> indices(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        indices[static_cast<std::size_t>(i)] = static_cast<HYPRE_BigInt>(i);
    std::vector<HYPRE_Real> zeros(static_cast<std::size_t>(n), 0.0);

    if (HYPRE_Int rc = HYPRE_IJVectorCreate(hypre_detail::hypreComm(), ilower, iupper, &bvec); rc != 0 ||
        HYPRE_IJVectorCreate(hypre_detail::hypreComm(), ilower, iupper, &xvec) != 0) {
        std::cerr << "  Hypre: IJVectorCreate failed\n";
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
    const HYPRE_Int setupRc = (parB && parX) ? HYPRE_BoomerAMGSetup(amg, parA, parB, parX) : -1;
    if (bvec) HYPRE_IJVectorDestroy(bvec);
    if (xvec) HYPRE_IJVectorDestroy(xvec);
    if (!parB || !parX) return fail("IJVectorGetObject", setupRc);
    if (setupRc != 0) {
        char descr[256] = {0};
        HYPRE_DescribeError(setupRc, descr);
        int emptyRows = 0, zeroDiag = 0;
        double minAbsDiag = 1e300, maxAbsDiag = 0.0;
        const int* aouter = AR.outerIndexPtr();
        for (int i = 0; i < n; ++i) {
            const double d = std::abs(Aeff.coeff(i, i));
            if (aouter[i + 1] - aouter[i] == 0) ++emptyRows;
            if (d == 0.0) ++zeroDiag;
            else { minAbsDiag = std::min(minAbsDiag, d); maxAbsDiag = std::max(maxAbsDiag, d); }
        }
        std::cerr << "  Hypre: BoomerAMGSetup failed (rc=" << setupRc << ") " << descr
                  << "\n    n=" << n << " nnz=" << Aeff.nonZeros()
                  << " emptyRows=" << emptyRows << " zeroDiag=" << zeroDiag
                  << " |diag| in [" << minAbsDiag << ", " << maxAbsDiag << "]"
                  << " strength=" << strength << "\n";
        return false;
    }
    return true;
}

inline bool HypreSolver::applyCycle(HypreMat& mat, HypreSolverImpl& solver,
                                    const Eigen::VectorXd& b,
                                    Eigen::VectorXd& x,
                                    const std::vector<double>& scale) {
    if (!mat.ok() || !solver.ok()) return false;
    const int origN = static_cast<int>(b.size());
    if (origN <= 0 || static_cast<int>(x.size()) != origN) return false;
    // Pad b/x to Hypre matrix size (padded rows) — scale holds padded size
    int n = origN;
    if (!scale.empty() && (int)scale.size() > n) n = (int)scale.size();

    hypre_detail::ensureHypreInitialized();
    // Same sticky-error hazard as buildSolver(): clear before every solve so
    // one failed cycle does not make every later Hypre call report failure.
    HYPRE_ClearAllErrors();

    HYPRE_IJMatrix ij = static_cast<HYPRE_IJMatrix>(mat.mat);
    HYPRE_ParCSRMatrix parA = nullptr;
    if (HYPRE_IJMatrixGetObject(ij, reinterpret_cast<void**>(&parA)) != 0 || !parA) return false;

    const bool hasScale = !scale.empty();
    std::vector<HYPRE_Real> bdata(static_cast<std::size_t>(n), 0.0), xdata(static_cast<std::size_t>(n), 0.0);
    for (int i = 0; i < origN; ++i) {
        bdata[static_cast<std::size_t>(i)] = static_cast<HYPRE_Real>(hasScale ? b(i) * scale[i] : b(i));
        xdata[static_cast<std::size_t>(i)] = static_cast<HYPRE_Real>(hasScale ? x(i) / scale[i] : x(i));
    }
    // padded rows: b=0, x=0 (identity rows solve to 0)

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
    for (int i = 0; i < origN; ++i)
        x(i) = static_cast<double>(xout[static_cast<std::size_t>(i)]) * (hasScale ? scale[i] : 1.0);
    HYPRE_IJVectorDestroy(bvec);
    HYPRE_IJVectorDestroy(xvec);
    if (!x.allFinite()) { x.setZero(); return false; }
    return rc == 0;
}

inline bool HypreSolver::hierarchyLevelSizes(HypreSolverImpl& solver, int n,
                                             std::vector<int>& levelSizes,
                                             std::vector<int>& cgrid) {
    levelSizes.clear();
    cgrid.clear();
    if (!solver.solver || n <= 0) return false;
    // Sticky error flag: clear first so a stale failure does not mask this
    // (read-only) query, and clear again so we leave no trace for the caller.
    HYPRE_ClearAllErrors();
    std::vector<HYPRE_Int> cgridTmp(static_cast<std::size_t>(n), 0);
    if (HYPRE_BoomerAMGGetGridHierarchy(static_cast<HYPRE_Solver>(solver.solver),
                                        cgridTmp.data()) != 0) {
        HYPRE_ClearAllErrors();
        return false;
    }
    cgrid.assign(cgridTmp.begin(), cgridTmp.end());
    // cgrid[i] is the deepest level still containing row i (0 = fine only),
    // so level L holds every row with cgrid[i] >= L.
    HYPRE_Int maxLevel = 0;
    for (HYPRE_Int v : cgridTmp) maxLevel = std::max(maxLevel, v);
    levelSizes.assign(static_cast<std::size_t>(maxLevel) + 1, 0);
    for (HYPRE_Int lvl = 0; lvl <= maxLevel; ++lvl) {
        int count = 0;
        for (HYPRE_Int v : cgridTmp) {
            if (v >= lvl) ++count;
        }
        levelSizes[static_cast<std::size_t>(lvl)] = count;
    }
    HYPRE_ClearAllErrors();
    return true;
}

} // namespace fvm
#endif // FVM_HAVE_HYPRE
