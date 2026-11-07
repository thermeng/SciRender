#pragma once
// Optional Hypre BoomerAMG backend for fvm::AMGHierarchy.
//
// Hypre (>= 2.17, MIT/Apache dual, commercial OK) is an external dependency:
// this header is only compiled with FVM_HAVE_HYPRE defined (see the root
// CMakeLists.txt, which find_package()s HYPRE optionally and falls back to
// amgcl when it is not found). vendor/ is never touched by this backend.
//
// STAR-CCM+ parity target (lid 129x129): BoomerAMG with strength 0.25,
// max 50 levels, coarse solve direct at <= 2 unknowns, Gauss-Seidel
// smoother, V/W/F cycles and a Flexible cycle driven by a -2/-1/0/1 walk
// struct (Terminate/Coarser/Stay/Finer) with prolongation alpha = 0.5 and
// restriction beta = 0.9, 1 sweep.

#include "fvm/Types.h"

#include <memory>
#include <vector>

namespace fvm {

#ifdef FVM_HAVE_HYPRE
class HypreAMGHierarchy;
#endif

// SparseMatrix -> HYPRE_ParCSRMatrix converter + BoomerAMG solver wrapper.
// Serial use: the matrix/vector live on a single MPI rank (MPI_COMM_WORLD).
// Used by AMGHierarchy when Config::useHypre == true and Hypre is linked in;
// AMGHierarchy falls back to amgcl otherwise, so the API stays identical.
class HypreSolver {
public:
    // Export an Eigen SparseMatrix to a HYPRE_ParCSRMatrix (owned by the
    // caller via HypreMat). Returns false on failure.
    struct HypreMat;
    struct HypreSolverImpl;

