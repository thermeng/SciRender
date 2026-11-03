#pragma once

#include "mesh/Mesh.h"
#include "field/Field.h"
#include "field/VectorField.h"
#include "fvm/Types.h"
#include "fvm/SIMPLETypes.h"
#include "fvm/Gradient.h"
#include "fvm/RhieChow.h"
#include "fvm/AMG.h"
#include <Eigen/Sparse>
#include <Eigen/IterativeLinearSolvers>
#include <atomic>
#include <array>
#include <vector>
#include <memory>
#include <cstddef>
#include <utility>

namespace fvm {

template<std::size_t Dim>
class PressureCorrection {
public:
    struct Config {
        // Diagnostic Rhie-Chow switch (the residual reports the correction).
        bool rhieChow = true;
        // Assemble the pressure equation from the compact face-normal
        // Laplacian and use the Rhie-Chow face flux on its right-hand side.
        // This is the collocated-grid formulation: the pressure matrix is the
        // two-point operator div(d_f (p_N-p_P)/d), the Rhie-Chow term removes
        // the checkerboard mode, and the velocity correction remains
        // u = u* - d grad(p'). Enabled for PISO; the legacy composed
        // least-squares operator (compactRhieChow = false) is kept for the
        // steady path and its exact-cancellation contracts.
        bool compactRhieChow = false;
        std::size_t pressureRefCell = SIZE_MAX;
        bool useAMG = true;
        bool useAMGPreconditioner = true;
        // Number of cells below which the solve falls back to CG+IC (or
        // BiCGSTAB+ILU). 0 means "always use AMG", which is what 
        // does: it never switches the pressure equation to a different
        // preconditioner based on size. Any non-zero value keeps the legacy
        // small-mesh shortcut for callers that want it.
        std::size_t amgThreshold = 0;
        // Relative tolerance for the AMG-preconditioned Krylov solve. 0 keeps
        // the legacy mesh-scaled 1e-4 * mean(cell volume) rule. A positive
        // value is used verbatim as |r|/|b|;  stops at ~0.1.
        double solveTolerance = 0.1;
        int amgPreconditionerCycles = 1;
        // How many times the pressure matrix may change its VALUES before the
        // AMG hierarchy is rebuilt. The coarse levels depend on the matrix
        // through the Galerkin product, so a value change makes the hierarchy
        // stale - but rebuilding it on every outer iteration costs more than
        // the solve it accelerates. The matrix moves by a few percent per
        // iteration, so a hierarchy a few iterations old is still a good
        // preconditioner.
        std::size_t amgRebuildInterval = 10;
        AMGHierarchy::Config amgConfig = []{
            AMGHierarchy::Config c;
            c.strengthThreshold = 0.16;
            c.truncThreshold = 0.2;
            c.preSweeps = 1;
            c.postSweeps = 2;
            c.maxIterations = 100;
            c.scaling = AMGHierarchy::Config::ProportionalScaling::Automatic;
            c.tolerance = 1e-8;
            c.verbosity = AMGHierarchy::Config::Verbosity::High;
            return c;
        }();
    };

    struct PressureSolverStats {
        std::size_t solves = 0;
        std::size_t amgSolves = 0;
        std::size_t iluSolves = 0;
        std::size_t amgBuilds = 0;
        std::size_t amgRejected = 0;
        std::size_t krylovIters = 0;
        std::size_t amgKrylovIters = 0;
        double iluSeconds = 0.0;
        double amgSeconds = 0.0;
        double buildSeconds = 0.0;
    };

    PressureCorrection(const Mesh<Dim>& mesh, const Config& config);
    PressureCorrection(const Mesh<Dim>& mesh);
    void setConfig(const Config& cfg) { config_ = cfg; }
    const std::vector<char>& pressureMask() const { return pMask_; }
    const std::array<Field<Dim>, Dim>& pressureGradient() const { return pressureGrad_; }

    // Recompute the least-squares pressure gradient. The compact Rhie-Chow
    // face flux used by the residual diagnostics needs the gradient of the
    // CURRENT pressure; the one left behind by the last solve() is the
    // gradient of the pre-correction pressure.
    void refreshPressureGradient(const Field<Dim>& pressure);

    // True when the assembled pressure operator contains the Rhie-Chow face
    // flux. Residual diagnostics must add the same term only in that mode:
    // the legacy composed operator never sees it, so reporting it would measure
    // a mass imbalance the pressure solve neither created nor can remove.
    bool usesCompactRhieChow() const { return config_.compactRhieChow; }

    // The number of cells that have no pressure equation at all (no interior
    // face: a detached cell). Such a cell is excluded from the pressure
    // unknowns and its mass balance can never be corrected, so the count is
    // reported - otherwise the run silently never converges.
    std::size_t isolatedCellCount() const { return isolatedCellCount_; }

    // Diagnostics of the last linear solve: the iteration count and the
    // residual the solver ended with.
    std::size_t lastSolveIterations() const { return lastSolveIterations_; }
    double lastSolveResidual() const { return lastSolveResidual_; }
    // True when the last solve produced a non-finite p'. Such a solve is a
    // hard failure, not a slightly inaccurate one: the entries are zeroed to
    // keep the field usable, but the caller must abort rather than continue
    // from a correction that was never computed.
    bool lastSolveProducedNonFinite() const { return lastSolveProducedNonFinite_; }
    const PressureSolverStats& pressureStats() const { return pressureStats_; }
    void resetPressureStats() { pressureStats_ = PressureSolverStats{}; }
    // AMG hierarchy diagnostics (levels actually built and their sizes), for
    // comparing against a  "Level / Size" table.
    bool amgBuilt() const { return amgBuilt_; }
    int amgLevels() const { return amg_.numLevels(); }
    const std::vector<int>& amgLevelSizes() const { return amg_.levelSizes(); }
    // The assembled system of the last solve (diagnostics).
    const SparseMatrix& lastMatrix() const { return A_; }
    const Eigen::VectorXd& lastRhs() const { return bVec_; }