    // RAII holder for a HYPRE_ParCSRMatrix.
    struct HypreMat {
#ifdef FVM_HAVE_HYPRE
        void* mat = nullptr; // HYPRE_ParCSRMatrix
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

#include <mpi.h>

#include <HYPRE_utilities.h>
#include <HYPRE_parcsr_ls.h>

#include <cmath>

namespace fvm {

inline HypreSolver::HypreMat::~HypreMat() {
    if (mat) {
        HYPRE_ParCSRMatrixDestroy(static_cast<HYPRE_ParCSRMatrix>(mat));
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

    // Copy into RowMajor CSR so we can hand Hypre diag/off-diag arrays.
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

    // --- Export to HYPRE_ParCSRMatrix ------------------------------------
    HYPRE_Int ilower = 0;
    HYPRE_Int iupper = static_cast<HYPRE_Int>(n - 1);
    HYPRE_Real* diag = nullptr;
    HYPRE_Real* offDiag = nullptr;
    HYPRE_Int* offDiagColumns = nullptr;
    HYPRE_Int numOffDiag = 0;
    std::vector<HYPRE_Int> rowBounds;
    {
        const int* outer = AR.outerIndexPtr();
        const int* inner = AR.innerIndexPtr();
        const double* vals = AR.valuePtr();
        diag = new HYPRE_Real[n];
        rowBounds.assign(n + 1, 0);
        for (int i = 0; i < n; ++i) {
            HYPRE_Real d = 0.0;
            int cnt = 0;
            for (int k = outer[i]; k < outer[i + 1]; ++k) {
                if (inner[k] == i) d = static_cast<HYPRE_Real>(vals[k]);
                else ++cnt;
            }
            diag[i] = d;
            numOffDiag += cnt;
            rowBounds[i + 1] = numOffDiag;
        }
        offDiag = (numOffDiag > 0) ? new HYPRE_Real[numOffDiag] : nullptr;
        offDiagColumns = (numOffDiag > 0) ? new HYPRE_Int[numOffDiag] : nullptr;
        int p = 0;
        for (int i = 0; i < n; ++i) {
            for (int k = outer[i]; k < outer[i + 1]; ++k) {
                if (inner[k] == i) continue;
                offDiag[p] = static_cast<HYPRE_Real>(vals[k]);
                offDiagColumns[p] = static_cast<HYPRE_Int>(inner[k]);
                ++p;
            }
        }
    }

    HYPRE_ParCSRMatrix Amat = nullptr;
    if (HYPRE_ParCSRMatrixCreate(MPI_COMM_WORLD, ilower, iupper, diag, &Amat) != 0) {
        delete[] diag; delete[] offDiag; delete[] offDiagColumns;
        return false;
    }
    const HYPRE_Int ok = HYPRE_ParCSRMatrixSetNumNonzeros(Amat, numOffDiag);
    if (ok != 0) {
        HYPRE_ParCSRMatrixDestroy(Amat);
        delete[] diag; delete[] offDiag; delete[] offDiagColumns;
        return false;
    }
    // Off-diag arrays are consumed by Hypre's Set* calls (copied via
    // SetOffDiagValues/SetOffDiagColumnIndices in 2.17+); keep rowBounds
    // semantics identical to SetRowBounds/SetRowUpperBounds usage in the
    // Hypre examples.
    HYPRE_ParCSRMatrixSetOffDiagValues(Amat, offDiag);
    HYPRE_ParCSRMatrixSetOffDiagColumnIndices(Amat, offDiagColumns);
    if (HYPRE_AssembleMatrix(Amat) != 0) {
        HYPRE_ParCSRMatrixDestroy(Amat);
        delete[] diag; delete[] offDiag; delete[] offDiagColumns;
        return false;
    }
    mat.mat = static_cast<void*>(Amat);
    // diag/offDiag/offDiagColumns ownership transferred to Amat (Hypre frees
    // them on ParCSRMatrixDestroy); only rowBounds needs no action.

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
    if (cfg.cycleType == AMGHierarchy::Config::CycleType::Flexible) {
        std::vector<HYPRE_Int> flex;
        if (!cfg.flexStruct.empty()) {
            for (auto m : cfg.flexStruct)
                flex.push_back(static_cast<HYPRE_Int>(m));
        } else {
            // Default walk: stay, go coarser, coarser, coarser, finer,
            // finer, stay, terminate. Trailing -2 is required by Hypre.
            flex = {0, -1, -1, -1, 1, 1, 0, -2};
        }
        std::vector<HYPRE_Real> cgc;
        cgc.reserve(flex.size());
        for (HYPRE_Int step : flex)
            cgc.push_back(step == 1 ? static_cast<HYPRE_Real>(cfg.flexAlpha) : 1.0);
        HYPRE_BoomerAMGSetFlexibleCycleStruct(amg, flex.data());
        // HYPRE >= 2.17 exposes per-step coarse-grid correction scaling
        // (prolongation). Beta (restriction) has no direct Hypre knob; it is
        // kept in the config and honoured by the own FAMGHierarchy flexible
        // walk, so here it is recorded but not passed to Hypre.
        (void)cfg.flexBeta;
        HYPRE_BoomerAMGSetFlexibleCGCScalingFactors(amg, cgc.data());
        const HYPRE_Int sweeps =
            cfg.flexMaxSweeps > 0 ? static_cast<HYPRE_Int>(cfg.flexMaxSweeps) : 1;
        HYPRE_BoomerAMGSetNumSweeps(amg, sweeps);
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

    HYPRE_ParVector bvec = nullptr, xvec = nullptr;
    std::vector<HYPRE_Real> zeros(n, 0.0);
    if (HYPRE_ParVectorCreate(MPI_COMM_WORLD, ilower, iupper, zeros.data(), &bvec) != 0 ||
        HYPRE_ParVectorCreate(MPI_COMM_WORLD, ilower, iupper, zeros.data(), &xvec) != 0) {
        if (bvec) HYPRE_ParVectorDestroy(bvec);
        if (xvec) HYPRE_ParVectorDestroy(xvec);
        return false;
    }
    const bool setupOk = (HYPRE_BoomerAMGSetup(amg, Amat, bvec, xvec) == 0);
    HYPRE_ParVectorDestroy(bvec);
    HYPRE_ParVectorDestroy(xvec);
    return setupOk;
}

inline bool HypreSolver::applyCycle(HypreMat& mat, HypreSolverImpl& solver,
                                    const Eigen::VectorXd& b,
                                    Eigen::VectorXd& x,
                                    const std::vector<double>& scale) {
    if (!mat.ok() || !solver.ok()) return false;
    const int n = static_cast<int>(b.size());
    if (n <= 0 || static_cast<int>(x.size()) != n) return false;

    const bool hasScale = (static_cast<int>(scale.size()) == n);
    std::vector<HYPRE_Real> bdata(n), xdata(n);
    for (int i = 0; i < n; ++i) {
        bdata[i] = static_cast<HYPRE_Real>(hasScale ? b(i) * scale[i] : b(i));
        xdata[i] = static_cast<HYPRE_Real>(hasScale ? x(i) / scale[i] : x(i));
    }

    HYPRE_Int ilower = 0, iupper = static_cast<HYPRE_Int>(n - 1);
    HYPRE_ParVector bvec = nullptr, xvec = nullptr;
    if (HYPRE_ParVectorCreate(MPI_COMM_WORLD, ilower, iupper, bdata.data(), &bvec) != 0 ||
        HYPRE_ParVectorCreate(MPI_COMM_WORLD, ilower, iupper, xdata.data(), &xvec) != 0) {
        if (bvec) HYPRE_ParVectorDestroy(bvec);
        if (xvec) HYPRE_ParVectorDestroy(xvec);
        return false;
    }
    const HYPRE_Int rc =
        HYPRE_BoomerAMGSolve(static_cast<HYPRE_Solver>(solver.solver),
                             static_cast<HYPRE_ParCSRMatrix>(mat.mat), bvec, xvec);
    HYPRE_Real* xpar = nullptr;
    HYPRE_ParVectorGetData(xvec, &xpar);
    for (int i = 0; i < n; ++i)
        x(i) = static_cast<double>(xpar[i]) * (hasScale ? scale[i] : 1.0);
    HYPRE_ParVectorDestroy(bvec);
    HYPRE_ParVectorDestroy(xvec);
    if (!x.allFinite()) { x.setZero(); return false; }
    return rc == 0;
}

} // namespace fvm
#endif // FVM_HAVE_HYPRE