    void solve(
        const VectorField<Dim>& uStar,
        const std::array<std::vector<double>, Dim>& dCoeff,
        const Field<Dim>& pressure,
        Field<Dim>& pPrime,
        const std::vector<std::pair<std::size_t, double>>& pressureOutletCells,
        const std::vector<char>& isPressureNeumann,
        const std::vector<char>* isPressureDirichletFace = nullptr,
        const std::vector<std::pair<std::size_t, double>>* pressureOutletFaces = nullptr,
        const std::vector<FaceVelocityBC>* faceBC = nullptr,
        const std::vector<char>* faceHasBC = nullptr,
        // Velocity Dirichlet data (cell-level). When a cell carries a prescribed
        // velocity the momentum assembly uses that prescribed value for the WHOLE
        // face; the divergence here must use the very same face velocity, or the
        // pressure correction balances a mass imbalance that the momentum
        // equation never had.
        const std::vector<std::vector<char>>* velDirichletMask = nullptr,
        const std::vector<std::vector<double>>* velDirichletVals = nullptr,
        const std::atomic<bool>* cancel = nullptr);

private:
    const Mesh<Dim>& mesh_;
    Config config_;
    // The least-squares gradient of the pressure field. In compact Rhie-Chow
    // mode it supplies the face-interpolated gradient that the Rhie-Chow
    // correction subtracts; in legacy mode it is also the operator whose
    // divergence the pressure matrix is assembled from. It is the same
    // operator the velocity correction applies.
    std::unique_ptr<Gradient<Dim>> gradientCalc_;
    std::array<Field<Dim>, Dim> pressureGrad_;
    // The per-cell least-squares gradient as row coefficients
    // (gradientRows_[d][cell] = (column, coefficient) pairs), built once per
    // PressureCorrection object: the pressure matrix is assembled from them.
    std::array<std::vector<std::vector<std::pair<std::size_t, double>>>, Dim> gradientRows_;
    bool gradientRowsBuilt_ = false;

    // Reusable buffers to avoid per-iteration allocation
    mutable std::vector<Triplet> triplets_;
    mutable Eigen::VectorXd bVec_;
    mutable std::vector<char> pMask_;
    mutable std::vector<double> pVals_;
    mutable std::vector<char> isPressureOutlet_;
    mutable Field<Dim> divU_;
    mutable std::vector<std::vector<std::pair<std::size_t, double>>> rowAcc_;

    // The number of cells that have no pressure equation at all (no interior
    // face: a detached cell). Such a cell is excluded from the pressure
    // unknowns and its mass balance can never be corrected, so the count is
    // reported - otherwise the run silently never converges.
    std::size_t isolatedCellCount_ = 0;
    // Diagnostics of the last linear solve.
    std::size_t lastSolveIterations_ = 0;
    double lastSolveResidual_ = 0.0;
    bool lastSolveProducedNonFinite_ = false;
    mutable PressureSolverStats pressureStats_{};

    Eigen::ConjugateGradient<SparseMatrix, Eigen::Lower|Eigen::Upper,
                             Eigen::IncompleteCholesky<double>> cgSolver_;
    //  solves the pressure equation with PCG preconditioned by AMG
    // ("Preconditioned Conjugate-Gradient solve of SPD-system"). That is valid
    // for the compact Rhie-Chow operator, which is symmetric positive definite.
    Eigen::ConjugateGradient<SparseMatrix, Eigen::Lower|Eigen::Upper,
                             AMGPreconditioner> cgAmgSolver_;
    Eigen::BiCGSTAB<SparseMatrix, AMGPreconditioner> bicgSolver_;
    // GMRES is not part of Eigen's supported iterative solvers; the AMG
    // acceleration "GMRES" uses fvm::gmresRestarted (fvm/Gmres.h) instead of a
    // member here.
    Eigen::BiCGSTAB<SparseMatrix, Eigen::IncompleteLUT<double>> bicgFallbackSolver_;
    bool pPatternAnalyzed_ = false;
    // The fallback ILU/CG solvers are analysed separately from the AMG Krylov
    // solvers: an AMG solve can succeed without ever touching them, and a
    // later fallback must still call analyzePattern() before factorize().
    bool fallbackPatternAnalyzed_ = false;
    std::size_t fallbackPatternHash_ = 0;
    mutable std::vector<char> prevPMask_;
    mutable std::size_t prevNonZeroPattern_ = 0;
    // The compact and legacy operators have different stencils. A transition
    // between them (e.g. when PISO forces Rhie-Chow after construction) must
    // re-analyse the preconditioner, not just overwrite values.
    bool prevCompact_ = false;

    AMGHierarchy amg_;
    AMGPreconditioner amgPrec_;
    bool amgBuilt_ = false;
    std::size_t amgPatternHash_ = 0;
    // Number of value changes of the pressure matrix since the hierarchy was
    // last built (see Config::amgRebuildInterval).
    std::size_t amgValueUpdates_ = 0;
    mutable bool amgPathLogged_ = false;

    mutable SparseMatrix A_;
    mutable bool aPatternBuilt_ = false;
    mutable std::size_t aPatternHash_ = 0;
    // O(nnz) value update — avoids O(nnz log nnz) sort + O(nnz) hash per iter
    mutable std::vector<int> aTripletToValuePos_;
    mutable bool aNeedPosRebuild_ = false;
};

extern template class PressureCorrection<2>;
extern template class PressureCorrection<3>;

} // namespace fvm
